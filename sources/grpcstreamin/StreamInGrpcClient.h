#pragma once

#include "StreamInGrpcClientInterface.h"
#include "RTSPClientService.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

/**
 * Real gRPC client for collab.stream_in.RTSPClientService.
 *
 * Minimal first-cut implementation: connect + startStream / stopStream / status.
 * No auto-reconnect or status streaming yet (the proto exposes a unary status
 * RPC, not a server-streaming one).
 */
class StreamInGrpcClient : public StreamInGrpcClientInterface {
public:
    StreamInGrpcClient() = default;
    ~StreamInGrpcClient() override;

    bool connect(const std::string& target) override;
    void disconnect() override;
    bool isConnected() const override;

    bool startStream(const std::string& address,
                     uint32_t port,
                     const std::string& path) override;
    bool stopStream() override;
    bool status() override;

private:
    mutable std::mutex mutex_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<collab::stream_in::RTSPClientService::Stub> stub_;
    std::atomic<bool> connected_{false};
    std::string target_;
};
