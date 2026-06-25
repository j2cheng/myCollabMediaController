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
    std::lock_guard<std::mutex> lock(mutex_);
    grpcClient_ = std::move(client);
}

void MediaControllerImpl::setGrpcTarget(const std::string& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    grpcTarget_ = target;
}

void MediaControllerImpl::setStreamInGrpcClient(std::unique_ptr<StreamInGrpcClientInterface> client) {
    std::lock_guard<std::mutex> lock(mutex_);
    streamInGrpcClient_ = std::move(client);
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
    // Disconnect gRPC client — stops reconnect thread, state watcher, watch stream
    // Take grpcClient_ pointer under lock, then call disconnect() outside
    // (disconnect() may block on thread joins; we don't want to hold mutex_ during that)
    StreamoutGrpcClientInterface* client = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = grpcClient_.get();
    }
    if (client) {
        client->disconnect();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    streams_.clear();
    nextHandle_ = 1;
    deviceId_ = 0;
    LogInfo(logCategory, "deinit complete");
}

bool MediaControllerImpl::ensureStreamoutConnected() {
    // Lazy-init: create gRPC client if not injected externally
    if (!grpcClient_) {
        // Include here to avoid header dependency in MediaControllerImpl.h
        // (StreamoutGrpcClient.h pulls in grpc++ headers)
        grpcClient_ = std::make_unique<StreamoutGrpcClient>();
    }
    if (grpcClient_->isConnected()) {
        return true;
    }
    if (!grpcClient_->connect(grpcTarget_)) {
        LogError(logCategory, "gRPC streamout connect failed");
        return false;
    }
    // Send device ID immediately after connecting
    uint32_t id = deviceId_;
    if (id == 0 && deviceIdProvider_) {
        id = deviceIdProvider_();
        deviceId_ = id;
    }
    if (id != 0) {
        grpcClient_->setProductId(id);
    }

    // Register status callback to receive StreamoutStatusResponse from server
    grpcClient_->setStatusCallback(
        [this](int32_t streamId, int32_t statusCode, const std::string& statusInfo) {
            LogInfo(logCategory, "WatchStatus: stream_id=%d status_code=%d status_info=\"%s\"",
                    streamId, statusCode, statusInfo.c_str());

            GlobalCallbacks cb;
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

                // Update all matching streams (streamId 0 = all)
                for (auto& [handle, info] : streams_) {
                    info.status = newStatus;
                    notifications.emplace_back(handle, newStatus);
                }
                cb = callbacks_;
            }

            // Fire callbacks outside lock to prevent deadlock if callback re-enters
            if (cb.onStreamStatus) {
                for (auto& [handle, status] : notifications) {
                    cb.onStreamStatus(handle, status);
                }
            }
        });

    // Start the WatchStatus stream at connection time so we never miss status updates
    grpcClient_->startWatching();

    return true;
}

bool MediaControllerImpl::ensureStreamInConnected() {
    // Lazy-init: create gRPC client if not injected externally
    if (!streamInGrpcClient_) {
        // Include here to avoid header dependency in MediaControllerImpl.h
        streamInGrpcClient_ = std::make_unique<StreamInGrpcClient>();
    }
    if (streamInGrpcClient_->isConnected()) {
        return true;
    }
    if (!streamInGrpcClient_->connect(streamInGrpcTarget_)) {
        LogError(logCategory, "gRPC streamin connect failed (target=%s)", streamInGrpcTarget_.c_str());
        return false;
    }
    LogInfo(logCategory, "gRPC streamin connected (target=%s)", streamInGrpcTarget_.c_str());
    return true;
}

MediaController::StreamHandle MediaControllerImpl::create(StreamType type) {
    GlobalCallbacks cb;
    StreamHandle handle = -1;
    bool connectFailed = false;
    bool invalidType = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Validate stream type
        if (type != StreamType::StreamOut && type != StreamType::StreamIn) {
            invalidType = true;
            cb = callbacks_;
        }

        // Connect to the appropriate gRPC server based on stream type
        if (!invalidType && type == StreamType::StreamOut) {
            if (!ensureStreamoutConnected()) {
                connectFailed = true;
                cb = callbacks_;
            }
        } else if (!invalidType && type == StreamType::StreamIn) {
            if (!ensureStreamInConnected()) {
                connectFailed = true;
                cb = callbacks_;
            }
        }

        if (!connectFailed) {
            handle = nextHandle_++;
            streams_[handle] = {type, StreamStatus::Created, StreamError::NoError, {}};
            LogInfo(logCategory, "created handle=%d type=%s status=Created",
                    handle, type == StreamType::StreamOut ? "StreamOut" : "StreamIn");
            cb = callbacks_;
        }
    }

    // Invoke callbacks outside lock to avoid deadlock if callback re-enters
    if (invalidType) {
        if (cb.onStreamError) {
            cb.onStreamError(0, StreamError::InvalidStreamType);
        }
        return -1;
    }
    if (connectFailed) {
        if (cb.onStreamError) {
            cb.onStreamError(0, StreamError::NetworkError);
        }
        return -1;
    }
    if (cb.onStreamStatus) {
        cb.onStreamStatus(handle, StreamStatus::Created);
    }
    return handle;
}

void MediaControllerImpl::start(StreamHandle handle, const StreamConfiguration& configuration) {
    GlobalCallbacks cb;
    MediaController::StreamError errorToReport = StreamError::NoError;
    StreamoutGrpcClientInterface* outClient = nullptr;
    StreamInGrpcClientInterface*  inClient  = nullptr;
    StreamType streamType = StreamType::StreamOut;  // overwritten below; value irrelevant on error

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(handle);
        if (it == streams_.end()) {
            errorToReport = StreamError::InvalidHandle;
            cb = callbacks_;
        } else {
            auto& info = it->second;
            if (info.status == StreamStatus::Active) {
                info.lastError = StreamError::AlreadyStarted;
                errorToReport = StreamError::AlreadyStarted;
                cb = callbacks_;
            } else {
                info.config = configuration;
                streamType = info.type;
                if (streamType == StreamType::StreamOut) {
                    outClient = grpcClient_.get();
                } else {
                    inClient = streamInGrpcClient_.get();
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
    StreamoutGrpcClientInterface* outClient = nullptr;
    StreamInGrpcClientInterface*  inClient  = nullptr;
    StreamType streamType = StreamType::StreamOut;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(handle);
        if (it == streams_.end()) {
            return;  // no-op for invalid handles per interface doc
        }

        auto& info = it->second;
        if (info.status == StreamStatus::Stopped || info.status == StreamStatus::Idle) {
            return;  // already stopped, no-op
        }

        info.status = StreamStatus::Stopping;
        streamType = info.type;
        if (streamType == StreamType::StreamOut) {
            outClient = grpcClient_.get();
        } else {
            inClient = streamInGrpcClient_.get();
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
    StreamInGrpcClientInterface* inClient = nullptr;
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
            inClient = streamInGrpcClient_.get();
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
