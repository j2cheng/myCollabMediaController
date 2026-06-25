# MediaControllerImpl.cpp — Implementation Details

## 1. Implemented Functions

`MediaControllerImpl` derives from the abstract base class `MediaController` (defined in `MediaController.h`) and implements all pure virtual methods:

| Function | Description |
|----------|-------------|
| `create(StreamType type)` | Allocates a unique `StreamHandle`, inserts a new `StreamInfo` entry with status `Created`, and fires `onStreamStatus` callback. |
| `start(StreamHandle, StreamConfiguration)` | Validates the handle, stores the configuration, and forwards the start command to the gRPC streamout server. Does **not** change status — status updates come from the lower layer. |
| `stop(StreamHandle)` | Validates the handle, transitions status to `Stopping`, sends `StreamoutStop` via gRPC, and fires `onStreamStatus(Stopping)` callback. Final `Stopped` status arrives asynchronously via `WatchStatus` stream. No-op for invalid or already-stopped handles. |
| `getStatus(StreamHandle)` | Returns current `StreamStatus` for a handle, or `Idle` if the handle doesn't exist. |
| `getLastError(StreamHandle)` | Returns the last `StreamError` for a handle, or `InvalidHandle` if not found. |
| `isValidHandle(StreamHandle)` | Returns `true` if the handle exists in the streams map. |
| `setGlobalCallbacks(GlobalCallbacks)` | Stores the callback structure for status/error notifications. Wraps callbacks with logging — every `onStreamStatus` and `onStreamError` invocation is printed automatically. |
| `deinit()` | Tears down all background threads, disconnects gRPC, clears streams. Safe to call multiple times. |

Additionally:
- `statusToString()` — file-local helper that converts `StreamStatus` enum to a printable string.
- `errorToString()` — file-local helper that converts `StreamError` enum to a printable string.
- `ensureStreamoutConnected()` — lazy-init: creates gRPC client, connects, sets device ID, registers `WatchStatus` callback and opens the watch stream.

## 2. Features Used

### WatchStatus Stream (Server-Streaming RPC)

The `StreamoutWatchStatus` RPC is a server-streaming call that delivers real-time status updates from the streamout server. Key design decisions:

1. **Opened at connection time** (inside `ensureStreamoutConnected()` → `startWatching()`), not at start time. This prevents a race condition where the server sends status before the client opens the watch stream.
2. **Reader created synchronously** in `startWatching()` — the `grpc::ClientReader` is set up under the lock, so it's guaranteed to exist before any start/stop RPCs are issued.
3. **Read loop runs in a background thread** (`watchStatusLoop`) — blocks on `Read()` and dispatches to the status callback.
4. **Status mapping**: server `StreamStatus` enum → `MediaController::StreamStatus` (see TODO note below).

**TODO**: The server currently sends incorrect status codes (e.g. `status_code=0` for "running", `status_code=2` for "stopped"). Client-side mapping needs to be updated to match the server's actual behavior until the server is fixed.

### Mutex (Thread Safety)

All public methods are protected by `std::lock_guard<std::mutex>` on a `mutable std::mutex mutex_` member. This ensures thread-safe access when called from the top-layer controller across multiple threads.

The mutex guards the shared mutable state — specifically `streams_`, `nextHandle_`, `callbacks_`, `deviceId_`, and `grpcClient_`. Any thread accessing these members must hold `mutex_` first.

### Data Tracking

| Member | Type | Purpose |
|--------|------|---------|
| `streams_` | `std::map<StreamHandle, StreamInfo>` | Maps each handle to its type, status, last error, and configuration. |
| `nextHandle_` | `StreamHandle` (int) | Monotonically increasing counter — guarantees unique handles. |
| `callbacks_` | `GlobalCallbacks` | Stores `onStreamStatus` and `onStreamError` function objects. |

### Instantiation

`MediaControllerImpl` is a regular class — instantiate it directly (typically via `std::make_shared<MediaControllerImpl>()`). The application owns the lifetime; multiple instances are allowed.

## 3. Expected State Changes

