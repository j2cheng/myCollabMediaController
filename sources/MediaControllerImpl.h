#pragma once

#include "MediaController.h"
#include "grpcstreamout/StreamoutGrpcClientInterface.h"
#include "grpcstreamin/StreamInGrpcClientInterface.h"
#include <map>
#include <memory>
#include <mutex>
#include <iostream>

class MediaControllerImpl  {
public:
    // --- Type aliases for MediaController types ---
    using StreamType = MediaController::StreamType;
    using StreamStatus = MediaController::StreamStatus;
    using StreamError = MediaController::StreamError;
    using StreamHandle = MediaController::StreamHandle;
    using StreamConfiguration = MediaController::StreamConfiguration;
    using GlobalCallbacks = MediaController::GlobalCallbacks;

    MediaControllerImpl() = default;
    ~MediaControllerImpl() { deinit(); }

    void setGlobalCallbacks(const GlobalCallbacks& callbacks);
    StreamHandle create(StreamType type);
    void start(StreamHandle handle, const StreamConfiguration& configuration);
    void stop(StreamHandle handle);
    StreamStatus getStatus(StreamHandle handle) const;
    StreamError getLastError(StreamHandle handle) const;
    bool isValidHandle(StreamHandle handle) const;

    // gRPC client injection (for testing vs production)
    void setGrpcClient(std::unique_ptr<StreamoutGrpcClientInterface> client);
    void setGrpcTarget(const std::string& target);
    void setStreamInGrpcClient(std::unique_ptr<StreamInGrpcClientInterface> client);
    void setStreamInGrpcTarget(const std::string& target);
    void setDeviceId(uint32_t id);
    void setDeviceIdProvider(std::function<uint32_t()> provider);

    void deinit();

private:
    struct StreamInfo {
        StreamType type;
        StreamStatus status = StreamStatus::Idle;
        StreamError lastError = StreamError::NoError;
        StreamConfiguration config;
    };

    GlobalCallbacks callbacks_;
    mutable std::map<StreamHandle, StreamInfo> streams_;
    StreamHandle nextHandle_ = 1;
    mutable std::mutex mutex_;

    std::shared_ptr<StreamoutGrpcClientInterface> grpcClient_;
    std::string grpcTarget_ = "127.0.0.1:50051";
    std::shared_ptr<StreamInGrpcClientInterface> streamInGrpcClient_;
    std::string streamInGrpcTarget_ = "127.0.0.1:50052";
    uint32_t deviceId_ = 0xFF00;
    std::function<uint32_t()> deviceIdProvider_;
};
