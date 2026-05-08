#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <grpcpp/grpcpp.h>
#include "packagehello.grpc.pb.h"

// Set to 1 to enable the legacy SayHello-based connectivity watchdog probe
// inside ListenStreamStatusRqst.  The keepalive-ping mechanism
// (GRPC_ARG_KEEPALIVE_*) + StartConnectivityMonitor() now handles
// connection health, so this can be disabled (set to 0 or remove).
#define CHECK_CONNECTION 0

using grpc::Channel;
using grpc::ClientReader;
using grpc::ClientContext;
using grpc::Status;
using streamout::v1::StreamoutService;
using streamout::v1::HelloRequest;
using streamout::v1::HelloReply;
using streamout::v1::StreamoutSetPortRequest;
using streamout::v1::StreamoutSetPipelineRequest;
using streamout::v1::StreamoutStartRequest;
using streamout::v1::StreamoutStopRequest;
using streamout::v1::StreamoutDebugRequest;
using streamout::v1::StreamoutSetProductIdRequest;
using streamout::v1::StreamoutActionResponse;
using streamout::v1::StreamoutStatusRequest;
using streamout::v1::StreamoutStatusResponse;

std::string LogPrefix() {
	auto now = std::chrono::system_clock::now();
	std::time_t now_c = std::chrono::system_clock::to_time_t(now);
	std::tm tm_now{};
	localtime_r(&now_c, &tm_now);

	std::ostringstream os;
	os << "[" << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S")
	   << "][tid=" << std::this_thread::get_id() << "] ";
	return os.str();
}


class HelloClient {
public:
	HelloClient(const std::string& target)
		: target_(target) {
		{
			std::lock_guard<std::mutex> lk(connection_mu_);
			RecreateChannelLocked();
		}
		StartConnectivityMonitor();
	}

	~HelloClient() {
		conn_monitor_stop_.store(true);
		if (conn_monitor_thread_.joinable()) {
			conn_monitor_thread_.join();
		}
	}

	std::string SayHello(const std::string& user) {
		HelloRequest request;
		request.set_name(user);
		std::cout << LogPrefix() << "[SEND] SayHello name=" << request.name() << std::endl;
		HelloReply reply;
		std::shared_ptr<StreamoutService::Stub> stub;
		if (!PrepareMainStub(stub)) {
			return "RPC failed: unable to connect to server";
		}
		ClientContext context;
		ConfigureUnaryContext(context);
		Status status = stub->SayHello(&context, request, &reply);
		if (status.ok()) {
			std::cout << LogPrefix() << "[RECV] SayHello message=" << reply.message() << std::endl;
			return reply.message();
		} else {
			std::cout << LogPrefix() << "[RECV] SayHello RPC failed: " << status.error_message() << std::endl;
			return "RPC failed: " + status.error_message();
		}
	}

	std::string SetPort(const std::string& port) {
		StreamoutSetPortRequest request;
		request.set_port(port);
		std::cout << LogPrefix() << "[SEND] StreamoutSetPort port=" << request.port() << std::endl;
		StreamoutActionResponse reply;
		std::shared_ptr<StreamoutService::Stub> stub;
		if (!PrepareMainStub(stub)) {
			return "RPC failed: unable to connect to server";
		}
		ClientContext context;
		ConfigureUnaryContext(context);
		Status status = stub->StreamoutSetPort(&context, request, &reply);
		if (status.ok()) {
			std::cout << LogPrefix() << "[RECV] StreamoutSetPort success=" << (reply.success() ? "true" : "false")
			          << ", message=" << reply.message() << std::endl;
			return reply.message();
		} else {
			std::cout << LogPrefix() << "[RECV] StreamoutSetPort RPC failed: " << status.error_message() << std::endl;
			return "RPC failed: " + status.error_message();
		}
	}

