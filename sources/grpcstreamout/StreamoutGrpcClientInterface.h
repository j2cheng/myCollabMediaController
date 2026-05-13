#pragma once

#include <cstdint>
#include <functional>
#include <string>

/**
 * Abstract interface for the gRPC client that communicates with
 * the lower-layer Streamout server.
 *
 * MediaControllerImpl holds a pointer to this interface.
 * - In production (mk2 app): use StreamoutGrpcClient (real gRPC).
 * - In unit tests: use StreamoutGrpcClientMock (no network).
 */
class StreamoutGrpcClientInterface {
public:
    virtual ~StreamoutGrpcClientInterface() = default;

    // Connection management
    virtual bool connect(const std::string& target) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // RPC calls
    virtual bool setProductId(uint32_t id) = 0;
    virtual bool startStream(int32_t arg) = 0;
    virtual bool stopStream(int32_t arg) = 0;
    virtual bool setPort(const std::string& port) = 0;
    virtual bool setPipeline(const std::string& pipeline) = 0;

    // Status callback from server-streaming WatchStatus
    using StatusCallback = std::function<void(int32_t streamId, int32_t statusCode, const std::string& statusInfo)>;
    virtual void setStatusCallback(StatusCallback callback) = 0;

    // Start the WatchStatus server-streaming RPC (call once after connect + setStatusCallback)
    virtual void startWatching() = 0;
};
