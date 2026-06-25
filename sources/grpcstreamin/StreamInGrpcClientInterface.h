#pragma once

#include <cstdint>
#include <functional>
#include <string>

/**
 * Abstract interface for the gRPC client that talks to the lower-layer
 * StreamIn server (collab.stream_in.RTSPClientService).
 *
 * MediaControllerImpl holds a pointer to this interface. Use the real
 * StreamInGrpcClient in production; replace with a mock in unit tests.
 */
class StreamInGrpcClientInterface {
public:
    virtual ~StreamInGrpcClientInterface() = default;

    // Connection management
    virtual bool connect(const std::string& target) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // RPC calls (mirrors RTSPClientService.proto)
    virtual bool startStream(const std::string& address,
                             uint32_t port,
                             const std::string& path) = 0;
    virtual bool stopStream() = 0;
    virtual bool status() = 0;
};