```
(no handle) ──create()──► Created
                              │
                         start() called (command sent to gRPC server)
                         status remains Created until lower layer reports back
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
         Listening       Connected         Active
        (StreamOut)     (StreamIn)      (data flowing)
              │               │               │
              └───────────────┼───────────────┘
                              │
                         stop() called
                              ▼
                          Stopping
                              │
                              ▼
                           Stopped
```

**Notes:**
- `start()` does **not** transition state locally — it sends the command to the lower-layer gRPC streamout server. The `Listening`/`Connected`/`Active` transitions arrive via the `WatchStatus` server-streaming RPC.
- `stop()` transitions to `Stopping` locally and fires `onStreamStatus(Stopping)`. The final `Stopped` transition arrives asynchronously via `WatchStatus`.
- `Idle` is never stored — it's only returned for non-existent handles.
- The `WatchStatus` stream is opened at **connection time** (during `create()` → `ensureStreamoutConnected()`), not at start time, to avoid the race condition where the server sends status before the watch stream is open.

## 4. Expected Errors

| Error | Triggered When |
|-------|----------------|
| `InvalidHandle` | `start()` called with a handle not in `streams_` map. |
| `AlreadyStarted` | `start()` called on a handle already in `Active` status. |
| `InvalidStreamType` | `create()` called with an unknown `StreamType`. |
| `NetworkError` | `create()` failed — gRPC server unreachable. |
| `StartupFailed` | `start()` — `setPort()` or `startStream()` RPC failed. |
| `StopFailed` | `stop()` — `stopStream()` RPC failed. |
| `NoError` | Default — no error has occurred for this handle. |

## 5. Workflow Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        mk2 App (Top Layer)                              │
└────────────────────────────────┬────────────────────────────────────────┘
                                 │
         ┌───────────────────────┼───────────────────────────┐
         │ 1. std::make_shared<MediaControllerImpl>()        │
         │ 2. setGlobalCallbacks(onStreamStatus, onStreamError)
         │ 3. connect("host:port")                           │
         │ 4. create(StreamOut)  → returns handle            │
         │ 5. start(handle, config)                          │
         │ 6. ... (stream active) ...                        │
         │ 7. stop(handle)                                   │
         │ 8. disconnect()                                   │
         └───────────────────────┼───────────────────────────┘
                                 │
┌────────────────────────────────┼────────────────────────────────────────┐
│                     MediaController (This Module)                        │
│                                │                                         │
│  std::make_shared<MediaControllerImpl>() — instance created (no connection) │
│                                │                                         │
│  setGlobalCallbacks() ──► store onStreamStatus / onStreamError           │
│                                │                                         │
│  connect(target) ─────────────►├──► create gRPC channel + stub           │
│                                │    send StreamoutSetProductId(deviceId)  │
│                                │    setStatusCallback (register handler)  │
│                                │    startWatching() → open WatchStatus    │
│                                │    return success/failure                │
│                                │                                         │
│  create(StreamOut) ───────────►├──► allocate handle, status=Created      │
│                                │    fire onStreamStatus(Created)          │
│                                │                                         │
│  start(handle, config) ───────►├──► validate handle                      │
│                                │    store config                          │
│                                │    send StreamoutStart via gRPC ────────►│
│                                │    (status remains Created)              │
│                                │                                         │
│  ◄── gRPC status update ──────┤◄── lower layer reports Listening/Active  │
│       fire onStreamStatus()    │                                         │
│                                │                                         │
│  stop(handle) ────────────────►├──► send StreamoutStop via gRPC ─────────►│
│                                │    status → Stopping → Stopped          │
│                                │    fire onStreamStatus(Stopping)         │
│                                │    fire onStreamStatus(Stopped)          │
│                                │                                         │
│  disconnect() ────────────────►├──► tear down stub + channel             │
│                                │                                         │
└────────────────────────────────┼────────────────────────────────────────┘
                                 │
