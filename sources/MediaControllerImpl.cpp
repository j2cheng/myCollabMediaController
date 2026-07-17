#include "MediaControllerImpl.h"
#include "grpcstreamout/StreamoutGrpcClient.h"
#include "grpcstreamin/StreamInGrpcClient.h"

#define LOG_TAG "MediaController"
#include "Logging.h"

static const char* statusToString(MediaController::StreamStatus s) {
    switch (s) {
        case MediaController::StreamStatus::Idle:      return "Idle";
        case MediaController::StreamStatus::Created:   return "Created";
        case MediaController::StreamStatus::Connected: return "Connected";
        case MediaController::StreamStatus::Listening: return "Listening";
        case MediaController::StreamStatus::Active:    return "Active";
        case MediaController::StreamStatus::Stopping:  return "Stopping";
        case MediaController::StreamStatus::Stopped:   return "Stopped";
        case MediaController::StreamStatus::Error:     return "Error";
        default:                                       return "Unknown";
    }
}

static const char* errorToString(MediaController::StreamError e) {
    switch (e) {
        case MediaController::StreamError::NoError:              return "NoError";
        case MediaController::StreamError::InvalidHandle:        return "InvalidHandle";
        case MediaController::StreamError::InvalidConfiguration: return "InvalidConfiguration";
        case MediaController::StreamError::ConnectionFailed:     return "ConnectionFailed";
        case MediaController::StreamError::ConnectionTimeout:    return "ConnectionTimeout";
        case MediaController::StreamError::AlreadyStarted:       return "AlreadyStarted";
        case MediaController::StreamError::NotStarted:           return "NotStarted";
        case MediaController::StreamError::StartupFailed:        return "StartupFailed";
        case MediaController::StreamError::StopFailed:           return "StopFailed";
        case MediaController::StreamError::NetworkError:         return "NetworkError";
        case MediaController::StreamError::PortInUse:            return "PortInUse";
        case MediaController::StreamError::InvalidStreamType:    return "InvalidStreamType";
        case MediaController::StreamError::ResourceExhausted:    return "ResourceExhausted";
        case MediaController::StreamError::Unknown:              return "Unknown";
        default:                                                 return "Unknown";
    }
}

// --- start of MediaController ---
MediaController::MediaController()
    : impl_(std::make_unique<MediaControllerImpl>()) {
    initLogging();
#ifdef NDEBUG
    constexpr const char* kBuildFlavor = "Release";
#else
    constexpr const char* kBuildFlavor = "Debug";
#endif
    LogInfo(logCategory, "MediaControllerImpl created (%s build, %s %s)",
            kBuildFlavor, __DATE__, __TIME__);
}

MediaController::~MediaController() = default;

void MediaController::setGlobalCallbacks(const GlobalCallbacks& callbacks) {
    impl_->setGlobalCallbacks(callbacks);
}

MediaController::StreamHandle MediaController::create(StreamType type) {
    return impl_->create(type);
}

void MediaController::start(StreamHandle handle, const StreamConfiguration& configuration) {
    impl_->start(handle, configuration);
}

void MediaController::stop(StreamHandle handle) {
    impl_->stop(handle);
}

MediaController::StreamStatus MediaController::getStatus(StreamHandle handle) const {
    return impl_->getStatus(handle);
}

MediaController::StreamError MediaController::getLastError(StreamHandle handle) const {
    return impl_->getLastError(handle);
}

bool MediaController::isValidHandle(StreamHandle handle) const {
    return impl_->isValidHandle(handle);
}
// --- end of MediaController ---



// --- MediaControllerImpl ---

void MediaControllerImpl::setGlobalCallbacks(const GlobalCallbacks& callbacks) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.onStreamStatus = [callbacks](StreamHandle handle, StreamStatus status) {
        initLogging();
        LogInfo(logCategory, "onStreamStatus(handle=%d, status=%s)", handle, statusToString(status));
        if (callbacks.onStreamStatus) callbacks.onStreamStatus(handle, status);
    };
    callbacks_.onStreamError = [callbacks](StreamHandle handle, StreamError error) {
        initLogging();
        LogWarning(logCategory, "onStreamError(handle=%d, error=%s)", handle, errorToString(error));
        if (callbacks.onStreamError) callbacks.onStreamError(handle, error);
    };
}

