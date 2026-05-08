# gRPC Stream Out Client

This project is a C++ gRPC client for the `IntStreamoutSvcStrl` service defined in `protos/packagehello.proto`. It allows you to interactively send various requests to a gRPC server, including SayHello, SetPort, SetStreamPipeline, Start, Stop, and RtspServerDebug.

## Features
- Interactive menu for all gRPC methods in the proto
- Supports custom and default arguments for Start/Stop
- Easy to extend for new proto methods

## Prerequisites
- C++17 compiler (e.g., g++)
- CMake >= 3.10
- gRPC and Protobuf libraries (development headers and libraries)
- sudo apt install libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc

## Build Instructions
1. Clone or copy the project to your machine.
2. Open a terminal in the project root directory.
3. Run the following commands:

```bash
cmake -S . -B build
cmake --build build
```

This will generate the protobuf/gRPC files and build the client executable.

## How to Run
From the `build` directory, run:

```bash
./client <server_address>:port
```

- Replace `<server_address>` with the gRPC server's address, e.g. `localhost:50051` or `10.116.165.136:50051`.
- If no address is given, it defaults to `localhost:50051`.

## Usage Example
After starting the client, you'll see a menu:

```
Select an action:
1. SayHello
2. SetPort
3. SetStreamPipeline
4. Start
5. Stop
6. RtspServerDebug
7. Start (default port 0)
8. Stop (default port 0)
q. Quit
Enter choice: 
```

Follow the prompts to send requests to the server. Each option will ask for the required input and display the server's response.

## Notes
- The proto file is located at `protos/packagehello.proto`.
- The client source is in `grpc/client.cpp`.
- Make sure the server is running and accessible at the address you provide.

## Connection & Disconnection Behavior

### RPC call types
- **Unary RPCs** (SayHello, SetPort, Start, Stop, etc.) — synchronous/blocking. The calling thread waits until the server replies or the deadline fires.
- **Server-Streaming RPC** (StreamStatusRqst) — `Read()` blocks until the server pushes a message, closes the stream, or the connection drops.

### How the client detects server disconnection

| Scenario | Detection time |
|---|---|
| Server process killed (TCP RST sent) | ~milliseconds |
| Network silently drops packets (no RST) | Up to **5 seconds** (deadline) for unary calls; potentially minutes for streaming without watchdog |

### Unary RPC call flow (e.g. SayHello)

```
SayHello("alice")
    │
    ├─ PrepareMainStub()
    │       ├─ WaitForConnected (800ms)          ← can block up to 800ms here
    │       └─ if fail → recreate channel
    │                  → retry (2000ms)          ← can block up to 2000ms here
    │
    ├─ stub->SayHello()  ──── TCP/HTTP2 ────► server handler
    │   [BLOCKS here]    ◄─── protobuf ─────  returns HelloReply
    │        │
    │        ├─ if channel is known-down → returns UNAVAILABLE immediately  ← wait_for_ready=false
    │        └─ if server slow/unresponsive → blocks up to 2s  ← 2s deadline fires here
    │                                        → returns DEADLINE_EXCEEDED
    │
    └─ return reply.message()  or  error string
```

### `wait_for_ready` and the 2-second deadline
All unary calls set `wait_for_ready(false)` with a 2-second deadline. This means:
- If the channel is in a failed/disconnected state, the call returns `UNAVAILABLE` **immediately** — no waiting.
- If the channel appears up but the server is slow or unresponsive, the call returns `DEADLINE_EXCEEDED` after **2 seconds**.
- **A unary call returns within ~2 seconds worst case when the server is dead.**

### Streaming watchdog (fast disconnect detection)
The `StreamStatusRqst` listener runs a background watchdog thread that probes the server every 3 seconds using a fast unary `SayHello` with a 2-second deadline. If the probe fails, it calls `TryCancel()` on the stream context, immediately unblocking `Read()`. This limits silent disconnect detection to ~5 seconds instead of minutes.

### Reconnect loop
When the `StreamStatusRqst` stream drops (for any reason), the background reconnect loop automatically re-subscribes using exponential backoff (500ms → 1s → 2s → ... up to 30s max).

## License
MIT or your preferred license.



GRPC_VERBOSITY=DEBUG GRPC_TRACE=all ./your_grpc_app

GRPC_VERBOSITY=debug GRPC_TRACE=http_keepalive,connectivity_state ./build/client 10.116.165.104:50052


export GRPC_VERBOSITY=debug
export GRPC_TRACE=http_keepalive,connectivity_state
./build/client 10.116.165.104:50052


Possible outputs:

IDLE — no connection attempt yet
CONNECTING — trying to connect
READY — connected
TRANSIENT_FAILURE — connection lost / server unreachable
SHUTDOWN — channel shut down


READY → IDLE (automatic, server sent GOAWAY or idle timeout)
       ↓
IDLE → CONNECTING (monitor forces reconnect immediately)
       ↓
CONNECTING → READY (if server is reachable)
   or
CONNECTING → TRANSIENT_FAILURE (if server is down)

##restart streamout apk, it will read following configure file for grpc ip:port
adb push configure.txt /data/user/0/com.crestron.streamout/files/.
restart com.crestron.streamout