┌────────────────────────────────┼────────────────────────────────────────┐
│                  Streamout gRPC Server (Lower Layer)                     │
│                                │                                         │
│  Receives:                     │  Reports back:                          │
│    StreamoutSetProductId ◄─────┤                                         │
│    StreamoutStart ◄────────────┤──► StreamoutWatchStatus (stream)        │
│    StreamoutStop ◄─────────────┤      → READY / CLIENT_CONNECTED /      │
│    StreamoutSetPort ◄──────────┤        STOPPED                          │
│    StreamoutSetPipeline ◄──────┤                                         │
│                                │                                         │
└────────────────────────────────┴────────────────────────────────────────┘
```

## 6. Auto-Reconnect (StreamoutGrpcClient)

The real gRPC client (`StreamoutGrpcClient`) has a built-in auto-reconnect feature, controlled by a single `#define`:

```cpp
// In StreamoutGrpcClient.h — comment out to disable
#define AUTO_GRPC_RECONN
```

### How It Works

```
connect() fails           watchStatusLoop() stream broken
      │                              │
      ▼                              ▼
 startReconnectLoop()          connected_ = false
      │                         startReconnectLoop()
      ▼                              │
 ┌──────────────────────────────────────────────────────┐
 │              reconnectLoop() (background thread)      │
 │                                                       │
 │  while (reconnectRunning_) {                          │
 │      sleep RECONNECT_INTERVAL (5s, interruptible)     │
 │      try CreateChannel + WaitForConnected(3s)         │
 │      if success → connected_ = true, exit loop        │
 │      if fail → log, retry next interval               │
 │  }                                                    │
 └──────────────────────────────────────────────────────┘
         │
    disconnect() or successful reconnect
         │
         ▼
   stopReconnectLoop()
   (sets reconnectRunning_ = false, notifies CV, joins thread)
```

### Configuration

| Constant | Location | Default | Purpose |
|----------|----------|---------|---------|
| `RECONNECT_INTERVAL` | `StreamoutGrpcClient.h` | 5 seconds | Time between retry attempts |
| `AUTO_GRPC_RECONN` | `StreamoutGrpcClient.h` | defined | Master on/off switch |

### Thread Safety

| Member | Protection | Notes |
|--------|-----------|-------|
| `channel_`, `stub_`, `statusCallback_` | `mutex_` | All RPC methods lock before access |
| `connected_` | `std::atomic<bool>` | Lock-free reads in loops and fast-path checks |
| `reconnectRunning_` | `std::atomic<bool>` | Controls reconnect loop lifetime |
| `stateWatchRunning_` | `std::atomic<bool>` | Controls state watcher lifetime; reset at loop exit to allow restart |
| `reconnectCv_` | `reconnectMutex_` | Enables interruptible sleep (clean shutdown) |
| `watchContext_` | `mutex_` | `ClientContext` for cancelling the streaming RPC in `disconnect()` |
| `watchReader_` | `mutex_` | `ClientReader` for the `WatchStatus` stream; created in `startWatching()` |

#### Design Rules (enforced since mutex audit)

1. **Never invoke callbacks under `mutex_`** — copy callback + data under lock, invoke after releasing. Prevents deadlock if callback re-enters the same object.
2. **Never block under `mutex_`** — `WaitForConnected(3s)` is done outside the lock. Channel is created under lock, then wait is lock-free; re-lock to assign on success.
3. **`startWatching()` creates reader synchronously under lock** — the reader is set up in `startWatching()` (called at connection time), guaranteeing it exists before any start/stop RPCs. The `watchStatusLoop()` thread only runs the `Read()` loop — no lock needed for iteration.
4. **`disconnect()` cancels before joining** — calls `watchContext_->TryCancel()` to unblock `reader->Read()`, then joins `watchThread_` outside the lock.
5. **`stateWatchRunning_` resets at loop exit** — allows `startStateWatch()` to work after reconnect (previously caused the watcher to silently not start on 2nd+ reconnect).

