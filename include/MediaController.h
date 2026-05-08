#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include "types.h"

namespace AirMedia {
class MediaController {
public:
    // Stream type enumeration
    enum class StreamType {
        StreamIn,
        StreamOut
    };

    // Stream lifecycle states
    enum class StreamStatus {
        Idle,           // Initial state, no stream created
        Created,        // Stream object created, not started
        Connected,      // For StreamIn: successfully connected to peer
        Listening,      // For StreamOut: server listening, ready for connections
        Active,         // Stream actively carrying data
        Stopping,       // Stream shutdown in progress
        Stopped,        // Stream stopped, cleanup complete
        Error           // Error state, see getLastError()
    };

    // Stream error types
    enum class StreamError {
        NoError = 0,
        InvalidHandle,
        InvalidConfiguration,
        ConnectionFailed,
        ConnectionTimeout,
        AlreadyStarted,
        NotStarted,
        StartupFailed,
        StopFailed,
        NetworkError,
        PortInUse,
        InvalidStreamType,
        ResourceExhausted,
        Unknown
    };

    // Handle type for stream identification
    using StreamHandle = int;

    // Configuration for StreamIn (RTSP Client connecting to a remote server)
    struct StreamInConfiguration {
        std::string serverUrl;  // Remote server address (e.g., "192.168.1.100")
        uint16_t port;          // Remote server port
        std::string protocol;   // Protocol (e.g., "RTSP", "RTP")
    };

    // Configuration for StreamOut (RTSP Server listening for incoming connections)
    struct StreamOutConfiguration {
        std::string listenAddress; // Local bind address (e.g., "0.0.0.0")
        uint16_t port;             // Local port to listen on
        std::string protocol;      // Protocol (e.g., "RTSP", "RTP")
    };

    // Global callback structure for status and error notifications
    struct GlobalCallbacks {
        std::function<void(StreamHandle handle, StreamStatus status)> onStreamStatus;
        std::function<void(StreamHandle handle, StreamError error)> onStreamError;
    };

    // Singleton access
    static MediaController& getInstance();

    // Destructor
    virtual ~MediaController() = default;

    // Prevent copying and moving
    MediaController(const MediaController&) = delete;
    MediaController& operator=(const MediaController&) = delete;
    MediaController(MediaController&&) = delete;
    MediaController& operator=(MediaController&&) = delete;

    /**
     * Set global callbacks for all stream events.
     * @param callbacks Structure containing callback function pointers
     */
    virtual void setGlobalCallbacks(const GlobalCallbacks& callbacks) = 0;

    /**
     * Create a new stream handle and stream object.
     * @param type Stream type (StreamIn or StreamOut)
     * @return Valid StreamHandle on success, or invalid handle on failure
     * @note Call setLastError() on error; use getLastError() to inspect
     */
    virtual StreamHandle create(StreamType type) = 0;

    /**
     * Start a StreamIn (RTSP Client) with given configuration.
     * @param handle Stream handle from create(StreamType::StreamIn)
     * @param configuration Client configuration (server URL, port, protocol)
     */
    virtual void start(StreamHandle handle, const StreamInConfiguration& configuration) = 0;

    /**
     * Start a StreamOut (RTSP Server) with given configuration.
     * @param handle Stream handle from create(StreamType::StreamOut)
     * @param configuration Server configuration (listen address, port, protocol)
     */
    virtual void start(StreamHandle handle, const StreamOutConfiguration& configuration) = 0;

    /**
     * Stop an active stream.
     * @param handle Stream handle to stop
     * @note Updates StreamStatus via onStreamStatus callback on completion
     * @note Safe to call on already-stopped streams (no-op)
     */
    virtual void stop(StreamHandle handle) = 0;

    /**
     * Query current status of a stream.
     * @param handle Stream handle
     * @return Current StreamStatus
     */
    virtual StreamStatus getStatus(StreamHandle handle) const = 0;

    /**
     * Query last error encountered for a stream.
     * @param handle Stream handle
     * @return StreamError code; NoError if no error occurred
     */
    virtual StreamError getLastError(StreamHandle handle) const = 0;

    /**
     * Check if a stream handle is valid.
     * @param handle Stream handle to validate
     * @return true if handle represents an active/valid stream
     */
    virtual bool isValidHandle(StreamHandle handle) const = 0;

protected:
    // Protected constructor for singleton pattern and testing
    MediaController() = default;

};

}