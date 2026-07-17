#include "StreamoutGrpcClient.h"

#define LOG_TAG "StreamoutGrpcClient"
#include "Logging.h"

StreamoutGrpcClient::~StreamoutGrpcClient() {
    disconnect();
}

bool StreamoutGrpcClient::connect(const std::string& target) {
    // Quiesce any in-flight reconnect/state-watch threads so this manual
    // connect cannot race with them on channel_/stub_/connected_ and on
    // a second startStateWatch() spawn. shuttingDown_ blocks the exiting
    // reconnect thread from re-spawning anything; we clear it again before
    // building the new channel.
    shuttingDown_ = true;
#ifdef AUTO_GRPC_RECONN
    stopReconnectLoop();
#endif
#ifdef GRPC_KEEPALIVE
    stopStateWatch();
#endif
    shuttingDown_ = false;
    std::shared_ptr<grpc::Channel> ch;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (connected_) {
            return true;  // already connected
        }

        target_ = target;  // save for reconnect

        channel_ = createChannel(target);
        stub_ = streamout::v1::StreamoutService::NewStub(channel_);
        ch = channel_;  // local copy for thread-safe access outside lock
    }

    // Wait outside lock — avoids blocking other threads for up to 3s
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(3);
    if (!ch->WaitForConnected(deadline)) {
        initLogging();
        LogError(logCategory, "connect failed: timeout reaching %s", target.c_str());
        std::unique_lock<std::mutex> lock(mutex_);
        stub_.reset();
        channel_.reset();
#ifdef AUTO_GRPC_RECONN
        lock.unlock();  // release before startReconnectLoop to avoid AB-BA deadlock with reconnectLoop
        startReconnectLoop();
#endif
        return false;
    }

    connected_ = true;
    initLogging();
    LogInfo(logCategory, "connected to %s", target.c_str());

    // Start state watcher outside the lock so it doesn't block
#ifdef GRPC_KEEPALIVE
    startStateWatch();
#endif
    return true;
}

void StreamoutGrpcClient::disconnect() {
    // Set first so any in-flight reconnect/stateWatch thread that finishes
    // during the stop sequence below refuses to spawn replacement threads.
    shuttingDown_ = true;
#ifdef AUTO_GRPC_RECONN
    stopReconnectLoop();   // stop reconnect first: prevents it from re-spawning state watch
#endif
#ifdef GRPC_KEEPALIVE
    stopStateWatch();
#endif
    connected_ = false;

    // Cancel any active streaming RPC to unblock watchThread_
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (watchContext_) {
            watchContext_->TryCancel();
        }
    }

    // Join outside lock — watchStatusLoop may briefly lock mutex_ before exiting
    if (watchThread_.joinable()) {
        watchThread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    watchContext_.reset();
    watchReader_.reset();
    stub_.reset();
    channel_.reset();
    initLogging();
    LogInfo(logCategory, "disconnected");
}

bool StreamoutGrpcClient::isConnected() const {
    return connected_;
}

bool StreamoutGrpcClient::setProductId(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || !stub_) return false;

    grpc::ClientContext context;
    streamout::v1::StreamoutSetProductIdRequest request;
    streamout::v1::StreamoutActionResponse response;

    request.set_id(static_cast<int32_t>(id));

    auto status = stub_->StreamoutSetProductId(&context, request, &response);
    initLogging();
    if (!status.ok()) {
        LogError(logCategory, "setProductId failed: %s", status.error_message().c_str());
        return false;
    }

    LogInfo(logCategory, "setProductId=%u success=%d", id, static_cast<int>(response.success()));
    return response.success();
}