void MediaControllerImpl::setGrpcClient(std::unique_ptr<StreamoutGrpcClientInterface> client) {
    // Take ownership of the existing client under the lock, swap, then let the
    // old one destruct OUTSIDE the lock. The old client's destructor calls
    // disconnect(), which joins watchThread_; that thread's status callback
    // re-enters MediaControllerImpl and tries to take mutex_ — destructing
    // under the lock would deadlock.
    std::shared_ptr<StreamoutGrpcClientInterface> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old = std::move(grpcClient_);
        grpcClient_ = std::move(client);
    }
    // `old` releases its ref here; if it was the last, dtor runs outside the lock.
}

void MediaControllerImpl::setGrpcTarget(const std::string& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    grpcTarget_ = target;
}

void MediaControllerImpl::setStreamInGrpcClient(std::unique_ptr<StreamInGrpcClientInterface> client) {
    // Same rationale as setGrpcClient: destruct the old client outside the lock.
    std::shared_ptr<StreamInGrpcClientInterface> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old = std::move(streamInGrpcClient_);
        streamInGrpcClient_ = std::move(client);
    }
}

void MediaControllerImpl::setStreamInGrpcTarget(const std::string& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    streamInGrpcTarget_ = target;
}

void MediaControllerImpl::setDeviceId(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    deviceId_ = id;
}

void MediaControllerImpl::setDeviceIdProvider(std::function<uint32_t()> provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    deviceIdProvider_ = std::move(provider);
}

void MediaControllerImpl::deinit() {
    // Disconnect both gRPC clients — stops reconnect thread, state watcher,
    // watch stream (StreamOut) and any in-flight RPCs (StreamIn).
    // Copy both shared_ptrs under lock, then call disconnect() OUTSIDE the lock
    // (disconnect() may block on thread joins; we don't want to hold mutex_
    // during that, and a watch-thread callback re-acquires mutex_).
    // Holding a shared_ptr copy also keeps each client alive for the duration
    // of its disconnect() call regardless of what other threads do.
    std::shared_ptr<StreamoutGrpcClientInterface> outClient;
    std::shared_ptr<StreamInGrpcClientInterface>  inClient;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outClient = grpcClient_;
        inClient  = streamInGrpcClient_;
    }
    if (outClient) {
        outClient->disconnect();
    }
    if (inClient) {
        inClient->disconnect();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    streams_.clear();
    nextHandle_ = 1;
    deviceId_ = 0;
    LogInfo(logCategory, "deinit complete");
}

