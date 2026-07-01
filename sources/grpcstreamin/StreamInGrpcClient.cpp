#include "StreamInGrpcClient.h"

#define LOG_TAG "StreamInGrpcClient"
#include "Logging.h"

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
    request.mutable_asink()->set_type(collab::stream_in::SINK_TYPE_USB);
    request.mutable_vsink()->set_type(collab::stream_in::SINK_TYPE_USB);

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
