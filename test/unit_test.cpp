#include <cassert>
#include <iostream>
#include <set>
#include <thread>
#include <chrono>
#include <csignal>
#include <stdexcept>

#include "grpc_str_out.pb.h"
#include "MediaController.h"
#include "DeviceMock.h"

// ---------- Colored test output (gtest style) ----------
#define CLR_RED     "\033[1;31m"
#define CLR_GREEN   "\033[1;32m"
#define CLR_YELLOW  "\033[1;33m"
#define CLR_CYAN    "\033[1;36m"
#define CLR_RESET   "\033[0m"

static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define ASSERT_TRUE(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << CLR_RED << "    ASSERTION FAILED: " << CLR_RESET \
                << #expr << "\n    at " << __FILE__ << ":" << __LINE__ << std::endl; \
      throw std::runtime_error(#expr); \
    } \
  } while(0)

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

#define RUN_TEST(testFunc) \
  do { \
    g_tests_run++; \
    std::cout << CLR_CYAN << "[ RUN      ] " << CLR_RESET << #testFunc << std::endl; \
    try { \
      testFunc(); \
      g_tests_passed++; \
      std::cout << CLR_GREEN << "[       OK ] " << CLR_RESET << #testFunc << std::endl; \
    } catch (const std::exception& e) { \
      std::cout << CLR_RED << "[  FAILED  ] " << CLR_RESET << #testFunc << std::endl; \
    } \
  } while(0)

#define TEST_SUMMARY() \
  do { \
    std::cout << std::endl; \
    std::cout << CLR_CYAN << "[==========]" << CLR_RESET \
              << " " << g_tests_run << " test(s) ran." << std::endl; \
    if (g_tests_passed == g_tests_run) { \
      std::cout << CLR_GREEN << "[  PASSED  ]" << CLR_RESET \
                << " " << g_tests_passed << " test(s)." << std::endl; \
    } else { \
      std::cout << CLR_GREEN << "[  PASSED  ]" << CLR_RESET \
                << " " << g_tests_passed << " test(s)." << std::endl; \
      std::cout << CLR_RED << "[  FAILED  ]" << CLR_RESET \
                << " " << (g_tests_run - g_tests_passed) << " test(s)." << std::endl; \
    } \
  } while(0)

// #define  TEST_GRPC_RECONNECT 1

#ifndef TEST_GRPC_RECONNECT
#define USE_MOCK_GRPC_CLIENT 1
#endif

#ifdef USE_MOCK_GRPC_CLIENT
#include "StreamoutGrpcClientMock.h"
#endif

// gRPC server target — override via command line: ./unit_test <host:port>
static std::string GRPC_TARGET = "10.116.165.105:50052";

// Default stream configuration for tests
static const MediaController::StreamConfiguration DEFAULT_CONFIG = {"0.0.0.0", 8555, "RTSP"};

// Helper: create a configured controller for tests.
#ifdef TEST_GRPC_RECONNECT
static std::shared_ptr<MediaController> makeController() {
  auto controller = std::make_shared<MediaController>();
  return controller;
}
#else
static std::shared_ptr<MediaControllerImpl> makeController() {
  auto controller = std::make_shared<MediaControllerImpl>();
#ifdef USE_MOCK_GRPC_CLIENT
  controller->setGrpcClient(std::make_unique<StreamoutGrpcClientMock>());
  controller->setDeviceIdProvider(DeviceMock::getDeviceId);
#endif
  return controller;
}
#endif

// Fake service logic — tests SayHello without any gRPC dependency.
class FakeIntStreamoutSvcStrl {
public:
  bool SayHello(const streamout::v1::HelloRequest& request,
                streamout::v1::HelloReply* reply) {
    reply->set_message("Hello " + request.name());
    return true;
  }
};

static void TestSayHelloNoServer() {
  FakeIntStreamoutSvcStrl service;

  streamout::v1::HelloRequest req;
  streamout::v1::HelloReply reply;

  req.set_name("Tester");

  bool ok = service.SayHello(req, &reply);

  ASSERT_TRUE(ok);
  ASSERT_EQ(reply.message(), "Hello Tester");
}

static void TestMediaControllerDirect() {
  // Create controller directly (concrete implementation via interface)
  auto controller = std::make_shared<MediaController>();
  auto& mc = *controller;
  mc.setGlobalCallbacks({
    .onStreamStatus = [](MediaController::StreamHandle handle,
                         MediaController::StreamStatus status) {
      std::cout << "  [direct] callback: handle=" << handle << " status=" << static_cast<int>(status) << std::endl;
    },
    .onStreamError = [](MediaController::StreamHandle handle,
                        MediaController::StreamError error) {
      std::cout << "  [direct] callback: handle=" << handle << " error=" << static_cast<int>(error) << std::endl;
    },
  });
  auto handle = mc.create(MediaController::StreamType::StreamOut);
  mc.start(handle, DEFAULT_CONFIG);

  std::cout << "start() called, sleeping 4s..." << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(2));

  mc.stop(handle);
}

static void TestMediaController() {
  auto controller = makeController();
  auto& mc = *controller;

  // Set up callbacks
  mc.setGlobalCallbacks({
    .onStreamStatus = [](MediaController::StreamHandle handle,
                         MediaController::StreamStatus status) {
      std::cout << "  callback: handle=" << handle << " status=" << static_cast<int>(status) << std::endl;
    },
    .onStreamError = [](MediaController::StreamHandle handle,
                        MediaController::StreamError error) {
      std::cout << "  callback: handle=" << handle << " error=" << static_cast<int>(error) << std::endl;
    },
  });

  // Create a StreamOut handle
  auto handle = mc.create(MediaController::StreamType::StreamOut);
  ASSERT_TRUE(mc.isValidHandle(handle));
  ASSERT_EQ(mc.getStatus(handle), MediaController::StreamStatus::Created);

  // Start it
  mc.start(handle, DEFAULT_CONFIG);
  ASSERT_EQ(mc.getStatus(handle), MediaController::StreamStatus::Created);
  ASSERT_EQ(mc.getLastError(handle), MediaController::StreamError::NoError);

  // Stop it
  mc.stop(handle);
  ASSERT_EQ(mc.getStatus(handle), MediaController::StreamStatus::Stopping);
}