MediaController::StreamHandle MediaControllerImpl::create(StreamType type) {
    GlobalCallbacks cb;
    bool invalidType   = false;
    bool duplicateType = false;

    // --- Phase 1: short critical section --- validate, dedupe, snapshot inputs.
    std::string                                   targetSnap;
    uint32_t                                      deviceIdSnap = 0;
    std::function<uint32_t()>                     deviceIdProviderSnap;
    std::shared_ptr<StreamoutGrpcClientInterface> existingOut;
    std::shared_ptr<StreamInGrpcClientInterface>  existingIn;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (type != StreamType::StreamOut && type != StreamType::StreamIn) {
            invalidType = true;
            cb = callbacks_;
        }

        // Only one handle per type is supported today — the lower-layer
        // gRPC server does not distinguish between concurrent streams of the
        // same type. See MediaControllerImpl.md § "One-handle-per-type
        // limitation".
        if (!invalidType) {
            for (const auto& kv : streams_) {
                if (kv.second.type == type) {
                    duplicateType = true;
                    cb = callbacks_;
                    LogWarning(logCategory,
                               "create(%s) rejected: a handle of this type "
                               "already exists (handle=%d)",
                               type == StreamType::StreamOut ? "StreamOut" : "StreamIn",
                               kv.first);
                    break;
                }
            }
        }

        if (!invalidType && !duplicateType) {
            if (type == StreamType::StreamOut) {
                targetSnap  = grpcTarget_;
                existingOut = grpcClient_;
            } else {
                targetSnap = streamInGrpcTarget_;
                existingIn = streamInGrpcClient_;
            }
            deviceIdSnap         = deviceId_;
            deviceIdProviderSnap = deviceIdProvider_;
        }
    }

    if (invalidType) {
        if (cb.onStreamError) cb.onStreamError(0, StreamError::InvalidStreamType);
        return -1;
    }
    if (duplicateType) {
        if (cb.onStreamError) cb.onStreamError(0, StreamError::ResourceExhausted);
        return -1;
    }

    // --- Phase 2: OUTSIDE the lock --- slow connect / configuration.
    // We work on a local shared_ptr only; nothing on `this` is touched.
    std::shared_ptr<StreamoutGrpcClientInterface> newOut;
    std::shared_ptr<StreamInGrpcClientInterface>  newIn;
    bool connectFailed = false;

    if (type == StreamType::StreamOut) {
        auto out = existingOut ? existingOut
                                : std::shared_ptr<StreamoutGrpcClientInterface>(
                                      std::make_shared<StreamoutGrpcClient>());

        // Register status callback BEFORE connect so that if the initial
        // connect fails and the background reconnect later succeeds, the
        // reconnect-driven startWatching() will have a callback to invoke.
        out->setStatusCallback(
            [this](int32_t streamId, int32_t statusCode, const std::string& statusInfo) {
                LogInfo(logCategory, "WatchStatus: stream_id=%d status_code=%d status_info=\"%s\"",
                        streamId, statusCode, statusInfo.c_str());

                GlobalCallbacks innerCb;
                std::vector<std::pair<StreamHandle, StreamStatus>> notifications;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // Map gRPC StreamStatus to MediaController::StreamStatus
                    StreamStatus newStatus = StreamStatus::Idle;
                    switch (statusCode) {
                        case 0: newStatus = StreamStatus::Created;   break;  // STREAM_STATUS_UNSPECIFIED
                        case 1: newStatus = StreamStatus::Listening; break;  // STREAM_STATUS_READY
                        case 2: newStatus = StreamStatus::Active;    break;  // STREAM_STATUS_CLIENT_CONNECTED
                        case 3: newStatus = StreamStatus::Stopped;   break;  // STREAM_STATUS_STOPPED
                        default: newStatus = StreamStatus::Error;    break;
                    }

                    // This callback is registered only on the StreamOut
                    // gRPC client, so its status updates are only
                    // meaningful for StreamOut handles. Skip StreamIn
                    // entries so they aren't clobbered.
                    // (streamId is always 0 today, i.e. "all" — once
                    // the lower layer routes per-stream, switch to
                    // looking up by streamId.)
                    for (auto& [handle, info] : streams_) {
                        if (info.type != StreamType::StreamOut) continue;
                        info.status = newStatus;
                        notifications.emplace_back(handle, newStatus);
                    }
                    innerCb = callbacks_;
                }

                if (innerCb.onStreamStatus) {
                    for (auto& [handle, status] : notifications) {
                        innerCb.onStreamStatus(handle, status);
                    }
                }
            });

        if (!out->isConnected()) {
            if (!out->connect(targetSnap)) {
                LogError(logCategory, "gRPC streamout connect failed — reconnect loop running in background");
                connectFailed = true;
                // StreamoutGrpcClient::connect() already kicked its own
                // reconnect loop. We just need to keep `out` alive (handled
                // below by always assigning it to newOut).
            } else {
                uint32_t id = deviceIdSnap;
                if (id == 0 && deviceIdProviderSnap) id = deviceIdProviderSnap();
                if (id != 0) out->setProductId(id);

                // Start the WatchStatus stream at connection time so we never miss status updates
                out->startWatching();
            }
        }
        // Keep the client alive even on connect failure: its background reconnect
        // loop is the only thing that will recover, and the only shared_ptr to it
        // is this local `out`. Without this, returning from create() drops the
        // client and kills the reconnect loop immediately.
        if (out != existingOut) newOut = out;
    } else {
        auto in = existingIn ? existingIn
                              : std::shared_ptr<StreamInGrpcClientInterface>(
                                    std::make_shared<StreamInGrpcClient>());
        if (!in->isConnected()) {
            if (!in->connect(targetSnap)) {
                LogError(logCategory, "gRPC streamin connect failed (target=%s)", targetSnap.c_str());
                connectFailed = true;
            } else {
                LogInfo(logCategory, "gRPC streamin connected (target=%s)", targetSnap.c_str());
            }
        }
        // Same reasoning as the StreamOut branch: keep the client alive on
        // failure so any background recovery has an owner.
        if (in != existingIn) newIn = in;
    }

    // --- Phase 3: short critical section --- publish + allocate handle.
    // create() always returns a positive handle except for the explicit
    // rejections above (invalid type, duplicate type). A connect failure
    // still allocates a handle; its initial status is Error/NetworkError
    // and the background reconnect loop will drive recovery.
    StreamHandle handle = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (newOut) grpcClient_         = newOut;
        if (newIn)  streamInGrpcClient_ = newIn;
        // Cache deviceId if the provider produced one in phase 2.
        if (deviceId_ == 0 && deviceIdSnap != 0) deviceId_ = deviceIdSnap;

        handle = nextHandle_++;
        StreamStatus initialStatus = connectFailed ? StreamStatus::Error : StreamStatus::Created;
        StreamError  initialError  = connectFailed ? StreamError::NetworkError : StreamError::NoError;
        streams_[handle] = {type, initialStatus, initialError, {}};
        cb = callbacks_;
        LogInfo(logCategory, "created handle=%d type=%s status=%s",
                handle,
                type == StreamType::StreamOut ? "StreamOut" : "StreamIn",
                statusToString(initialStatus));
    }

    if (connectFailed && cb.onStreamError) 
        cb.onStreamError(handle, StreamError::NetworkError);
    
    // Notify the caller that the handle was created successfully. This is
    // always invoked even if the initial connect failed
    if (cb.onStreamStatus) cb.onStreamStatus(handle, StreamStatus::Created);
    
    return handle;
}