**Note:** `reconnectLoop()` does NOT hold `mutex_` during `WaitForConnected(3s)`. Other RPC methods continue to work (they'll fail fast with `!connected_` check). Once reconnect succeeds, it re-locks to assign `channel_`/`stub_`.

### Trigger Points

1. **`connect()` fails** — initial connection timeout → starts reconnect loop automatically
2. **`startStream()` RPC fails** — server died after `create()` but before/during start → marks disconnected, starts reconnect loop
3. **`stopStream()` RPC fails** — server died during active stream → marks disconnected, starts reconnect loop
4. **`watchStatusLoop()` breaks** — server went down while streaming status updates → sets `connected_ = false`, starts reconnect loop
5. **`disconnect()` called** — stops reconnect loop, joins thread, cleans up
6. **`deinit()` called** — calls `disconnect()`, clears streams, resets state

### Shutdown / deinit()

Call `deinit()` for explicit cleanup before destroying the controller:

```cpp
auto controller = std::make_shared<MediaControllerImpl>();
// ... use it ...
controller->deinit();
```

**What it does:**
1. Calls `grpcClient_->disconnect()` — which stops state watcher, reconnect loop, cancels active streaming RPC, and joins all background threads.
2. Clears the `streams_` map and resets `nextHandle_` and `deviceId_`.

**After `deinit()`:** The instance is still alive. Calling `create()` again will trigger `ensureStreamoutConnected()` and reconnect cleanly (full lifecycle restart).

**Safe to call:** Multiple times, from any thread. The gRPC client's `disconnect()` is idempotent.

### Disabling Auto-Reconnect

Comment out the `#define` in `StreamoutGrpcClient.h`:
```cpp
// #define AUTO_GRPC_RECONN
```
This removes all reconnect members and logic at compile time. The top layer (MediaController) can then manage reconnection by calling `create()` again, which triggers `ensureStreamoutConnected()`.

### Testing the Reconnect Feature

In `test/unit_test.cpp`, set:
```cpp
#define TEST_GRPC_RECONNECT 1
```

This switches to the real `StreamoutGrpcClient` and adds a keep-alive loop:

```
=== gRPC Reconnect Test ===
Target: 10.116.165.104:50052
Reconnect interval: 5s
Trying initial create()...
[StreamoutGrpcClient] connect failed: timeout reaching 10.116.165.104:50052
[StreamoutGrpcClient] reconnect loop started (interval=5s)
create() failed (server unreachable) — reconnect loop running in background

Press Enter to quit...

[StreamoutGrpcClient] attempting reconnect to 10.116.165.104:50052
[StreamoutGrpcClient] reconnect failed, retrying in 5s
[StreamoutGrpcClient] attempting reconnect to 10.116.165.104:50052
[StreamoutGrpcClient] reconnected to 10.116.165.104:50052
[StreamoutGrpcClient] reconnect loop ended
```

To test: start the binary, toggle the gRPC server on/off, observe logs. Press Enter to exit cleanly.

## 7. Keep-Alive (gRPC HTTP/2 PING Frames)

Controlled by `#define GRPC_KEEPALIVE 1` in `StreamoutGrpcClient.h`.

Uses **gRPC's built-in HTTP/2 PING** mechanism plus a **channel state watcher** thread that detects when the channel enters `TRANSIENT_FAILURE` and triggers auto-reconnect.

### How It Works

```cpp
// Channel args set during CreateCustomChannel:
GRPC_ARG_KEEPALIVE_TIME_MS         = 10000  // send PING every 10s
GRPC_ARG_KEEPALIVE_TIMEOUT_MS      = 3000   // fail if no response in 3s
GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS = 1 // ping even when no active RPCs
GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA   = 0 // unlimited pings without data
GRPC_ARG_HTTP2_BDP_PROBE                = 0 // disable BDP probing
```

```
                     ┌────────────────────────────────┐
                     │  gRPC HTTP/2 Transport Layer    │
                     │  sends PING every 10s           │
                     │  timeout after 3s → channel     │
                     │  enters TRANSIENT_FAILURE       │
                     └───────────────┬────────────────┘
                                     │
                     ┌───────────────▼────────────────┐
                     │  stateWatchLoop() (our thread)  │
                     │  polls channel_->GetState(true)  │
                     │  every ~1s via WaitForStateChange│
                     │                                 │
                     │  if TRANSIENT_FAILURE/SHUTDOWN:  │
                     │    connected_ = false            │
                     │    startReconnectLoop()          │
                     └─────────────────────────────────┘
```

### Detection Delay

Worst case after server dies: `KEEPALIVE_TIME_MS (10s) + KEEPALIVE_TIMEOUT_MS (3s) + state poll (1s)` = **~14 seconds max**.

To detect faster, reduce `KEEPALIVE_TIME_MS` (e.g., 5000 for ~9s detection).

**Confirmed working:** connect → kill server → `channel state FAILED` detected → reconnect loop runs → restart server → `reconnected`.

### Configuration

| Constant | Location | Default | Purpose |
|----------|----------|---------|---------|
| `KEEPALIVE_TIME_MS` | `StreamoutGrpcClient.h` | 10000 (10s) | Interval between PING frames |
| `KEEPALIVE_TIMEOUT_MS` | `StreamoutGrpcClient.h` | 3000 (3s) | Max wait for PING response |
| `GRPC_KEEPALIVE` | `StreamoutGrpcClient.h` | defined | Master on/off switch |

### Disabling Keep-Alive

```cpp
// #define GRPC_KEEPALIVE 1
```
When disabled, `createChannel()` uses plain `grpc::CreateChannel()` without keepalive args, and no state watcher thread runs. Detection only happens on RPC failure (start/stop).

## 8. Log/Print Reference

All log messages are prefixed with `[StreamoutGrpcClient]`. Here's what to look for:

### Connection

| Log | Meaning |
|-----|---------|
| `creating channel with keepalive (ping=10000ms, timeout=3000ms)` | Channel created with HTTP/2 keepalive args |
| `connected to <target>` | Initial connection succeeded |
| `connect failed: timeout reaching <target>` | Server unreachable on first attempt |
| `channel state watcher started` | Monitoring thread launched (detects server going away) |
| `channel state FAILED (state=N)` | Channel entered TRANSIENT_FAILURE/SHUTDOWN → triggers reconnect |
| `channel state watcher ended` | Monitoring thread exited |
| `disconnected` | Clean disconnect called |

### Auto-Reconnect

| Log | Meaning |
|-----|---------|
| `reconnect loop started (interval=5s)` | Background reconnect thread launched |
| `attempting reconnect to <target>` | Trying to reach server |
| `reconnect failed, retrying in 5s` | Attempt failed, will retry |
| `reconnected to <target>` | Successfully reconnected |
| `reconnect loop ended` | Loop exited (success or shutdown) |

### RPC Operations

| Log | Meaning |
|-----|---------|
| `setProductId=<id> success=<bool>` | Product ID sent to server |
| `startStream success=<bool>` | Start command result |
| `startStream failed: <error>` | Start failed → triggers reconnect |
| `stopStream success=<bool>` | Stop command result |
| `stopStream failed: <error>` | Stop failed → triggers reconnect |
| `setPort failed: <error>` | Port config failed |
| `setPipeline failed: <error>` | Pipeline config failed |
| `watchStatusLoop ended` | Status stream broke (triggers reconnect if unexpected) |

### Typical Scenarios

**Happy path (server always on):**
```
[StreamoutGrpcClient] creating channel with keepalive (ping=10000ms, timeout=3000ms)
[StreamoutGrpcClient] connected to 10.116.165.104:50052
[StreamoutGrpcClient] channel state watcher started
[StreamoutGrpcClient] setProductId=65280 success=1
[StreamoutGrpcClient] WatchStatus stream opened, ready to receive
[MediaController] created handle=1 type=StreamOut status=Created
[MediaController] start handle=1 url=0.0.0.0 port=8555 status=Created
[StreamoutGrpcClient] watchStatusLoop started, waiting for server status...
[StreamoutGrpcClient] startStream success=1
[MediaController] WatchStatus: stream_id=0 status_code=0 status_info="Stream is running main loop...."
```

**Server dies after connect (detected by state watcher ~15s):**
```
[StreamoutGrpcClient] channel state FAILED (state=3)
[StreamoutGrpcClient] channel state watcher ended
[StreamoutGrpcClient] reconnect loop started (interval=5s)
[StreamoutGrpcClient] creating channel with keepalive (ping=10000ms, timeout=3000ms)
[StreamoutGrpcClient] attempting reconnect to 10.116.165.104:50055
[StreamoutGrpcClient] reconnect failed, retrying in 5s
...
[StreamoutGrpcClient] reconnected to 10.116.165.104:50055
[StreamoutGrpcClient] channel state watcher started
[StreamoutGrpcClient] reconnect loop ended
```

**Server unreachable from start:**
```
[StreamoutGrpcClient] creating channel with keepalive (ping=10000ms, timeout=3000ms)
[StreamoutGrpcClient] connect failed: timeout reaching 10.116.165.104:50055
[StreamoutGrpcClient] reconnect loop started (interval=5s)
[StreamoutGrpcClient] attempting reconnect to 10.116.165.104:50055
[StreamoutGrpcClient] reconnect failed, retrying in 5s
...
```

---

### Call Sequence (Happy Path)

```
mk2 App                    MediaController              gRPC Streamout Server
   │                             │                              │
   │── getInstance() ───────────►│                              │
   │── setGlobalCallbacks() ────►│                              │
   │── connect("host:50051") ───►│── CreateChannel ────────────►│
   │                             │── SetProductId(0xff00) ─────►│   │                             │── setStatusCallback() ───────│
   │                             │── startWatching() ──────────►│◄── WatchStatus stream opened   │◄── return true ────────────│◄── success ──────────────────│
   │                             │                              │
   │── create(StreamOut) ───────►│                              │
   │◄── handle=1 ───────────────│                              │
   │◄── onStreamStatus(Created) │                              │
   │                             │                              │
   │── start(1, config) ────────►│── StreamoutStart ───────────►│
   │                             │                              │
   │                             │◄── WatchStatus(Listening) ──│
   │◄── onStreamStatus(Listening)│                              │
   │                             │◄── WatchStatus(Active) ─────│
   │◄── onStreamStatus(Active) ─│                              │
   │                             │                              │
   │── stop(1) ─────────────────►│── StreamoutStop ────────────►│
   │◄── onStreamStatus(Stopping)│                              │
   │◄── onStreamStatus(Stopped) │                              │
   │                             │                              │
   │── disconnect() ────────────►│── close channel ────────────►│
   │                             │                              │
```

## 9. WatchStatus Race Condition Fix

### Problem

The original implementation opened the `WatchStatus` server-streaming RPC inside `startStream()`, **after** the `StreamoutStart` RPC returned. Since the server sends status immediately upon starting, the status was emitted before the watch stream existed — lost in transit.

```
[BEFORE - BROKEN]
startStream() called
  └── StreamoutStart RPC sent → server starts, sends status immediately
  └── startStream() returns success
  └── watchStatusLoop spawned → opens WatchStatus stream (TOO LATE)
       └── blocks on Read() — but status was already sent
```

### Root Cause

The `WatchStatus` stream was treated as a per-operation concern (start watching when starting), but it's actually a **connection-level concern** — it should listen for all status changes for the lifetime of the connection.

### Fix

1. Added `startWatching()` to `StreamoutGrpcClientInterface` — a dedicated method to open the watch stream.
2. `startWatching()` creates the `grpc::ClientReader` **synchronously** (under the lock) before spawning the read thread. This guarantees the reader exists before any RPCs are issued.
3. Called at **connection time** in `ensureStreamoutConnected()`, right after `setStatusCallback()`.
4. Removed all watch thread management from `startStream()`.

```
[AFTER - FIXED]
ensureStreamoutConnected() called (during create())
  └── connect()
  └── setProductId()
  └── setStatusCallback()
  └── startWatching() → opens WatchStatus stream synchronously
       └── spawns Read() thread (already listening)
... later ...
startStream() called
  └── StreamoutStart RPC sent → server starts, sends status
  └── watchStatusLoop receives it immediately via Read()
  └── [MediaController] WatchStatus: stream_id=0 status_code=... ✓
```

### TODO: Server Status Code Mismatch

The server currently sends incorrect `StreamStatus` enum values:

| Server sends | Expected (per proto) | Server's actual meaning |
|---|---|---|
| `status_code=0` (UNSPECIFIED) | N/A | "Stream is running main loop...." |
| `status_code=2` (CLIENT_CONNECTED) | Active | "Stream is not running returning NULL...." (actually stopped) |

Client-side mapping in the status callback needs to be updated to match the server's actual behavior. Server fix is preferred but not currently possible.

