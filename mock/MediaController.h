#pragma once

#include <functional>
#include <memory>
#include <string>
#include <cstdint>
#include <list>

// Forward declaration
class MediaControllerImpl;

/**
 * MediaController
 * 
 * Manages creation, lifecycle, and state of stream in/out components.
 * Provides a bridge between the state machine and underlying media transport layers.
 * 
 * Typical usage:
 *   auto controller = std::make_shared<MediaController>();
 *   controller->setGlobalCallbacks(callbacks);
 *   auto handle = controller->create(MediaController::StreamType::StreamOut);
 *   controller->start(handle, config);
 *   // ... stream active ...
 *   controller->stop(handle);
 */


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

    // Could be multiple paired devices that StreamOut should trust / advertise.
    struct PairedDevice {
        std::string DeviceId;
        std::string IPAddress;
        std::string MACAddress;
    };

    // Stream configuration parameters
    struct StreamConfiguration {
        std::string url;         // IP address (e.g., "127.0.0.1", "0.0.0.0")
        uint16_t port;           // Port number
        std::string protocol;    // Protocol (e.g., "RTSP", "RTP"); optional
        std::string set_tlscert; // TLS certificate (PEM contents or path); used by both StreamIn and StreamOut
        std::string set_tlskey;  // TLS private key (PEM contents or path); used by both StreamIn and StreamOut
        std::string set_tlsca;   // TLS CA bundle (PEM contents or path); used by both StreamIn and StreamOut
        std::list<PairedDevice> pairedDevices; // Paired devices for StreamOut (client identities to trust)
    };

    // Global callback structure for status and error notifications
    struct GlobalCallbacks {
        std::function<void(StreamHandle handle, StreamStatus status)> onStreamStatus;
        std::function<void(StreamHandle handle, StreamError error)> onStreamError;
    };

    // Constructor - now public for instantiation
    MediaController();

    // Destructor
    virtual ~MediaController();

    // Prevent copying but allow moving
    MediaController(const MediaController&) = delete;
    MediaController& operator=(const MediaController&) = delete;
    MediaController(MediaController&&) = default;
    MediaController& operator=(MediaController&&) = default;

    /**
     * Set global callbacks for all stream events.
     * @param callbacks Structure containing callback function pointers
     */
    void setGlobalCallbacks(const GlobalCallbacks& callbacks);

    /**
     * Create a new stream handle and stream object.
     * @param type Stream type (StreamIn or StreamOut)
     * @return Valid StreamHandle on success, or invalid handle on failure
     * @note Call setLastError() on error; use getLastError() to inspect
     */
    StreamHandle create(StreamType type) ;

    /**
     * Start a stream with given configuration.
     * @param handle Stream handle from create()
     * @param configuration Configuration parameters (URL, port, protocol)
     * @note Updates StreamStatus via onStreamStatus callback on success
     * @note Updates StreamError via onStreamError callback on failure
     */
    void start(StreamHandle handle, const StreamConfiguration& configuration);

    /**
     * Stop an active stream.
     * @param handle Stream handle to stop
     * @note Updates StreamStatus via onStreamStatus callback on completion
     * @note Safe to call on already-stopped streams (no-op)
     */
    void stop(StreamHandle handle) ;

    /**
     * Query current status of a stream.
     * @param handle Stream handle
     * @return Current StreamStatus
     */
    StreamStatus getStatus(StreamHandle handle) const ;

    /**
     * Query last error encountered for a stream.
     * @param handle Stream handle
     * @return StreamError code; NoError if no error occurred
     */
    StreamError getLastError(StreamHandle handle) const;

    /**
     * Check if a stream handle is valid.
     * @param handle Stream handle to validate
     * @return true if handle represents an active/valid stream
     */
    bool isValidHandle(StreamHandle handle) const;

private:
    std::unique_ptr<MediaControllerImpl> impl_;
};