static void splitUrl(const std::string &url, std::string &address, std::string &path) {
    // Simple parsing: assume url is in the form "rtsp://address:port/path"
    const std::string prefix = "rtsp://";
    if (url.compare(0, prefix.size(), prefix) == 0) {
        std::string remainder = url.substr(prefix.size());
        size_t slashPos = remainder.find('/');
        if (slashPos != std::string::npos) {
            address = remainder.substr(0, slashPos);
            path = remainder.substr(slashPos);
        } else {
            address = remainder;
            path = "/";
        }
    } else {
        address = url;
        path = "/";
    }
}

void MediaControllerImpl::start(StreamHandle handle, const StreamConfiguration& configuration) {
    GlobalCallbacks cb;
    MediaController::StreamError errorToReport = StreamError::NoError;
    std::shared_ptr<StreamoutGrpcClientInterface> outClient;
    std::shared_ptr<StreamInGrpcClientInterface>  inClient;
    StreamType streamType = StreamType::StreamOut;  // overwritten below; value irrelevant on error

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(handle);
        if (it == streams_.end()) {
            errorToReport = StreamError::InvalidHandle;
            cb = callbacks_;
        } else {
            auto& info = it->second;
            // Pass-through: do not gate on current status. The lower layer
            // owns the state machine; we just forward the call.
            info.config = configuration;
            streamType = info.type;
            if (streamType == StreamType::StreamOut) {
                outClient = grpcClient_;
            } else {
                inClient = streamInGrpcClient_;
            }
            cb = callbacks_;

            LogInfo(logCategory, "start handle=%d type=%s url=%s port=%u status=%s tlscert=%zu tlskey=%zu tlsca=%zu",
                    handle,
                    streamType == StreamType::StreamOut ? "StreamOut" : "StreamIn",
                    configuration.url.c_str(),
                    static_cast<unsigned>(configuration.port),
                    statusToString(info.status),
                    configuration.set_tlscert.size(),
                    configuration.set_tlskey.size(),
                    configuration.set_tlsca.size());
        }
    }

    // Invoke error callback outside lock
    if (errorToReport != StreamError::NoError) {
        if (cb.onStreamError) {
            cb.onStreamError(handle, errorToReport);
        }
        return;
    }

    // Helper: mark stream as failed under lock
    auto markStartupFailed = [&]() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(handle);
        if (it != streams_.end()) {
            it->second.status = StreamStatus::Error;
            it->second.lastError = StreamError::StartupFailed;
        }
    };

    // Dispatch to the correct gRPC client based on stream type
    if (streamType == StreamType::StreamOut && outClient) {
        // Push TLS material to the server BEFORE StartStream. Each field is
        // checked and sent independently via the StreamoutServerDebug RPC,
        // with the exact prefix strings the server expects.
        if (!configuration.set_tlscert.empty()) {
            LogInfo(logCategory, "pushing TLS cert to server (%zu bytes)",
                    configuration.set_tlscert.size());
            if (!outClient->StreamoutServerDebug(
                    "STREAMOUT_CAM2 SET_CRESTRON_CERTS " + configuration.set_tlscert)) {
                LogError(logCategory, "failed to push TLS cert to server");
                markStartupFailed();
                if (cb.onStreamError) cb.onStreamError(handle, StreamError::StartupFailed);
                return;
            }
        }

        if (!configuration.set_tlskey.empty()) {
            LogInfo(logCategory, "pushing TLS key to server (%zu bytes)",
                    configuration.set_tlskey.size());
            if (!outClient->StreamoutServerDebug(
                    "STREAMOUT_CAM2 SET_CRESTRON_KEY " + configuration.set_tlskey)) {
                LogError(logCategory, "failed to push TLS key to server");
                markStartupFailed();
                if (cb.onStreamError) cb.onStreamError(handle, StreamError::StartupFailed);
                return;
            }
        }

        if (!configuration.set_tlsca.empty()) {
            LogInfo(logCategory, "pushing TLS CA to server (%zu bytes)",
                    configuration.set_tlsca.size());
            if (!outClient->StreamoutServerDebug(
                    "STREAMOUT_CAM2 SET_CLIENT_CA " + configuration.set_tlsca)) {
                LogError(logCategory, "failed to push TLS CA to server");
                markStartupFailed();
                if (cb.onStreamError) cb.onStreamError(handle, StreamError::StartupFailed);
                return;
            }
        }

        // Push the paired-device list. This is a full replacement; sending an
        // empty vector tells the server to clear its set. Done right before
        // setPort/startStream so the server has everything it needs by the
        // time the stream comes up.
        {
            std::vector<PairedDeviceEntry> entries;
            entries.reserve(configuration.pairedDevices.size());
            for (const auto& d : configuration.pairedDevices) {
                entries.push_back({d.DeviceId, d.IPAddress, d.MACAddress});
            }
            LogInfo(logCategory, "pushing paired devices to server (count=%zu)",
                    entries.size());
            if (!outClient->setPairedDevices(entries)) {
                LogError(logCategory, "failed to push paired devices to server");
                markStartupFailed();
                if (cb.onStreamError) cb.onStreamError(handle, StreamError::StartupFailed);
                return;
            }
        }

        if (!outClient->setPort(std::to_string(configuration.port))) {
            markStartupFailed();
            if (cb.onStreamError) cb.onStreamError(handle, StreamError::StartupFailed);
            return;
        }

        //Note: startStream takes arguments, it is used in streamout apk for stream index.
        //      for now, we just set to 0.
        //      Ideally, it should be the same as 'handle'.
        if (!outClient->startStream(0)) {
            markStartupFailed();
            if (cb.onStreamError) cb.onStreamError(handle, StreamError::StartupFailed);
            return;
        }
    } else if (streamType == StreamType::StreamIn && inClient) {
        // StreamIn proto carries address/port/path in the StartStreamRequest itself.
        // StreamConfiguration has no `path` field yet — pass empty for now.
        std::string path;
        std::string address;
        splitUrl(configuration.url, address, path);

        LogInfo(logCategory, "StreamIn start: address=%s port=%u path=%s",
                address.c_str(), static_cast<unsigned>(configuration.port), path.c_str());
        if (!inClient->startStream(address,
                                   static_cast<uint32_t>(configuration.port),
                                   path)) {
            markStartupFailed();
            if (cb.onStreamError) cb.onStreamError(handle, StreamError::StartupFailed);
            return;
        }
    }
    // Status will be updated when lower layer reports back via WatchStatus (StreamOut only).
}

