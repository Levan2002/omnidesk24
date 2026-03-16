#include "transport/tcp_channel.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#ifdef _WIN32
#  pragma comment(lib, "ws2_32.lib")
#endif

namespace omnidesk {

// ---- SocketInitializer ----

bool SocketInitializer::initialized_ = false;

void SocketInitializer::initialize() {
    if (initialized_) return;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    initialized_ = true;
}

void SocketInitializer::shutdown() {
#ifdef _WIN32
    if (initialized_) {
        WSACleanup();
        initialized_ = false;
    }
#endif
}

// ---- Platform helpers ----

static void closeSocket(SocketHandle s) {
    if (s == INVALID_SOCK) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

static int lastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static bool isWouldBlock(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

// ---- TcpChannel ----

TcpChannel::TcpChannel() {
    SocketInitializer::initialize();
    std::memset(&remoteAddr_, 0, sizeof(remoteAddr_));
}

TcpChannel::~TcpChannel() {
    close();
}

TcpChannel::TcpChannel(SocketHandle sock, const sockaddr_storage& addr)
    : sock_(sock), remoteAddr_(addr)
{
    setNonBlocking();
    setNoDelay();
}

TcpChannel::TcpChannel(TcpChannel&& other) noexcept
    : sock_(other.sock_),
      remoteAddr_(other.remoteAddr_),
      recvBuf_(std::move(other.recvBuf_)),
      recvPos_(other.recvPos_)
{
    other.sock_ = INVALID_SOCK;
    other.recvPos_ = 0;
}

TcpChannel& TcpChannel::operator=(TcpChannel&& other) noexcept {
    if (this != &other) {
        close();
        sock_ = other.sock_;
        remoteAddr_ = other.remoteAddr_;
        recvBuf_ = std::move(other.recvBuf_);
        recvPos_ = other.recvPos_;
        other.sock_ = INVALID_SOCK;
        other.recvPos_ = 0;
    }
    return *this;
}

void TcpChannel::setNonBlocking() {
    if (sock_ == INVALID_SOCK) return;
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock_, FIONBIO, &mode);
#else
    int flags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
#endif
}

void TcpChannel::setNoDelay() {
    if (sock_ == INVALID_SOCK) return;
    int flag = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&flag), sizeof(flag));
}

bool TcpChannel::connect(const std::string& host, uint16_t port, int timeoutMs) {
    SocketInitializer::initialize();

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return false;
    }

    sock_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ == INVALID_SOCK) {
        freeaddrinfo(res);
        return false;
    }

    setNonBlocking();

    int ret = ::connect(sock_, res->ai_addr, static_cast<int>(res->ai_addrlen));
    if (ret < 0) {
        int err = lastSocketError();
#ifdef _WIN32
        if (err != WSAEWOULDBLOCK) {
#else
        if (err != EINPROGRESS) {
#endif
            closeSocket(sock_);
            sock_ = INVALID_SOCK;
            freeaddrinfo(res);
            return false;
        }

        // Wait for connection with poll
        struct pollfd pfd{};
        pfd.fd = sock_;
        pfd.events = POLLOUT;

#ifdef _WIN32
        int pollRet = WSAPoll(&pfd, 1, timeoutMs);
#else
        int pollRet = ::poll(&pfd, 1, timeoutMs);
#endif
        if (pollRet <= 0) {
            closeSocket(sock_);
            sock_ = INVALID_SOCK;
            freeaddrinfo(res);
            return false;
        }

        // Check for connection errors
        int sockErr = 0;
        socklen_t errLen = sizeof(sockErr);
        getsockopt(sock_, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&sockErr), &errLen);
        if (sockErr != 0) {
            closeSocket(sock_);
            sock_ = INVALID_SOCK;
            freeaddrinfo(res);
            return false;
        }
    }

    std::memcpy(&remoteAddr_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    setNoDelay();

    // Pre-allocate receive buffer
    recvBuf_.resize(64 * 1024);
    recvPos_ = 0;

    return true;
}

bool TcpChannel::listen(uint16_t port, int backlog) {
    SocketInitializer::initialize();

    sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCK) return false;

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

    if (::listen(sock_, backlog) < 0) {
        close();
        return false;
    }

    setNonBlocking();
    return true;
}

std::unique_ptr<TcpChannel> TcpChannel::accept() {
    if (sock_ == INVALID_SOCK) return nullptr;

    sockaddr_storage clientAddr{};
    socklen_t addrLen = sizeof(clientAddr);

    SocketHandle clientSock = ::accept(sock_,
        reinterpret_cast<struct sockaddr*>(&clientAddr), &addrLen);

    if (clientSock == INVALID_SOCK) return nullptr;

    auto channel = std::unique_ptr<TcpChannel>(new TcpChannel(clientSock, clientAddr));
    channel->recvBuf_.resize(64 * 1024);
    channel->recvPos_ = 0;
    return channel;
}

SocketResult TcpChannel::sendRaw(const void* data, size_t length) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < length) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        auto n = ::send(sock_, reinterpret_cast<const char*>(ptr + sent),
                        static_cast<int>(length - sent), flags);
        if (n < 0) {
            int err = lastSocketError();
            if (isWouldBlock(err)) {
                // Poll until writable
                struct pollfd pfd{};
                pfd.fd = sock_;
                pfd.events = POLLOUT;
#ifdef _WIN32
                int pollRet = WSAPoll(&pfd, 1, 5000);
#else
                int pollRet = ::poll(&pfd, 1, 5000);
#endif
                if (pollRet <= 0) return SocketResult::ERROR;
                continue;
            }
            return SocketResult::ERROR;
        }
        if (n == 0) return SocketResult::DISCONNECTED;
        sent += static_cast<size_t>(n);
    }
    return SocketResult::OK;
}

