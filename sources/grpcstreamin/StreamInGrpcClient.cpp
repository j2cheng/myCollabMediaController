#include "StreamInGrpcClient.h"

#ifdef USE_FOUNDATION
#include "Logging/Log.hpp"
#include <atomic>

using Foundation::Log;

static Log::Category logCategory;
static std::atomic<bool> logInitialized{false};

static void initLogging() {
    if (!logInitialized.exchange(true)) {
        logCategory = Log::GetInstance().AddCategory("StreamInGrpcClient");
    }
}
#else
#include <cstdarg>
#include <cstdio>
#include <iostream>

namespace {
inline void sicLog(std::ostream& os, const char* level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    os << "[" << level << "] [StreamInGrpcClient] " << buf << std::endl;
}
inline void initLogging() {}
} // namespace

#define LogInfo(cat, ...)    ::sicLog(std::cout, "INFO",  __VA_ARGS__)
#define LogWarning(cat, ...) ::sicLog(std::cout, "WARN",  __VA_ARGS__)
#define LogError(cat, ...)   ::sicLog(std::cerr, "ERROR", __VA_ARGS__)
#endif

#include <chrono>

StreamInGrpcClient::~StreamInGrpcClient() {
    disconnect();
}

bool StreamInGrpcClient::connect(const std::string& target) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) {
            return true;
        }
        target_ = target;
        channel_ = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
        stub_ = collab::stream_in::RTSPClientService::NewStub(channel_);
    }

    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(3);
    if (!channel_->WaitForConnected(deadline)) {
        initLogging();
        LogError(logCategory, "connect failed: timeout reaching %s", target.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        stub_.reset();
        channel_.reset();
        return false;
    }

    connected_ = true;
    initLogging();
    LogInfo(logCategory, "connected to %s", target.c_str());
    return true;
}

void StreamInGrpcClient::disconnect() {
    connected_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    stub_.reset();
    channel_.reset();
    initLogging();
    LogInfo(logCategory, "disconnected");
}

bool StreamInGrpcClient::isConnected() const {
    return connected_;
}

bool StreamInGrpcClient::startStream(const std::string& address,
                                     uint32_t port,
                                     const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || !stub_) return false;

    grpc::ClientContext context;
    collab::stream_in::StartStreamRequest request;
    collab::stream_in::StartStreamReply  response;

    request.set_address(address);
    request.set_port(port);
    request.set_path(path);

    // Use fakesink for both audio and video by default; the higher layer can
    // override later once a real pipeline mapping is decided.
    request.mutable_asink()->set_type(collab::stream_in::SINK_TYPE_FAKE);
    request.mutable_vsink()->set_type(collab::stream_in::SINK_TYPE_FAKE);

    auto status = stub_->startStream(&context, request, &response);
    initLogging();
    if (!status.ok()) {
        LogError(logCategory, "startStream failed: %s", status.error_message().c_str());
        return false;
    }

    if (response.has_error()) {
        LogError(logCategory, "startStream returned error code=%d verbose=\"%s\"",
                 static_cast<int>(response.error().code()),
                 response.error().has_verbose() ? response.error().verbose().c_str() : "");
        return false;
    }

    LogInfo(logCategory, "startStream OK address=%s port=%u path=%s",
            address.c_str(), port, path.c_str());
    return true;
}

bool StreamInGrpcClient::stopStream() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || !stub_) return false;

    grpc::ClientContext context;
    collab::stream_in::StopStreamRequest request;
    collab::stream_in::StopStreamReply  response;

    auto status = stub_->stopStream(&context, request, &response);
    initLogging();
    if (!status.ok()) {
        LogError(logCategory, "stopStream failed: %s", status.error_message().c_str());
        return false;
    }
    if (response.has_error()) {
        LogError(logCategory, "stopStream returned error code=%d",
                 static_cast<int>(response.error().code()));
        return false;
    }
    LogInfo(logCategory, "stopStream OK");
    return true;
}

bool StreamInGrpcClient::status() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || !stub_) return false;

    grpc::ClientContext context;
    collab::stream_in::StatusRequest request;
    collab::stream_in::StatusReply  response;

    auto rpcStatus = stub_->status(&context, request, &response);
    initLogging();
    if (!rpcStatus.ok()) {
        LogError(logCategory, "status RPC failed: %s", rpcStatus.error_message().c_str());
        return false;
    }
    if (response.has_error()) {
        LogError(logCategory, "status returned error code=%d",
                 static_cast<int>(response.error().code()));
        return false;
    }
    LogInfo(logCategory, "status OK");
    return true;
}
