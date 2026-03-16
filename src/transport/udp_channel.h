#pragma once

#include "core/types.h"
#include "transport/protocol.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using SocketHandle = SOCKET;
   constexpr SocketHandle INVALID_SOCK_UDP = INVALID_SOCKET;
#else
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <errno.h>
   using SocketHandle = int;
   constexpr SocketHandle INVALID_SOCK_UDP = -1;
#endif

namespace omnidesk {

// Non-blocking UDP channel. MTU-safe with 1200-byte max payloads.
class UdpChannel {
public:
    UdpChannel();
    ~UdpChannel();

    UdpChannel(const UdpChannel&) = delete;
    UdpChannel& operator=(const UdpChannel&) = delete;
    UdpChannel(UdpChannel&& other) noexcept;
    UdpChannel& operator=(UdpChannel&& other) noexcept;

    // Bind to a local port. Pass 0 for OS-assigned port.
    bool bind(uint16_t port = 0);

    // Send data to a specific peer. Data must not exceed MAX_UDP_PAYLOAD.
    bool sendTo(const PeerAddress& peer, const void* data, size_t length);
    bool sendTo(const PeerAddress& peer, const std::vector<uint8_t>& data);

    // Receive data. Returns number of bytes received, 0 if nothing available.
    // Populates sender address.
    size_t recvFrom(void* data, size_t maxLength, PeerAddress& sender);
    size_t recvFrom(std::vector<uint8_t>& data, PeerAddress& sender);

    // Poll for incoming data.
    bool pollRead(int timeoutMs = 0);

    // Close the socket.
    void close();

    // Check if socket is open.
    bool isOpen() const;

    // Get the local port (useful when bound to port 0).
    uint16_t localPort() const;

    // Get the local address.
    std::string localAddress() const;

    // "Connect" the UDP socket to a specific peer (for convenience).
    // After this, send() and recv() can be used without addresses.
    bool connectTo(const PeerAddress& peer);

    // Send to the connected peer.
    bool send(const void* data, size_t length);

    // Receive from the connected peer.
    size_t recv(void* data, size_t maxLength);

private:
    void setNonBlocking();
    bool resolvePeer(const PeerAddress& peer, sockaddr_in& addr) const;

    SocketHandle sock_ = INVALID_SOCK_UDP;
    bool connected_ = false;
    mutable std::mutex sendMutex_;
};

} // namespace omnidesk