bool StreamoutGrpcClient::startStream(int32_t arg) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!connected_ || !stub_) return false;

    grpc::ClientContext context;
    streamout::v1::StreamoutStartRequest request;
    streamout::v1::StreamoutActionResponse response;

    request.set_arg(arg);

    auto status = stub_->StreamoutStart(&context, request, &response);
    initLogging();
    if (!status.ok()) {
        LogError(logCategory, "startStream failed: %s", status.error_message().c_str());
#ifdef AUTO_GRPC_RECONN
        connected_ = false;
        stub_.reset();
        channel_.reset();
        lock.unlock();  // release before startReconnectLoop to avoid AB-BA deadlock with reconnectLoop
        startReconnectLoop();
#endif
        return false;
    }

    LogInfo(logCategory, "startStream success=%d", static_cast<int>(response.success()));

    return response.success();
}

bool StreamoutGrpcClient::stopStream(int32_t arg) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!connected_ || !stub_) return false;

    grpc::ClientContext context;
    streamout::v1::StreamoutStopRequest request;
    streamout::v1::StreamoutActionResponse response;

    request.set_arg(arg);

    auto status = stub_->StreamoutStop(&context, request, &response);
    initLogging();
    if (!status.ok()) {
        LogError(logCategory, "stopStream failed: %s", status.error_message().c_str());
#ifdef AUTO_GRPC_RECONN
        connected_ = false;
        stub_.reset();
        channel_.reset();
        lock.unlock();  // release before startReconnectLoop to avoid AB-BA deadlock with reconnectLoop
        startReconnectLoop();
#endif
        return false;
    }

    LogInfo(logCategory, "stopStream success=%d", static_cast<int>(response.success()));
    return response.success();
}

bool StreamoutGrpcClient::setPort(const std::string& port) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || !stub_) return false;

    grpc::ClientContext context;
    streamout::v1::StreamoutSetPortRequest request;
    streamout::v1::StreamoutActionResponse response;

    request.set_port(port);

    auto status = stub_->StreamoutSetPort(&context, request, &response);
    initLogging();
    if (!status.ok()) {
        LogError(logCategory, "setPort failed: %s", status.error_message().c_str());
        return false;
    }

    return response.success();
}

bool StreamoutGrpcClient::setPipeline(const std::string& pipeline) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || !stub_) return false;

    grpc::ClientContext context;
    streamout::v1::StreamoutSetPipelineRequest request;
    streamout::v1::StreamoutActionResponse response;

    request.set_pipeline(pipeline);

    auto status = stub_->StreamoutSetPipeline(&context, request, &response);
    initLogging();
    if (!status.ok()) {
        LogError(logCategory, "setPipeline failed: %s", status.error_message().c_str());
        return false;
    }

    return response.success();
}

bool StreamoutGrpcClient::StreamoutServerDebug(const std::string& debug_string) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || !stub_) return false;

    grpc::ClientContext context;
    streamout::v1::StreamoutDebugRequest request;
    streamout::v1::StreamoutActionResponse response;

    request.set_debug_string(debug_string);

    auto status = stub_->StreamoutServerDebug(&context, request, &response);
    initLogging();
    if (!status.ok()) {
        LogError(logCategory, "StreamoutServerDebug failed: %s", status.error_message().c_str());
        return false;
    }

    LogInfo(logCategory, "StreamoutServerDebug success=%d payload_bytes=%zu",
            static_cast<int>(response.success()), debug_string.size());
    return response.success();
}

bool StreamoutGrpcClient::setPairedDevices(const std::vector<PairedDeviceEntry>& devices) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || !stub_) return false;

    grpc::ClientContext context;
    streamout::v1::StreamoutSetPairedDevicesRequest request;
    streamout::v1::StreamoutActionResponse response;

    // Pack every entry from the local vector into the protobuf `repeated`
    // field. `add_devices()` appends a new PairedDevice sub-message to the
    // request and returns a pointer we populate in place; the whole list is
    // then sent in a single unary RPC below.
    for (const auto& d : devices) {
        auto* proto = request.add_devices();
        proto->set_device_id(d.deviceId);
        proto->set_ip_address(d.ipAddress);
        proto->set_mac_address(d.macAddress);
    }

    auto status = stub_->StreamoutSetPairedDevices(&context, request, &response);
    initLogging();
    if (!status.ok()) {
        LogError(logCategory, "setPairedDevices failed: %s", status.error_message().c_str());
        return false;
    }

    LogInfo(logCategory, "setPairedDevices success=%d count=%zu",
            static_cast<int>(response.success()), devices.size());
    return response.success();
}