void MediaControllerImpl::stop(StreamHandle handle) {
    GlobalCallbacks cb;
    std::shared_ptr<StreamoutGrpcClientInterface> outClient;
    std::shared_ptr<StreamInGrpcClientInterface>  inClient;
    StreamType streamType = StreamType::StreamOut;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(handle);
        if (it == streams_.end()) {
            return;  // no-op for invalid handles per interface doc
        }

        auto& info = it->second;
        // Pass-through: do not gate on current status. The lower layer owns
        // the state machine; we just forward the call and record "Stopping".
        info.status = StreamStatus::Stopping;
        streamType = info.type;
        if (streamType == StreamType::StreamOut) {
            outClient = grpcClient_;
        } else {
            inClient = streamInGrpcClient_;
        }
        cb = callbacks_;

        LogInfo(logCategory, "stop handle=%d type=%s status=Stopping",
                handle, streamType == StreamType::StreamOut ? "StreamOut" : "StreamIn");
    }

    // Fire Stopping callback outside lock
    if (cb.onStreamStatus) {
        cb.onStreamStatus(handle, StreamStatus::Stopping);
    }

    // Helper: mark stream as failed under lock
    auto markStopFailed = [&]() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(handle);
        if (it != streams_.end()) {
            it->second.status = StreamStatus::Error;
            it->second.lastError = StreamError::StopFailed;
        }
    };

    // Dispatch to the correct gRPC client based on stream type
    if (streamType == StreamType::StreamOut && outClient) {
        if (!outClient->stopStream(0)) {
            markStopFailed();
            if (cb.onStreamError) cb.onStreamError(handle, StreamError::StopFailed);
            return;
        }
    } else if (streamType == StreamType::StreamIn && inClient) {
        if (!inClient->stopStream()) {
            markStopFailed();
            if (cb.onStreamError) cb.onStreamError(handle, StreamError::StopFailed);
            return;
        }
    }
    // Stopped status will come back asynchronously from lower layer streamout apk.
}

