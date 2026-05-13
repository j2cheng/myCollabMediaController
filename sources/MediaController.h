#pragma once

#include "MediaControllerInterface.h"
#include "grpcstreamout/StreamoutGrpcClientInterface.h"
#include <map>
#include <memory>
#include <mutex>
#include <iostream>

class MediaControllerImpl : public MediaController {
public:
    static MediaControllerImpl& getImplInstance();
    ~MediaControllerImpl() override = default;

    void setGlobalCallbacks(const GlobalCallbacks& callbacks) override;
    StreamHandle create(StreamType type) override;
    void start(StreamHandle handle, const StreamConfiguration& configuration) override;
    void stop(StreamHandle handle) override;
    StreamStatus getStatus(StreamHandle handle) const override;
    StreamError getLastError(StreamHandle handle) const override;
    bool isValidHandle(StreamHandle handle) const override;

    // gRPC client injection (for testing vs production)
    void setGrpcClient(std::unique_ptr<StreamoutGrpcClientInterface> client);
    void setGrpcTarget(const std::string& target);
    void setDeviceId(uint32_t id);
    void setDeviceIdProvider(std::function<uint32_t()> provider);

    /**
     * Tear down all background threads and release resources.
     * Call this before app exit (singleton destructor may not run reliably).
     * Safe to call multiple times. After deinit(), create() will reconnect.
     */
    void deinit();

private:
    MediaControllerImpl() = default;

    struct StreamInfo {
        StreamType type;
        StreamStatus status = StreamStatus::Idle;
        StreamError lastError = StreamError::NoError;
        StreamConfiguration config;
    };

    bool ensureStreamoutConnected();

    GlobalCallbacks callbacks_;
    std::map<StreamHandle, StreamInfo> streams_;
    StreamHandle nextHandle_ = 1;
    mutable std::mutex mutex_;

    // grpcClient_ pointer read under mutex_ (in create/deinit). Only call setGrpcClient() during init.
    // If not set externally, ensureStreamoutConnected() creates one automatically.
    std::unique_ptr<StreamoutGrpcClientInterface> grpcClient_;
    std::string grpcTarget_ = "127.0.0.1:50051";  // default target; override with setGrpcTarget()
    uint32_t deviceId_ = 0xFF00;  // default device ID; override with setDeviceId() or setDeviceIdProvider()
    std::function<uint32_t()> deviceIdProvider_;
};
