#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
#include <set>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <stdexcept>

#include "grpc_str_out.pb.h"
#include "MediaController.h"
#include "DeviceMock.h"

// Include the implementation definition for MediaControllerImpl
#include "../sources/MediaControllerImpl.h"

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

#define  TEST_GRPC_RECONNECT 1

#ifndef TEST_GRPC_RECONNECT
#define USE_MOCK_GRPC_CLIENT 1
#endif

#ifdef USE_MOCK_GRPC_CLIENT
#include "StreamoutGrpcClientMock.h"
#include "StreamInGrpcClientMock.h"
#endif

// gRPC server target — override via command line: ./unit_test <host:port>
static std::string GRPC_TARGET = "10.116.165.104:50052";

// Default stream configuration for tests
static const MediaController::StreamConfiguration DEFAULT_CONFIG = {"0.0.0.0", 8555, "RTSP"};

// Helper: read entire file into a string. Returns empty string and prints a warning on failure.
static std::string readFileToString(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    std::cerr << "  [warn] failed to open " << path << std::endl;
    return {};
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

// Helper: try a list of candidate paths for a cert file (relative to the executable's cwd).
// Returns contents of the first path that opens; empty string if none work.
static std::string loadCert(const std::string& filename) {
  static const std::string kCandidates[] = {
    "certs/",
    "mock/certs/",
    "../mock/certs/",
    "../../mock/certs/",
    "../../../mock/certs/",
  };
  for (const auto& dir : kCandidates) {
    std::ifstream ifs(dir + filename, std::ios::binary);
    if (ifs) {
      std::ostringstream oss;
      oss << ifs.rdbuf();
      std::cout << "  [tls] loaded " << (dir + filename) << std::endl;
      return oss.str();
    }
  }
  std::cerr << "  [warn] could not locate " << filename
            << " (searched certs/, mock/certs/, ../mock/certs/, ...)" << std::endl;
  return {};
}

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
  controller->setStreamInGrpcClient(std::make_unique<StreamInGrpcClientMock>());
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

  // Only one handle per type is allowed. The first create() succeeds;
  // every subsequent create() of the same type must be rejected with
  // ResourceExhausted and return -1.
  MediaController::StreamError lastError = MediaController::StreamError::NoError;
  int errorCallbackCount = 0;
  mc.setGlobalCallbacks({
    .onStreamStatus = nullptr,
    .onStreamError = [&](MediaController::StreamHandle /*h*/, MediaController::StreamError error) {
      lastError = error;
      ++errorCallbackCount;
    },
  });

  auto first = mc.create(MediaController::StreamType::StreamOut);
  ASSERT_TRUE(mc.isValidHandle(first));
  ASSERT_EQ(errorCallbackCount, 0);

  for (int i = 0; i < 5; ++i) {
    auto h = mc.create(MediaController::StreamType::StreamOut);
    ASSERT_EQ(h, MediaController::StreamHandle{-1});
    ASSERT_EQ(lastError, MediaController::StreamError::ResourceExhausted);
  }
  ASSERT_EQ(errorCallbackCount, 5);
}