SocketResult TcpChannel::recvRaw(void* data, size_t length, size_t& bytesRead) {
    auto n = ::recv(sock_, reinterpret_cast<char*>(data),
                    static_cast<int>(length), 0);
    if (n < 0) {
        int err = lastSocketError();
        if (isWouldBlock(err)) {
            bytesRead = 0;
            return SocketResult::WOULD_BLOCK;
        }
        return SocketResult::ERROR;
    }
    if (n == 0) {
        bytesRead = 0;
        return SocketResult::DISCONNECTED;
    }
    bytesRead = static_cast<size_t>(n);
    return SocketResult::OK;
}

SocketResult TcpChannel::send(MessageType type, const void* data, uint32_t length) {
    if (sock_ == INVALID_SOCK) return SocketResult::ERROR;

    std::lock_guard<std::mutex> lock(sendMutex_);

    ControlHeader hdr;
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = static_cast<uint16_t>(type);
    hdr.length = length;

    uint8_t hdrBuf[ControlHeader::SIZE];
    hdr.serialize(hdrBuf);

    auto result = sendRaw(hdrBuf, ControlHeader::SIZE);
    if (result != SocketResult::OK) return result;

    if (length > 0 && data) {
        result = sendRaw(data, length);
    }
    return result;
}

SocketResult TcpChannel::send(MessageType type, const std::vector<uint8_t>& data) {
    return send(type, data.data(), static_cast<uint32_t>(data.size()));
}

SocketResult TcpChannel::recv(MessageType& type, std::vector<uint8_t>& payload) {
    if (sock_ == INVALID_SOCK) return SocketResult::ERROR;

    // Read more data into the receive buffer
    if (recvPos_ < recvBuf_.size()) {
        size_t bytesRead = 0;
        auto result = recvRaw(recvBuf_.data() + recvPos_,
                              recvBuf_.size() - recvPos_, bytesRead);
        if (result == SocketResult::DISCONNECTED) return SocketResult::DISCONNECTED;
        if (result == SocketResult::ERROR) return SocketResult::ERROR;
        recvPos_ += bytesRead;
    }

    // Check if we have a complete header
    if (recvPos_ < ControlHeader::SIZE) {
        return SocketResult::WOULD_BLOCK;
    }

    ControlHeader hdr = ControlHeader::deserialize(recvBuf_.data());
    if (!hdr.valid()) {
        return SocketResult::ERROR;
    }

    size_t totalSize = ControlHeader::SIZE + hdr.length;

    // Check if we have the complete message
    if (recvPos_ < totalSize) {
        // Grow buffer if needed
        if (recvBuf_.size() < totalSize) {
            recvBuf_.resize(totalSize);
        }
        return SocketResult::WOULD_BLOCK;
    }

    // Extract message
    type = static_cast<MessageType>(hdr.type);
    payload.assign(recvBuf_.data() + ControlHeader::SIZE,
                   recvBuf_.data() + totalSize);

    // Shift remaining data to front of buffer
    size_t remaining = recvPos_ - totalSize;
    if (remaining > 0) {
        std::memmove(recvBuf_.data(), recvBuf_.data() + totalSize, remaining);
    }
    recvPos_ = remaining;

    return SocketResult::OK;
}

bool TcpChannel::pollRead(int timeoutMs) {
    if (sock_ == INVALID_SOCK) return false;

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

void TcpChannel::close() {
    if (sock_ != INVALID_SOCK) {
        closeSocket(sock_);
        sock_ = INVALID_SOCK;
    }
    recvPos_ = 0;
}

bool TcpChannel::isOpen() const {
    return sock_ != INVALID_SOCK;
}

std::string TcpChannel::remoteAddress() const {
    char buf[INET6_ADDRSTRLEN] = {};
    if (remoteAddr_.ss_family == AF_INET) {
        auto* a = reinterpret_cast<const sockaddr_in*>(&remoteAddr_);
        inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
    } else if (remoteAddr_.ss_family == AF_INET6) {
        auto* a = reinterpret_cast<const sockaddr_in6*>(&remoteAddr_);
        inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
    }
    return std::string(buf);
}

uint16_t TcpChannel::remotePort() const {
    if (remoteAddr_.ss_family == AF_INET) {
        auto* a = reinterpret_cast<const sockaddr_in*>(&remoteAddr_);
        return ntohs(a->sin_port);
    }
    if (remoteAddr_.ss_family == AF_INET6) {
        auto* a = reinterpret_cast<const sockaddr_in6*>(&remoteAddr_);
        return ntohs(a->sin6_port);
    }
    return 0;
}

std::string TcpChannel::localAddress() const {
    if (sock_ == INVALID_SOCK) return "";
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    getsockname(sock_, reinterpret_cast<struct sockaddr*>(&addr), &len);
    char buf[INET6_ADDRSTRLEN] = {};
    if (addr.ss_family == AF_INET) {
        auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
        inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
    }
    return std::string(buf);
}

uint16_t TcpChannel::localPort() const {
    if (sock_ == INVALID_SOCK) return 0;
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    getsockname(sock_, reinterpret_cast<struct sockaddr*>(&addr), &len);
    if (addr.ss_family == AF_INET) {
        auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
        return ntohs(a->sin_port);
    }
    return 0;
}

} // namespace omnidesk