	std::string SetStreamPipeline(const std::string& pipeline) {
		StreamoutSetPipelineRequest request;
		request.set_pipeline(pipeline);
		std::cout << LogPrefix() << "[SEND] StreamoutSetPipeline pipeline=" << request.pipeline() << std::endl;
		StreamoutActionResponse reply;
		std::shared_ptr<StreamoutService::Stub> stub;
		if (!PrepareMainStub(stub)) {
			return "RPC failed: unable to connect to server";
		}
		ClientContext context;
		ConfigureUnaryContext(context);
		Status status = stub->StreamoutSetPipeline(&context, request, &reply);
		if (status.ok()) {
			std::cout << LogPrefix() << "[RECV] StreamoutSetPipeline success=" << (reply.success() ? "true" : "false")
			          << ", message=" << reply.message() << std::endl;
			return reply.message();
		} else {
			std::cout << LogPrefix() << "[RECV] StreamoutSetPipeline RPC failed: " << status.error_message() << std::endl;
			return "RPC failed: " + status.error_message();
		}
	}

	std::string Start(int arg) {
		StreamoutStartRequest request;
		request.set_arg(arg);
		std::cout << LogPrefix() << "[SEND] StreamoutStart arg=" << request.arg() << std::endl;
		StreamoutActionResponse reply;
		std::shared_ptr<StreamoutService::Stub> stub;
		if (!PrepareMainStub(stub)) {
			return "RPC failed: unable to connect to server";
		}
		ClientContext context;
		ConfigureUnaryContext(context);
		Status status = stub->StreamoutStart(&context, request, &reply);
		if (status.ok()) {
			std::cout << LogPrefix() << "[RECV] StreamoutStart success=" << (reply.success() ? "true" : "false")
			          << ", message=" << reply.message() << std::endl;
			return reply.message();
		} else {
			std::cout << LogPrefix() << "[RECV] StreamoutStart RPC failed: " << status.error_message() << std::endl;
			return "RPC failed: " + status.error_message();
		}
	}

	std::string Stop(int arg) {
		StreamoutStopRequest request;
		request.set_arg(arg);
		std::cout << LogPrefix() << "[SEND] StreamoutStop arg=" << request.arg() << std::endl;
		StreamoutActionResponse reply;
		std::shared_ptr<StreamoutService::Stub> stub;
		if (!PrepareMainStub(stub)) {
			return "RPC failed: unable to connect to server";
		}
		ClientContext context;
		ConfigureUnaryContext(context);
		Status status = stub->StreamoutStop(&context, request, &reply);
		if (status.ok()) {
			std::cout << LogPrefix() << "[RECV] StreamoutStop success=" << (reply.success() ? "true" : "false")
			          << ", message=" << reply.message() << std::endl;
			return reply.message();
		} else {
			std::cout << LogPrefix() << "[RECV] StreamoutStop RPC failed: " << status.error_message() << std::endl;
			return "RPC failed: " + status.error_message();
		}
	}

	std::string RtspServerDebug(const std::string& debug_string) {
		StreamoutDebugRequest request;
		request.set_debug_string(debug_string);
		std::cout << LogPrefix() << "[SEND] StreamoutServerDebug debug_string="
		          << request.debug_string() << std::endl;
		StreamoutActionResponse reply;
		std::shared_ptr<StreamoutService::Stub> stub;
		if (!PrepareMainStub(stub)) {
			return "RPC failed: unable to connect to server";
		}
		ClientContext context;
		ConfigureUnaryContext(context);
		Status status = stub->StreamoutServerDebug(&context, request, &reply);
		if (status.ok()) {
			std::cout << LogPrefix() << "[RECV] StreamoutServerDebug success=" << (reply.success() ? "true" : "false")
			          << ", message=" << reply.message() << std::endl;
			return reply.message();
		} else {
			std::cout << LogPrefix() << "[RECV] StreamoutServerDebug RPC failed: " << status.error_message() << std::endl;
			return "RPC failed: " + status.error_message();
		}
	}