void StreamoutGrpcClient::setStatusCallback(StatusCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    statusCallback_ = std::move(callback);
}

void StreamoutGrpcClient::startWatching() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!connected_ || !stub_ || !statusCallback_) return;

    // Cancel any existing watch before joining
    if (watchContext_) {
        watchContext_->TryCancel();
    }
    if (watchThread_.joinable()) {
        lock.unlock();
        watchThread_.join();
        lock.lock();
        // disconnect() may have run while we were joining; re-validate state
        if (!connected_ || !stub_ || !statusCallback_) return;
    }

    // Set up the reader synchronously so it's ready before any RPCs are sent
    watchContext_ = std::make_unique<grpc::ClientContext>();
    streamout::v1::StreamoutStatusRequest request;
    request.set_id(0);  // watch all streams
    watchReader_ = stub_->StreamoutWatchStatus(watchContext_.get(), request);

    initLogging();
    LogInfo(logCategory, "WatchStatus stream opened, ready to receive");

    // Spawn thread just for the Read() loop
    watchThread_ = std::thread(&StreamoutGrpcClient::watchStatusLoop, this);
}

void StreamoutGrpcClient::watchStatusLoop() {
    initLogging();
    LogInfo(logCategory, "watchStatusLoop started, waiting for server status...");

    streamout::v1::StreamoutStatusResponse response;
    while (connected_ && watchReader_ && watchReader_->Read(&response)) {
        StatusCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = statusCallback_;
        }
        if (cb) {
            cb(response.stream_id(),
               static_cast<int32_t>(response.status_code()),
               response.status_info());
        }
    }

    LogInfo(logCategory, "watchStatusLoop ended");

#ifdef AUTO_GRPC_RECONN
    // If we lost connection unexpectedly, trigger reconnect
    if (connected_) {

        LogInfo(logCategory, "watchStatusLoop need to reconnect");
        connected_ = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stub_.reset();
            channel_.reset();
        }
        // startReconnectLoop outside lock — it may join an old thread that briefly locks mutex_
        startReconnectLoop();
    }
#endif
}

#ifdef AUTO_GRPC_RECONN
void StreamoutGrpcClient::startReconnectLoop() {
    // Serialize the check-set-join-spawn sequence so concurrent callers cannot
    // double-spawn (which would assign to a joinable reconnectThread_ and call
    // std::terminate).
    std::lock_guard<std::mutex> g(reconnectControlMutex_);
    if (shuttingDown_) return;      // disconnect in progress; do not respawn
    if (reconnectRunning_) return;  // already running
    reconnectRunning_ = true;
    if (reconnectThread_.joinable()) {
        reconnectThread_.join();
    }
    reconnectThread_ = std::thread(&StreamoutGrpcClient::reconnectLoop, this);
}

void StreamoutGrpcClient::stopReconnectLoop() {
    std::lock_guard<std::mutex> g(reconnectControlMutex_);
    reconnectRunning_ = false;
    reconnectCv_.notify_all();
    if (reconnectThread_.joinable()) {
        reconnectThread_.join();
    }
}

