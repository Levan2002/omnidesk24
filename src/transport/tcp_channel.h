#pragma once

#include "core/types.h"
#include "transport/protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using SocketHandle = SOCKET;
   constexpr SocketHandle INVALID_SOCK = INVALID_SOCKET;
#else
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <errno.h>
   using SocketHandle = int;
   constexpr SocketHandle INVALID_SOCK = -1;
#endif

namespace omnidesk {

// Result codes for socket operations
enum class SocketResult {
    OK,
    WOULD_BLOCK,
    DISCONNECTED,
    ERROR,
};

// Callback for incoming messages
using MessageCallback = std::function<void(MessageType type, const std::vector<uint8_t>& payload)>;

// Non-blocking TCP channel that sends/receives ControlHeader-prefixed messages.
class TcpChannel {
public:
    TcpChannel();
    ~TcpChannel();

    TcpChannel(const TcpChannel&) = delete;
    TcpChannel& operator=(const TcpChannel&) = delete;
    TcpChannel(TcpChannel&& other) noexcept;
    TcpChannel& operator=(TcpChannel&& other) noexcept;

    // Client: connect to remote host. Returns true on success.
    bool connect(const std::string& host, uint16_t port, int timeoutMs = 5000);

    // Server: bind and listen on port. Returns true on success.
    bool listen(uint16_t port, int backlog = 5);

    // Server: accept incoming connection. Returns new TcpChannel or nullptr.
    std::unique_ptr<TcpChannel> accept();

    // Send a message with ControlHeader prefix. Thread-safe.
    SocketResult send(MessageType type, const void* data, uint32_t length);
    SocketResult send(MessageType type, const std::vector<uint8_t>& data);

    // Try to receive a complete message. Returns OK if a message was received.
    // On OK, populates type and payload.
    SocketResult recv(MessageType& type, std::vector<uint8_t>& payload);

    // Poll for readability (incoming data). Returns true if data is available.
    bool pollRead(int timeoutMs = 0);

    // Close the connection.
    void close();

    // Check if the socket is open.
    bool isOpen() const;

    // Get the remote address as string.
    std::string remoteAddress() const;
    uint16_t remotePort() const;

    // Get the local address as string.
    std::string localAddress() const;
    uint16_t localPort() const;

private:
    // Construct from an already-connected socket (used by accept()).
    explicit TcpChannel(SocketHandle sock, const sockaddr_storage& addr);

    void setNonBlocking();
    void setNoDelay();
    SocketResult sendRaw(const void* data, size_t length);
    SocketResult recvRaw(void* data, size_t length, size_t& bytesRead);

    SocketHandle sock_ = INVALID_SOCK;
    sockaddr_storage remoteAddr_{};
    mutable std::mutex sendMutex_;

    // Receive buffer for partial message reassembly
    std::vector<uint8_t> recvBuf_;
    size_t recvPos_ = 0;
};

// RAII initializer for Winsock on Windows (no-op on Linux).
class SocketInitializer {
public:
    static void initialize();
    static void shutdown();
private:
    static bool initialized_;
};

} // namespace omnidesk