	std::string SetProductID(int product_id) {
		StreamoutSetProductIdRequest request;
		request.set_id(product_id);
		std::cout << LogPrefix() << "[SEND] StreamoutSetProductId product_id=" << request.id() << std::endl;
		StreamoutActionResponse reply;
		std::shared_ptr<StreamoutService::Stub> stub;
		if (!PrepareMainStub(stub)) {
			return "RPC failed: unable to connect to server";
		}
		ClientContext context;
		ConfigureUnaryContext(context);
		Status status = stub->StreamoutSetProductId(&context, request, &reply);
		if (status.ok()) {
			std::cout << LogPrefix() << "[RECV] StreamoutSetProductId success=" << (reply.success() ? "true" : "false")
			          << ", message=" << reply.message() << std::endl;
			return reply.message();
		} else {
			std::cout << LogPrefix() << "[RECV] StreamoutSetProductId RPC failed: " << status.error_message() << std::endl;
			return "RPC failed: " + status.error_message();
		}
	}

	std::string ListenStreamStatusRqst(int stream_id) {
		return ListenStreamStatusRqst(stream_id, false);
	}

	std::string ListenStreamStatusRqst(int stream_id, bool register_manual_cancel) {
		StreamoutStatusRequest request;
		request.set_id(stream_id);
		std::cout << LogPrefix() << "[SEND] StreamoutWatchStatus id=" << request.id() << std::endl;
		std::shared_ptr<StreamoutService::Stub> stub;
		if (!PrepareMainStub(stub)) {
			return "StreamoutWatchStatus RPC failed: unable to connect to server";
		}

		auto context = std::make_shared<ClientContext>();

		if (register_manual_cancel) {
			std::lock_guard<std::mutex> lk(manual_stream_cancel_mu_);
			manual_stream_context_ = context;
		}

#if CHECK_CONNECTION
		// --- Legacy watchdog: sends SayHello probe every 3s to detect dead server ---
		std::atomic<bool> stream_done{false};
		std::thread connectivity_watchdog([this, context, &stream_done]() {
			while (!stream_done.load()) {
				std::this_thread::sleep_for(std::chrono::seconds(3));
				if (stream_done.load()) {
					break;
				}

					HelloRequest probe_request;
					probe_request.set_name("watchdog");
					HelloReply probe_reply;
					std::shared_ptr<StreamoutService::Stub> probe_stub;
				if (!PrepareMainStub(probe_stub)) {
						std::cout << LogPrefix() << "[WATCHDOG] Server unreachable, cancelling StreamoutWatchStatus" << std::endl;
					break;
				}

				ClientContext probe_context;
				probe_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
				Status probe_status = probe_stub->SayHello(&probe_context, probe_request, &probe_reply);
				if (!probe_status.ok()) {
					std::cout << LogPrefix() << "[WATCHDOG] Probe RPC failed: "
						          << probe_status.error_message() << ", cancelling StreamoutWatchStatus" << std::endl;
					context->TryCancel();
					break;
				}
			}
		});
#endif // CHECK_CONNECTION

		std::unique_ptr<ClientReader<StreamoutStatusResponse>> reader(
			stub->StreamoutWatchStatus(context.get(), request));

		StreamoutStatusResponse update;
		while (reader->Read(&update)) {
			std::cout << LogPrefix() << "[RECV] StreamoutWatchStatus stream_id=" << update.stream_id()
				      << ", code=" << update.status_code()
				      << ", info=" << update.status_info() << std::endl;
		}

#if CHECK_CONNECTION
		stream_done.store(true);
		if (connectivity_watchdog.joinable()) {
			connectivity_watchdog.join();
		}
#endif // CHECK_CONNECTION

		Status status = reader->Finish();

		if (register_manual_cancel) {
			std::lock_guard<std::mutex> lk(manual_stream_cancel_mu_);
			if (manual_stream_context_ == context) {
				manual_stream_context_.reset();
			}
		}

		if (status.ok()) {
			std::cout << LogPrefix() << "[RECV] StreamoutWatchStatus stream closed by server" << std::endl;
			return "StreamoutWatchStatus stream closed by server.";
		}
		std::cout << LogPrefix() << "[RECV] StreamoutWatchStatus RPC failed: " << status.error_message() << std::endl;
		return "StreamoutWatchStatus RPC failed: " + status.error_message();
	}

	bool GetConnStatus() const {
		return conn_status_.load();
	}

