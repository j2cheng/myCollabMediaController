#include "MediaController.h"
#include "grpcstreamout/StreamoutGrpcClient.h"

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
        std::cout << "[MediaController] >> onStreamStatus(handle=" << handle
                  << ", status=" << statusToString(status) << ")" << std::endl;
        if (callbacks.onStreamStatus) callbacks.onStreamStatus(handle, status);
    };
    callbacks_.onStreamError = [callbacks](StreamHandle handle, StreamError error) {
        std::cout << "[MediaController] >> onStreamError(handle=" << handle
                  << ", error=" << errorToString(error) << ")" << std::endl;
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
    std::cout << "[MediaController] deinit complete" << std::endl;
}

bool MediaControllerImpl::ensureStreamoutConnected() {
    // Lazy-init: create gRPC client if not injected externally
    if (!grpcClient_) {
        // Include here to avoid header dependency in MediaController.h
        // (StreamoutGrpcClient.h pulls in grpc++ headers)
        grpcClient_ = std::make_unique<StreamoutGrpcClient>();
    }
    if (grpcClient_->isConnected()) {
        return true;
    }
    if (!grpcClient_->connect(grpcTarget_)) {
        std::cout << "[MediaController] gRPC streamout connect failed" << std::endl;
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
            std::cout << "[MediaController] WatchStatus: stream_id=" << streamId
                      << " status_code=" << statusCode
                      << " status_info=\"" << statusInfo << "\"" << std::endl;

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
            // TODO: connect grpcstreamin when implemented
            std::cout << "[MediaController] StreamIn gRPC not yet implemented" << std::endl;
        }

        if (!connectFailed) {
            handle = nextHandle_++;
            streams_[handle] = {type, StreamStatus::Created, StreamError::NoError, {}};
            std::cout << "[MediaController] created handle=" << handle
                      << " type=" << (type == StreamType::StreamOut ? "StreamOut" : "StreamIn")
                      << " status=Created"
                      << std::endl;
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
    StreamoutGrpcClientInterface* client = nullptr;

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
                client = grpcClient_.get();
                cb = callbacks_;

                std::cout << "[MediaController] start handle=" << handle
                          << " url=" << configuration.url
                          << " port=" << configuration.port
                          << " status=" << statusToString(info.status)
                          << std::endl;
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

    // Send configuration and start command to gRPC streamout server
    if (client) {
        if (!client->setPort(std::to_string(configuration.port))) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = streams_.find(handle);
                if (it != streams_.end()) {
                    it->second.status = StreamStatus::Error;
                    it->second.lastError = StreamError::StartupFailed;
                }
            }
            if (cb.onStreamError) {
                cb.onStreamError(handle, StreamError::StartupFailed);
            }
            return;
        }

        //Note: startStream takes arguments, it is used in streamout apk for stream index.
        //      for now, we just set to 0. 
        //      Ideally, it should be the same as 'handle'.
        if (!client->startStream(0)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = streams_.find(handle);
                if (it != streams_.end()) {
                    it->second.status = StreamStatus::Error;
                    it->second.lastError = StreamError::StartupFailed;
                }
            }
            if (cb.onStreamError) {
                cb.onStreamError(handle, StreamError::StartupFailed);
            }
            return;
        }
    }
    // Status will be updated when lower layer reports back via WatchStatus.
}

void MediaControllerImpl::stop(StreamHandle handle) {
    GlobalCallbacks cb;
    StreamoutGrpcClientInterface* client = nullptr;

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
        client = grpcClient_.get();
        cb = callbacks_;

        std::cout << "[MediaController] stop handle=" << handle
                  << " status=Stopping" << std::endl;
    }

    // Fire Stopping callback outside lock
    if (cb.onStreamStatus) {
        cb.onStreamStatus(handle, StreamStatus::Stopping);
    }

    // Send stop command to gRPC streamout server
    if (client) {
        if (!client->stopStream(0)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = streams_.find(handle);
                if (it != streams_.end()) {
                    it->second.status = StreamStatus::Error;
                    it->second.lastError = StreamError::StopFailed;
                }
            }
            if (cb.onStreamError) {
                cb.onStreamError(handle, StreamError::StopFailed);
            }
            return;
        }
    }
    // Stopped status will come back asynchronously from lower layer streamout apk.
}

MediaController::StreamStatus MediaControllerImpl::getStatus(StreamHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(handle);
    if (it == streams_.end()) {
        return StreamStatus::Idle;
    }
    return it->second.status;
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
