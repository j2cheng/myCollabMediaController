#pragma once

#include "StreamoutGrpcClientInterface.h"
#include <iostream>

/**
 * Mock gRPC client for unit testing.
 * No real network — all calls succeed with hardcoded responses.
 */
class StreamoutGrpcClientMock : public StreamoutGrpcClientInterface {
public:
    StreamoutGrpcClientMock() = default;
    ~StreamoutGrpcClientMock() override = default;

    bool connect(const std::string& target) override {
        std::cout << "[GrpcMock] connect(" << target << ")" << std::endl;
        connected_ = true;
        return true;
    }

    void disconnect() override {
        std::cout << "[GrpcMock] disconnect()" << std::endl;
        connected_ = false;
    }

    bool isConnected() const override {
        return connected_;
    }

    bool setProductId(uint32_t id) override {
        std::cout << "[GrpcMock] setProductId(" << id << ")" << std::endl;
        lastProductId_ = id;
        return true;
    }

    bool startStream(int32_t arg) override {
        std::cout << "[GrpcMock] startStream(" << arg << ")" << std::endl;
        return true;
    }

    bool stopStream(int32_t arg) override {
        std::cout << "[GrpcMock] stopStream(" << arg << ")" << std::endl;
        return true;
    }

    bool setPort(const std::string& port) override {
        std::cout << "[GrpcMock] setPort(" << port << ")" << std::endl;
        return true;
    }

    bool setPipeline(const std::string& pipeline) override {
        std::cout << "[GrpcMock] setPipeline(" << pipeline << ")" << std::endl;
        return true;
    }

    void setStatusCallback(StatusCallback callback) override {
        statusCallback_ = std::move(callback);
    }

    void startWatching() override {
        // No-op in mock — use simulateStatusUpdate() to push status
    }

    // Test helpers — allow tests to simulate status updates from server
    void simulateStatusUpdate(int32_t streamId, int32_t statusCode, const std::string& info) {
        if (statusCallback_) {
            statusCallback_(streamId, statusCode, info);
        }
    }

    uint32_t getLastProductId() const { return lastProductId_; }

private:
    bool connected_ = false;
    uint32_t lastProductId_ = 0;
    StatusCallback statusCallback_;
};