	void SetConnStatus(bool status) {
		conn_status_.store(status);
	}

	std::string GetConnState() {
		std::lock_guard<std::mutex> lk(connection_mu_);
		if (!channel_) return "NO_CHANNEL";
		return ConnStateName(channel_->GetState(false));
	}

	void CancelManualStreamStatusListener() {
		std::shared_ptr<ClientContext> context;
		{
			std::lock_guard<std::mutex> lk(manual_stream_cancel_mu_);
			context = manual_stream_context_;
		}
		if (context) {
			std::cout << LogPrefix() << "[SEND] Cancelling manual StreamoutWatchStatus listener" << std::endl;
			context->TryCancel();
		}
	}

private:
	void RecreateChannelLocked() {
		grpc::ChannelArguments args;
		args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);
		args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 3000);
		args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
		args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
		args.SetInt(GRPC_ARG_HTTP2_BDP_PROBE, 0);
		channel_ = grpc::CreateCustomChannel(target_, grpc::InsecureChannelCredentials(), args);
		stub_ = StreamoutService::NewStub(channel_);
	}

	bool EnsureConnectedLocked() {
		auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(800);
		if (channel_ && channel_->WaitForConnected(deadline)) {
			return true;
		}

		std::cout << LogPrefix() << "[CONN] Channel not ready, recreating channel/stubs" << std::endl;
		RecreateChannelLocked();

		deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(2000);
		return channel_->WaitForConnected(deadline);
	}

	bool PrepareMainStub(std::shared_ptr<StreamoutService::Stub>& out_stub) {
		std::lock_guard<std::mutex> lk(connection_mu_);
		if (!EnsureConnectedLocked()) {
			conn_status_.store(false);
			std::cout << LogPrefix() << "[CONN] Server still unreachable" << std::endl;
			return false;
		}
		conn_status_.store(true);
		out_stub = stub_;
		return true;
	}

	void ConfigureUnaryContext(ClientContext& context) {
		// wait_for_ready(false): fail immediately with UNAVAILABLE if channel is down,
		// rather than retrying until the deadline.
		context.set_wait_for_ready(false);
		context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
	}

	static std::string ConnStateName(grpc_connectivity_state state) {
		switch (state) {
			case GRPC_CHANNEL_IDLE: return "IDLE";
			case GRPC_CHANNEL_CONNECTING: return "CONNECTING";
			case GRPC_CHANNEL_READY: return "READY";
			case GRPC_CHANNEL_TRANSIENT_FAILURE: return "TRANSIENT_FAILURE";
			case GRPC_CHANNEL_SHUTDOWN: return "SHUTDOWN";
			default: return "UNKNOWN";
		}
	}

	void StartConnectivityMonitor() {
		conn_monitor_thread_ = std::thread([this]() {
			grpc_connectivity_state last_state = GRPC_CHANNEL_IDLE;
			while (!conn_monitor_stop_.load()) {
				std::shared_ptr<Channel> ch;
				{
					std::lock_guard<std::mutex> lk(connection_mu_);
					ch = channel_;
				}
				if (!ch) {
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
					continue;
				}
				auto state = ch->GetState(false);
				if (state == GRPC_CHANNEL_IDLE) {
					// Force reconnect from IDLE — GetState(true) triggers connection attempt
					state = ch->GetState(true);
				}
				if (state != last_state) {
					std::cout << LogPrefix() << "[CONN_MONITOR] "
					          << ConnStateName(last_state) << " -> "
					          << ConnStateName(state) << std::endl;
					if (state == GRPC_CHANNEL_TRANSIENT_FAILURE || state == GRPC_CHANNEL_SHUTDOWN) {
						conn_status_.store(false);
						std::cout << LogPrefix() << "[CONN_MONITOR] *** SERVER CONNECTION LOST ***" << std::endl;
					} else if (state == GRPC_CHANNEL_READY) {
						conn_status_.store(true);
						std::cout << LogPrefix() << "[CONN_MONITOR] Server connected" << std::endl;
					}
					last_state = state;
				}
				// Wait for state to change, with a short deadline so we can check stop flag
				ch->WaitForStateChange(state,
					std::chrono::system_clock::now() + std::chrono::seconds(1));
			}
		});
	}

	std::string target_;
	std::shared_ptr<Channel> channel_;
	std::shared_ptr<StreamoutService::Stub> stub_;
	std::mutex connection_mu_;
	std::mutex manual_stream_cancel_mu_;
	std::shared_ptr<ClientContext> manual_stream_context_;
	std::atomic<bool> conn_status_{false};
	std::atomic<bool> conn_monitor_stop_{false};
	std::thread conn_monitor_thread_;
};