static void TestStreamInCreateStartStop() {
  auto controller = makeController();
  auto& mc = *controller;

  int statusCount = 0;
  MediaController::StreamStatus lastStatus = MediaController::StreamStatus::Idle;
  mc.setGlobalCallbacks({
    .onStreamStatus = [&](MediaController::StreamHandle /*h*/, MediaController::StreamStatus s) {
      lastStatus = s;
      ++statusCount;
    },
    .onStreamError = nullptr,
  });

  // create()
  std::cout << "*****TestStreamInCreateStartStop: calling create()" << std::endl;

  auto h = mc.create(MediaController::StreamType::StreamIn);
  
  std::cout << "***** TestStreamInCreateStartStop: h=" << h << std::endl;


  ASSERT_TRUE(mc.isValidHandle(h));
  ASSERT_EQ(mc.getStatus(h), MediaController::StreamStatus::Created);
  ASSERT_EQ(lastStatus, MediaController::StreamStatus::Created);
  ASSERT_EQ(statusCount, 1);

  // Second create(StreamIn) must be rejected (one-handle-per-type rule).
  auto dup = mc.create(MediaController::StreamType::StreamIn);
  ASSERT_EQ(dup, MediaController::StreamHandle{-1});

  // start()
  const MediaController::StreamConfiguration cfg{"192.168.1.10", 8554, "RTSP"};
  mc.start(h, cfg);
  ASSERT_EQ(mc.getLastError(h), MediaController::StreamError::NoError);

  // stop()
  mc.stop(h);
  ASSERT_EQ(mc.getStatus(h), MediaController::StreamStatus::Stopping);
  ASSERT_EQ(lastStatus, MediaController::StreamStatus::Stopping);
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

#ifdef USE_MOCK_GRPC_CLIENT
// Verifies MediaControllerImpl::start() converts the list<PairedDevice> in
// StreamConfiguration into a vector<PairedDeviceEntry> and hands the whole
// set to the gRPC client in one call, in the caller's order.
static void TestStreamOutSetPairedDevices() {
  // Build the impl by hand (not via makeController()) so we can keep a raw
  // pointer to the mock after ownership is transferred. The pointer stays
  // valid for the lifetime of the controller.
  auto controller = std::make_shared<MediaControllerImpl>();
  auto mockOwning = std::make_unique<StreamoutGrpcClientMock>();
  auto* mock = mockOwning.get();
  controller->setGrpcClient(std::move(mockOwning));
  controller->setStreamInGrpcClient(std::make_unique<StreamInGrpcClientMock>());
  controller->setDeviceIdProvider(DeviceMock::getDeviceId);

  auto& mc = *controller;
  auto h = mc.create(MediaController::StreamType::StreamOut);
  ASSERT_TRUE(mc.isValidHandle(h));

  MediaController::StreamConfiguration cfg = DEFAULT_CONFIG;
  cfg.pairedDevices = {
    {"device-A", "10.0.0.1", "aa:aa:aa:aa:aa:aa"},
    {"device-B", "10.0.0.2", "bb:bb:bb:bb:bb:bb"},
  };
  mc.start(h, cfg);

  // The mock captures the last vector passed to setPairedDevices().
  // We expect the two entries, in the same order the caller provided them.
  const auto& last = mock->getLastPairedDevices();
  ASSERT_EQ(last.size(), size_t{2});
  ASSERT_EQ(last[0].deviceId,   std::string{"device-A"});
  ASSERT_EQ(last[0].ipAddress,  std::string{"10.0.0.1"});
  ASSERT_EQ(last[0].macAddress, std::string{"aa:aa:aa:aa:aa:aa"});
  ASSERT_EQ(last[1].deviceId,   std::string{"device-B"});
  ASSERT_EQ(last[1].ipAddress,  std::string{"10.0.0.2"});
  ASSERT_EQ(last[1].macAddress, std::string{"bb:bb:bb:bb:bb:bb"});

  mc.stop(h);
}

// Verifies that an empty pairedDevices list still triggers the RPC (the
// contract is "full replacement", so empty == clear all on the server).
static void TestStreamOutSetPairedDevicesEmpty() {
  auto controller = std::make_shared<MediaControllerImpl>();
  auto mockOwning = std::make_unique<StreamoutGrpcClientMock>();
  auto* mock = mockOwning.get();
  controller->setGrpcClient(std::move(mockOwning));
  controller->setStreamInGrpcClient(std::make_unique<StreamInGrpcClientMock>());
  controller->setDeviceIdProvider(DeviceMock::getDeviceId);

  auto& mc = *controller;
  auto h = mc.create(MediaController::StreamType::StreamOut);
  ASSERT_TRUE(mc.isValidHandle(h));

  MediaController::StreamConfiguration cfg = DEFAULT_CONFIG;
  // cfg.pairedDevices left default-empty
  mc.start(h, cfg);

  ASSERT_EQ(mock->getLastPairedDevices().size(), size_t{0});

  mc.stop(h);
}
#endif

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
  RUN_TEST(TestStreamInCreateStartStop);
  RUN_TEST(TestStartStopValidHandle);
  RUN_TEST(TestStartInvalidHandle);
  RUN_TEST(TestStopInvalidHandle);
  RUN_TEST(TestCreateInvalidStreamType);
  RUN_TEST(TestStreamOutSetPairedDevices);
  RUN_TEST(TestStreamOutSetPairedDevicesEmpty);

  TEST_SUMMARY();

#endif

#ifdef TEST_GRPC_RECONNECT
  // --- Reconnect integration test ---
  // This is exactly how the mk2 app uses MediaController:
  {
    {
      auto controller = makeController();
    }
    
    std::cout << "\n=== gRPC Reconnect Test ===" << std::endl;
    std::cout << "Trying initial create()..." << std::endl;

    // set 1 = wait until Ctrl+C (good for reconnect test);
    // set 0 = run scripted start/stop sequence
    int stayforever = 1;

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
    }

    //set 0 for connection loss test, set 1 for normal start/stop test
    if(!stayforever && h != -1)
    {
      mc.start(h, DEFAULT_CONFIG);
      std::cout << "start() called, sleeping 30s..." << std::endl;
      
      
      
      auto status = mc.getStatus(h);
      for(int i = 0; i < 30; ++i) {
        std::cout << "  status=" << static_cast<int>(status) << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        status = mc.getStatus(h);
      }

      std::cout << "status after sleep: " << static_cast<int>(status) << std::endl;
      mc.stop(h);
      std::cout << "stop() called" << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(5));      
      
      std::cout << "start() called, sleeping 30s..." << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(30));
      mc.stop(h);
      std::cout << "stop() called" << std::endl;
    }
    else
    {
      MediaController::StreamConfiguration cfg = DEFAULT_CONFIG;
      // TLS material lives in the certs/ folder (searched in a few relative locations).
      cfg.set_tlscert = loadCert("server.cert.pem");
      cfg.set_tlskey  = loadCert("server.key.pem");
      cfg.set_tlsca   = loadCert("ca.cert.pem");

      // Build the paired-device list StreamOut should trust.
      // Extend / replace with real values as the pairing story firms up.
      cfg.pairedDevices = {
        { /*DeviceId=*/"device-001", /*IPAddress=*/"192.168.1.101", /*MACAddress=*/"00:11:22:33:44:55" },
        { /*DeviceId=*/"device-002", /*IPAddress=*/"192.168.1.102", /*MACAddress=*/"00:11:22:33:44:56" },
      };
      std::cout << "  TLS sizes: cert=" << cfg.set_tlscert.size()
                << " key=" << cfg.set_tlskey.size()
                << " ca=" << cfg.set_tlsca.size()
                << " paired=" << cfg.pairedDevices.size() << std::endl;
      mc.start(h, cfg);

      static std::atomic<bool> stopRequested{false};
      std::signal(SIGINT, [](int) { stopRequested = true; });
      std::cout << "  (press Ctrl+C to continue)" << std::endl;
      while (!stopRequested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      std::signal(SIGINT, SIG_DFL);
    }
    
    std::cout << "Shutting down." << std::endl;

    controller->deinit();
  }
#endif

  return 0;
}
