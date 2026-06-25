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
    : impl_(std::make_unique<MediaControllerImpl>()) {}

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
        if (!out->isConnected()) {
            if (!out->connect(targetSnap)) {
                LogError(logCategory, "gRPC streamout connect failed");
                connectFailed = true;
            } else {
                uint32_t id = deviceIdSnap;
                if (id == 0 && deviceIdProviderSnap) id = deviceIdProviderSnap();
                if (id != 0) out->setProductId(id);

                // Register status callback to receive StreamoutStatusResponse from server
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

                // Start the WatchStatus stream at connection time so we never miss status updates
                out->startWatching();
            }
        }
        if (!connectFailed && out != existingOut) newOut = out;
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
        if (!connectFailed && in != existingIn) newIn = in;
    }

    if (connectFailed) {
        // Re-snapshot callbacks under lock, then fire outside.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = callbacks_;
        }
        if (cb.onStreamError) cb.onStreamError(0, StreamError::NetworkError);
        return -1;
    }

    // --- Phase 3: short critical section --- publish + allocate handle.
    StreamHandle handle = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (newOut) grpcClient_         = newOut;
        if (newIn)  streamInGrpcClient_ = newIn;
        // Cache deviceId if the provider produced one in phase 2.
        if (deviceId_ == 0 && deviceIdSnap != 0) deviceId_ = deviceIdSnap;

        handle = nextHandle_++;
        streams_[handle] = {type, StreamStatus::Created, StreamError::NoError, {}};
        cb = callbacks_;
        LogInfo(logCategory, "created handle=%d type=%s status=Created",
                handle, type == StreamType::StreamOut ? "StreamOut" : "StreamIn");
    }

    if (cb.onStreamStatus) cb.onStreamStatus(handle, StreamStatus::Created);
    return handle;
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

            LogInfo(logCategory, "start handle=%d type=%s url=%s port=%u status=%s",
                    handle,
                    streamType == StreamType::StreamOut ? "StreamOut" : "StreamIn",
                    configuration.url.c_str(),
                    static_cast<unsigned>(configuration.port),
                    statusToString(info.status));
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
        if (!inClient->startStream(configuration.url,
                                   static_cast<uint32_t>(configuration.port),
                                   /*path=*/"")) {
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