int main(int argc, char** argv) {
	std::string target_str = "localhost:50051";
	if (argc > 1) {
		target_str = argv[1];
	}
	HelloClient client(target_str);
	bool stream_status_listener_started = false;
	std::atomic<int> stream_status_attempt_count{0};
	std::atomic<bool> force_stream_status_reconnect{false};
	std::atomic<bool> manual_stream_listener_running{false};
	std::mutex stream_status_retry_mu;
	std::condition_variable stream_status_retry_cv;
	std::condition_variable stream_status_attempt_cv;
	std::thread manual_stream_listener_thread;
	while (true) {
	std::cout << "\nSelect an action:\n"
		  << "0. Stop StreamoutWatchStatus listener (choice 9)\n"
		  << "1. SayHello\n"
		  << "2. SetPort\n"
		  << "3. SetStreamPipeline\n"
		  << "4. Start\n"
		  << "5. Stop\n"
		  << "6. StreamoutServerDebug\n"
		  << "7. Start (default arg/id 0)\n"
		  << "8. Stop (default arg/id 0)\n"
		  << "9. Listen StreamoutWatchStatus updates\n"
		  << "10. Get connection state\n"
		  << "11. SetProductID\n"
		  << "q. Quit\n"
		  << "Enter choice: ";
		std::string choice;
		std::getline(std::cin, choice);
		if (choice == "q" || choice == "quit") {
			if (manual_stream_listener_running.load()) {
				client.CancelManualStreamStatusListener();
				if (manual_stream_listener_thread.joinable()) {
					manual_stream_listener_thread.join();
				}
			}
			break;
		}
		if (choice == "0") {
			if (!manual_stream_listener_running.load()) {
				std::cout << "No choice-9 StreamoutWatchStatus listener is running." << std::endl;
			} else {
				client.CancelManualStreamStatusListener();
				if (manual_stream_listener_thread.joinable()) {
					manual_stream_listener_thread.join();
				}
				std::cout << "Stopped choice-9 StreamoutWatchStatus listener." << std::endl;
			}
			continue;
		}
		if (choice == "1") {
			std::string user;
			std::cout << "Enter name for SayHello: ";
			std::getline(std::cin, user);
			std::string reply = client.SayHello(user);
			std::cout << "SayHello response: " << reply << std::endl;
		} else if (choice == "2") {
			std::string port;
			std::cout << "Enter port: ";
			std::getline(std::cin, port);
			std::string reply = client.SetPort(port);
			std::cout << "SetPort response: " << reply << std::endl;
		} else if (choice == "3") {
			std::string pipeline;
			std::cout << "Enter pipeline: ";
			std::getline(std::cin, pipeline);
			std::string reply = client.SetStreamPipeline(pipeline);
			std::cout << "SetStreamPipeline response: " << reply << std::endl;
		} else if (choice == "4") {
			int arg;
			std::cout << "Enter start arg (int): ";
			std::cin >> arg;
			std::cin.ignore();
			std::string reply = client.Start(arg);
			std::cout << "Start response: " << reply << std::endl;
		} else if (choice == "5") {
			int arg;
			std::cout << "Enter stop arg (int): ";
			std::cin >> arg;
			std::cin.ignore();
			std::string reply = client.Stop(arg);
			std::cout << "Stop response: " << reply << std::endl;
		} else if (choice == "6") {
			std::string debug_string;
			std::cout << "Enter debug string: ";
			std::getline(std::cin, debug_string);
			std::string reply = client.RtspServerDebug(debug_string);
			std::cout << "RtspServerDebug response: " << reply << std::endl;
		} else if (choice == "7") {
			int arg = 0;
			if (!stream_status_listener_started) {
				stream_status_listener_started = true;
				std::cout << "Starting StreamoutWatchStatus listener in background..." << std::endl;
				std::thread([&client,
					     &stream_status_attempt_count,
					     &force_stream_status_reconnect,
					     &stream_status_retry_mu,
					     &stream_status_retry_cv,
					     &stream_status_attempt_cv]() {
					int reconnect_delay_ms = 500;
					const int kMaxReconnectDelayMs = 30000;
					int attempt = 1;
					while (true) {
						stream_status_attempt_count.store(attempt);
						stream_status_attempt_cv.notify_all();
						std::cout << "[StreamoutWatchStatus] connect attempt #" << attempt << std::endl;
						std::string reply = client.ListenStreamStatusRqst(0);
						std::cout << reply << std::endl;

						std::cout << "[StreamoutWatchStatus] reconnecting in " << reconnect_delay_ms << "ms" << std::endl;
						std::unique_lock<std::mutex> lk(stream_status_retry_mu);
						bool forced = stream_status_retry_cv.wait_for(
							lk,
							std::chrono::milliseconds(reconnect_delay_ms),
							[&force_stream_status_reconnect]() { return force_stream_status_reconnect.load(); });
						if (forced) {
							force_stream_status_reconnect.store(false);
							reconnect_delay_ms = 500;
						} else {
							reconnect_delay_ms = std::min(reconnect_delay_ms * 2, kMaxReconnectDelayMs);
						}
						attempt++;
					}
				}).detach();
			}

			int prev_attempt = stream_status_attempt_count.load();
			force_stream_status_reconnect.store(true);
			stream_status_retry_cv.notify_all();

			{
				std::mutex wait_mu;
				std::unique_lock<std::mutex> wait_lk(wait_mu);
				stream_status_attempt_cv.wait_for(
					wait_lk,
					std::chrono::milliseconds(200),
					[&stream_status_attempt_count, prev_attempt]() {
						return stream_status_attempt_count.load() > prev_attempt;
					});
			}

			std::string reply = client.Start(arg);
			std::cout << "Start (default 0) response: " << reply << std::endl;
		} else if (choice == "8") {
			int arg = 0;
			std::string reply = client.Stop(arg);
			std::cout << "Stop (default 0) response: " << reply << std::endl;
		} else if (choice == "9") {
			if (manual_stream_listener_running.load()) {
				std::cout << "Choice-9 StreamoutWatchStatus listener already running. Use 0 to stop it first." << std::endl;
				continue;
			}

			int stream_id;
			std::cout << "Enter stream ID for StreamoutWatchStatus: ";
			std::cin >> stream_id;
			std::cin.ignore();
			manual_stream_listener_running.store(true);
			std::cout << "Listening for StreamoutWatchStatus updates in background (use 0 to stop)..." << std::endl;
			manual_stream_listener_thread = std::thread([&client, &manual_stream_listener_running, stream_id]() {
				std::string reply = client.ListenStreamStatusRqst(stream_id, true);
				std::cout << reply << std::endl;
				manual_stream_listener_running.store(false);
			});
		} else if (choice == "10") {
			std::cout << "Connection state: " << client.GetConnState() << std::endl;
		} else if (choice == "11") {
			int product_id;
			std::cout << "Enter product ID (int): ";
			std::cin >> product_id;
			std::cin.ignore();
			std::string reply = client.SetProductID(product_id);
			std::cout << "SetProductID response: " << reply << std::endl;
		} else {
			std::cout << "Invalid choice. Try again." << std::endl;
		}
	}

	if (manual_stream_listener_running.load()) {
		client.CancelManualStreamStatusListener();
		if (manual_stream_listener_thread.joinable()) {
			manual_stream_listener_thread.join();
		}
	}

	if (manual_stream_listener_thread.joinable()) {
		manual_stream_listener_thread.join();
	}

	return 0;
}
