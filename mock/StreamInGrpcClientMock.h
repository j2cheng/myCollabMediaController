#pragma once

#include "StreamInGrpcClientInterface.h"
#include <iostream>
#include <string>

/**
 * Mock gRPC client for StreamIn unit testing.
 * No real network — all calls succeed with hardcoded responses.
 */
class StreamInGrpcClientMock : public StreamInGrpcClientInterface {
public:
    StreamInGrpcClientMock() = default;
    ~StreamInGrpcClientMock() override = default;

    bool connect(const std::string& target) override {
        std::cout << "[StreamInMock] connect(" << target << ")" << std::endl;
        connected_ = true;
        return true;
    }

    void disconnect() override {
        std::cout << "[StreamInMock] disconnect()" << std::endl;
        connected_ = false;
    }

    bool isConnected() const override { return connected_; }

    bool startStream(const std::string& address,
                     uint32_t port,
                     const std::string& path) override {
        std::cout << "[StreamInMock] startStream(" << address
                  << ", " << port << ", \"" << path << "\")" << std::endl;
        lastAddress_ = address;
        lastPort_    = port;
        lastPath_    = path;
        return true;
    }

    bool stopStream() override {
        std::cout << "[StreamInMock] stopStream()" << std::endl;
        return true;
    }

    bool status() override {
        std::cout << "[StreamInMock] status()" << std::endl;
        return true;
    }

    const std::string& getLastAddress() const { return lastAddress_; }
    uint32_t           getLastPort()    const { return lastPort_; }
    const std::string& getLastPath()    const { return lastPath_; }

private:
    bool        connected_ = false;
    std::string lastAddress_;
    uint32_t    lastPort_ = 0;
    std::string lastPath_;
};