static void TestStreamOutCreateReturns() {
  auto controller = makeController();
  auto& mc = *controller;

  const int count = 10;
  std::set<MediaController::StreamHandle> handles;

  for (int i = 0; i < count; ++i) {
    auto h = mc.create(MediaController::StreamType::StreamOut);
    ASSERT_TRUE(handles.find(h) == handles.end());
    handles.insert(h);
  }

  ASSERT_EQ(static_cast<int>(handles.size()), count);
}

static void TestStartStopValidHandle() {
  auto controller = makeController();
  auto& mc = *controller;
  auto h = mc.create(MediaController::StreamType::StreamOut);
  mc.start(h, DEFAULT_CONFIG);
  ASSERT_EQ(mc.getStatus(h), MediaController::StreamStatus::Created);
  ASSERT_EQ(mc.getLastError(h), MediaController::StreamError::NoError);

  mc.stop(h);
  ASSERT_EQ(mc.getStatus(h), MediaController::StreamStatus::Stopping);
}

static void TestStartInvalidHandle() {
  auto controller = makeController();
  auto& mc = *controller;
  MediaController::StreamHandle invalidHandle = 99999;
  bool errorCallbackFired = false;

  mc.setGlobalCallbacks({
    .onStreamStatus = nullptr,
    .onStreamError = [&](MediaController::StreamHandle h, MediaController::StreamError error) {
      errorCallbackFired = true;
      ASSERT_EQ(error, MediaController::StreamError::InvalidHandle);
    },
  });

  mc.start(invalidHandle, DEFAULT_CONFIG);
  ASSERT_TRUE(errorCallbackFired);
  ASSERT_EQ(mc.getStatus(invalidHandle), MediaController::StreamStatus::Idle);
}

static void TestStopInvalidHandle() {
  auto controller = makeController();
  auto& mc = *controller;
  MediaController::StreamHandle invalidHandle = 99999;

  mc.stop(invalidHandle);
  ASSERT_EQ(mc.getStatus(invalidHandle), MediaController::StreamStatus::Idle);
}

static void TestCreateInvalidStreamType() {
  auto controller = makeController();
  auto& mc = *controller;
  bool errorCallbackFired = false;

  mc.setGlobalCallbacks({
    .onStreamStatus = nullptr,
    .onStreamError = [&](MediaController::StreamHandle h, MediaController::StreamError error) {
      errorCallbackFired = true;
      ASSERT_EQ(error, MediaController::StreamError::InvalidStreamType);
    },
  });

  auto invalidType = static_cast<MediaController::StreamType>(999);
  auto handle = mc.create(invalidType);
  ASSERT_EQ(handle, -1);
  ASSERT_TRUE(errorCallbackFired);
}

int main(int argc, char* argv[]) {
  if (argc > 1) {
    GRPC_TARGET = argv[1];
    std::cout << "Using gRPC target: " << GRPC_TARGET << std::endl;
  }

#ifdef USE_MOCK_GRPC_CLIENT
  // --- Test setup ---
  // Each test creates its own MediaControllerImpl via makeController().
#else
  // --- Reconnect test: simulate mk2 app (only uses MediaController interface) ---
#endif

#ifdef USE_MOCK_GRPC_CLIENT

  RUN_TEST(TestSayHelloNoServer);
  RUN_TEST(TestMediaControllerDirect);
  RUN_TEST(TestMediaController);
  RUN_TEST(TestStreamOutCreateReturns);
  RUN_TEST(TestStartStopValidHandle);
  RUN_TEST(TestStartInvalidHandle);
  RUN_TEST(TestStopInvalidHandle);
  RUN_TEST(TestCreateInvalidStreamType);

  TEST_SUMMARY();

#endif

#ifdef TEST_GRPC_RECONNECT
  // --- Reconnect integration test ---
  // This is exactly how the mk2 app uses MediaController:
  {
    std::cout << "\n=== gRPC Reconnect Test ===" << std::endl;
    std::cout << "Trying initial create()..." << std::endl;

    // mk2 app code:
    auto controller = std::make_shared<MediaControllerImpl>();
    controller->setGrpcTarget(GRPC_TARGET);
    auto& mc = *controller;

    mc.setGlobalCallbacks({
      .onStreamStatus = [](MediaController::StreamHandle handle,
                           MediaController::StreamStatus status) {
        std::cout << "  callback: handle=" << handle << " status=" << static_cast<int>(status) << std::endl;
      },
      .onStreamError = [](MediaController::StreamHandle handle,
                          MediaController::StreamError error) {
        std::cout << "  callback: handle=" << handle << " error=" << static_cast<int>(error) << std::endl;
      },
    });

    auto h = mc.create(MediaController::StreamType::StreamOut);

    if (h == -1) {
      std::cout << "create() failed (server unreachable) — reconnect loop running in background" << std::endl;
    } else {
      std::cout << "create() succeeded, handle=" << h << std::endl;

      mc.start(h, DEFAULT_CONFIG);
      std::cout << "start() called, sleeping 30s..." << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(30));

      mc.stop(h);
      std::cout << "stop() called" << std::endl;
    }

    std::cout << "Shutting down." << std::endl;

    controller->deinit();
  }
#endif

  return 0;
}