MediaController::StreamStatus MediaControllerImpl::getStatus(StreamHandle handle) const {
    std::shared_ptr<StreamInGrpcClientInterface> inClient;
    StreamType streamType = StreamType::StreamOut;
    StreamStatus cached = StreamStatus::Idle;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(handle);
        if (it == streams_.end()) {
            return StreamStatus::Idle;
        }
        streamType = it->second.type;
        cached = it->second.status;
        if (streamType == StreamType::StreamIn) {
            inClient = streamInGrpcClient_;
        }
    }

    // StreamOut: status is pushed via the WatchStatus server-streaming RPC,
    // so the cached value is authoritative — just return it.
    if (streamType == StreamType::StreamOut || !inClient) {
        return cached;
    }

    // StreamIn: no server-streaming watch is defined in the proto, so we poll
    // via the unary status() RPC. The proto's Status message is currently empty,
    // so a successful reply only means "server is alive" — we update state only
    // on failure (RPC error or Error reply), demoting to Error / NetworkError.
    if (!inClient->status()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(handle);
        if (it != streams_.end()) {
            it->second.status = StreamStatus::Error;
            it->second.lastError = StreamError::NetworkError;
            cached = it->second.status;
        }
    }
    return cached;
}

MediaController::StreamError MediaControllerImpl::getLastError(StreamHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(handle);
    if (it == streams_.end()) {
        return StreamError::InvalidHandle;
    }
    return it->second.lastError;
}

bool MediaControllerImpl::isValidHandle(StreamHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_.find(handle) != streams_.end();
}
