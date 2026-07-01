#pragma once

#include "StreamoutGrpcClientInterface.h"
#include "grpc_str_out.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

// Enable/disable auto-reconnect. Comment out to let top layer manage connect.
#define AUTO_GRPC_RECONN

// Enable/disable keep-alive ping. Uses gRPC HTTP/2 PING frames (channel args).
#define GRPC_KEEPALIVE 1

/**
 * Real gRPC client implementation.
 * Requires libgrpc++-dev at link time.
 */
class StreamoutGrpcClient : public StreamoutGrpcClientInterface {
public:
    // Reconnect interval — change this to adjust retry frequency
    static constexpr std::chrono::seconds RECONNECT_INTERVAL{5};

#ifdef GRPC_KEEPALIVE
    // Keep-alive settings (gRPC HTTP/2 PING frames)
    static constexpr int KEEPALIVE_TIME_MS     = 10000;  // send ping every 10s
    static constexpr int KEEPALIVE_TIMEOUT_MS   = 3000;   // wait 3s for response
#endif

    StreamoutGrpcClient() = default;
    ~StreamoutGrpcClient() override;

    bool connect(const std::string& target) override;
    void disconnect() override;
    bool isConnected() const override;

    bool setProductId(uint32_t id) override;
    bool startStream(int32_t arg) override;
    bool stopStream(int32_t arg) override;
    bool setPort(const std::string& port) override;
    bool setPipeline(const std::string& pipeline) override;

    void setStatusCallback(StatusCallback callback) override;
    void startWatching() override;

private:
    mutable std::mutex mutex_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<streamout::v1::StreamoutService::Stub> stub_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> shuttingDown_{false};  // set in disconnect(); blocks new watch/reconnect threads
    StatusCallback statusCallback_;
    std::thread watchThread_;
    std::unique_ptr<grpc::ClientContext> watchContext_;  // for cancelling streaming RPC
    std::unique_ptr<grpc::ClientReader<streamout::v1::StreamoutStatusResponse>> watchReader_;
    std::string target_;  // saved for reconnect

    void watchStatusLoop();
    std::shared_ptr<grpc::Channel> createChannel(const std::string& target);

#ifdef AUTO_GRPC_RECONN
    // Auto-reconnect members
    std::thread reconnectThread_;
    std::atomic<bool> reconnectRunning_{false};
    std::condition_variable reconnectCv_;
    std::mutex reconnectMutex_;          // guards reconnectCv_ wait
    std::mutex reconnectControlMutex_;   // serializes startReconnectLoop/stopReconnectLoop

    void startReconnectLoop();
    void stopReconnectLoop();
    void reconnectLoop();
#endif

#ifdef GRPC_KEEPALIVE
    // Keepalive watcher — monitors channel state for TRANSIENT_FAILURE
    std::thread stateWatchThread_;
    std::atomic<bool> stateWatchRunning_{false};
    std::mutex stateWatchControlMutex_;   // serializes startStateWatch/stopStateWatch

    void startStateWatch();
    void stopStateWatch();
    void stateWatchLoop();
#endif
};
