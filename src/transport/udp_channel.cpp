#include "transport/udp_channel.h"
#include "transport/tcp_channel.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#ifdef _WIN32
#  pragma comment(lib, "ws2_32.lib")
#endif

namespace omnidesk {

// ---- Platform helpers ----

static void closeUdpSocket(SocketHandle s) {
    if (s == INVALID_SOCK_UDP) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

static int lastUdpError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static bool isUdpWouldBlock(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

// ---- UdpChannel ----

UdpChannel::UdpChannel() {
    SocketInitializer::initialize();
}

UdpChannel::~UdpChannel() {
    close();
}

UdpChannel::UdpChannel(UdpChannel&& other) noexcept
    : sock_(other.sock_),
      connected_(other.connected_)
{
    other.sock_ = INVALID_SOCK_UDP;
    other.connected_ = false;
}

UdpChannel& UdpChannel::operator=(UdpChannel&& other) noexcept {
    if (this != &other) {
        close();
        sock_ = other.sock_;
        connected_ = other.connected_;
        other.sock_ = INVALID_SOCK_UDP;
        other.connected_ = false;
    }
    return *this;
}

void UdpChannel::setNonBlocking() {
    if (sock_ == INVALID_SOCK_UDP) return;
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock_, FIONBIO, &mode);
#else
    int flags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
#endif
}

bool UdpChannel::resolvePeer(const PeerAddress& peer, sockaddr_in& addr) const {
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(peer.port);

    if (inet_pton(AF_INET, peer.host.c_str(), &addr.sin_addr) == 1) {
        return true;
    }

    // Try DNS resolution
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(peer.host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        return false;
    }

    auto* sa = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    addr.sin_addr = sa->sin_addr;
    freeaddrinfo(res);
    return true;
}

bool UdpChannel::bind(uint16_t port) {
    SocketInitializer::initialize();

    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCK_UDP) return false;

    int reuse = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close();
        return false;
    }

    setNonBlocking();
    return true;
}

bool UdpChannel::sendTo(const PeerAddress& peer, const void* data, size_t length) {
    if (sock_ == INVALID_SOCK_UDP) return false;
    if (length > MAX_UDP_PAYLOAD) return false;

    sockaddr_in addr{};
    if (!resolvePeer(peer, addr)) return false;

    std::lock_guard<std::mutex> lock(sendMutex_);

    auto n = ::sendto(sock_, reinterpret_cast<const char*>(data),
                      static_cast<int>(length), 0,
                      reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    return n >= 0;
}

bool UdpChannel::sendTo(const PeerAddress& peer, const std::vector<uint8_t>& data) {
    return sendTo(peer, data.data(), data.size());
}

size_t UdpChannel::recvFrom(void* data, size_t maxLength, PeerAddress& sender) {
    if (sock_ == INVALID_SOCK_UDP) return 0;

    sockaddr_in addr{};
    socklen_t addrLen = sizeof(addr);

    auto n = ::recvfrom(sock_, reinterpret_cast<char*>(data),
                        static_cast<int>(maxLength), 0,
                        reinterpret_cast<struct sockaddr*>(&addr), &addrLen);

    if (n <= 0) return 0;

    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    sender.host = buf;
    sender.port = ntohs(addr.sin_port);

    return static_cast<size_t>(n);
}

size_t UdpChannel::recvFrom(std::vector<uint8_t>& data, PeerAddress& sender) {
    data.resize(MAX_UDP_PAYLOAD + VideoHeader::SIZE);
    size_t n = recvFrom(data.data(), data.size(), sender);
    data.resize(n);
    return n;
}

bool UdpChannel::pollRead(int timeoutMs) {
    if (sock_ == INVALID_SOCK_UDP) return false;

    struct pollfd pfd{};
    pfd.fd = sock_;
    pfd.events = POLLIN;

#ifdef _WIN32
    int ret = WSAPoll(&pfd, 1, timeoutMs);
#else
    int ret = ::poll(&pfd, 1, timeoutMs);
#endif
    return ret > 0 && (pfd.revents & POLLIN);
}

void UdpChannel::close() {
    if (sock_ != INVALID_SOCK_UDP) {
        closeUdpSocket(sock_);
        sock_ = INVALID_SOCK_UDP;
    }
    connected_ = false;
}

bool UdpChannel::isOpen() const {
    return sock_ != INVALID_SOCK_UDP;
}

uint16_t UdpChannel::localPort() const {
    if (sock_ == INVALID_SOCK_UDP) return 0;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(sock_, reinterpret_cast<struct sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
}

std::string UdpChannel::localAddress() const {
    if (sock_ == INVALID_SOCK_UDP) return "";
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(sock_, reinterpret_cast<struct sockaddr*>(&addr), &len);
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return std::string(buf);
}

bool UdpChannel::connectTo(const PeerAddress& peer) {
    if (sock_ == INVALID_SOCK_UDP) return false;

    sockaddr_in addr{};
    if (!resolvePeer(peer, addr)) return false;

    if (::connect(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        return false;
    }

    connected_ = true;
    return true;
}

bool UdpChannel::send(const void* data, size_t length) {
    if (sock_ == INVALID_SOCK_UDP || !connected_) return false;
    if (length > MAX_UDP_PAYLOAD) return false;

    std::lock_guard<std::mutex> lock(sendMutex_);

    auto n = ::send(sock_, reinterpret_cast<const char*>(data),
                    static_cast<int>(length), 0);
    return n >= 0;
}

size_t UdpChannel::recv(void* data, size_t maxLength) {
    if (sock_ == INVALID_SOCK_UDP) return 0;

    auto n = ::recv(sock_, reinterpret_cast<char*>(data),
                    static_cast<int>(maxLength), 0);
    if (n <= 0) return 0;
    return static_cast<size_t>(n);
}

} // namespace omnidesk