void StreamoutGrpcClient::reconnectLoop() {
    initLogging();
    LogInfo(logCategory, "reconnect loop started (interval=%llds)",
            static_cast<long long>(RECONNECT_INTERVAL.count()));

    while (reconnectRunning_) {
        // Sleep for the reconnect interval (interruptible via cv)
        {
            std::unique_lock<std::mutex> lock(reconnectMutex_);
            reconnectCv_.wait_for(lock, RECONNECT_INTERVAL, [this] {
                return !reconnectRunning_.load();
            });
        }

        if (!reconnectRunning_) break;

        // Snapshot target_ under lock to avoid a data race if connect() rewrites it concurrently.
        std::string target;
        std::shared_ptr<grpc::Channel> ch;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target = target_;
            ch = createChannel(target);
        }

        LogInfo(logCategory, "attempting reconnect to %s", target.c_str());

        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(3);
        if (ch->WaitForConnected(deadline)) {
            std::unique_lock<std::mutex> lock(mutex_);
            channel_ = ch;
            stub_ = streamout::v1::StreamoutService::NewStub(channel_);
            connected_ = true;
            LogInfo(logCategory, "reconnected to %s", target.c_str());
            reconnectRunning_ = false;

            lock.unlock();  // unlock before starting state watch to avoid deadlock
            startWatching();  // restart watchStatusLoop
#ifdef GRPC_KEEPALIVE
            startStateWatch();
#endif
        } else {
            LogWarning(logCategory, "reconnect failed, retrying in %llds",
                       static_cast<long long>(RECONNECT_INTERVAL.count()));
        }
    }

    LogInfo(logCategory, "reconnect loop ended");
}
#endif

std::shared_ptr<grpc::Channel> StreamoutGrpcClient::createChannel(const std::string& target) {
#ifdef GRPC_KEEPALIVE
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, KEEPALIVE_TIME_MS);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, KEEPALIVE_TIMEOUT_MS);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    args.SetInt(GRPC_ARG_HTTP2_BDP_PROBE, 0);
    initLogging();
    LogInfo(logCategory, "creating channel with keepalive (ping=%dms, timeout=%dms)",
            KEEPALIVE_TIME_MS, KEEPALIVE_TIMEOUT_MS);
    return grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), args);
#else
    return grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
#endif
}

#ifdef GRPC_KEEPALIVE
void StreamoutGrpcClient::startStateWatch() {
    // Serialize check-join-spawn so concurrent callers cannot assign to a
    // joinable stateWatchThread_ (which would call std::terminate).
    std::lock_guard<std::mutex> g(stateWatchControlMutex_);
    if (shuttingDown_) return;  // disconnect in progress; do not respawn
    // Join old thread first to avoid race with stateWatchRunning_ reset at loop exit
    if (stateWatchThread_.joinable()) {
        stateWatchRunning_ = false;  // signal old loop to stop
        stateWatchThread_.join();
    }
    stateWatchRunning_ = true;
    stateWatchThread_ = std::thread(&StreamoutGrpcClient::stateWatchLoop, this);
}

void StreamoutGrpcClient::stopStateWatch() {
    std::lock_guard<std::mutex> g(stateWatchControlMutex_);
    stateWatchRunning_ = false;
    if (stateWatchThread_.joinable()) {
        stateWatchThread_.join();
    }
}

void StreamoutGrpcClient::stateWatchLoop() {
    initLogging();
    LogInfo(logCategory, "channel state watcher started");

    while (stateWatchRunning_ && connected_) {
        std::shared_ptr<grpc::Channel> ch;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ch = channel_;
        }
        if (!ch) break;

        // Get current state (true = try to connect if IDLE, keeps subchannel active)
        auto state = ch->GetState(true);

        if (state == GRPC_CHANNEL_TRANSIENT_FAILURE ||
            state == GRPC_CHANNEL_SHUTDOWN) {
            LogError(logCategory, "channel state FAILED (state=%d)", static_cast<int>(state));
            connected_ = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stub_.reset();
                channel_.reset();
            }
#ifdef AUTO_GRPC_RECONN
            startReconnectLoop();
#endif
            break;
        }

        // Block until state changes or timeout (wakes on TRANSIENT_FAILURE)
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(1);
        ch->WaitForStateChange(state, deadline);
    }

    stateWatchRunning_ = false;  // allow restart on next reconnect
    LogInfo(logCategory, "channel state watcher ended");
}
#endif
