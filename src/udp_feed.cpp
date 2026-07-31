#include "quotemesh/udp_feed.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace qm {
namespace {

[[noreturn]] void fail(const std::string& what) {
  throw std::runtime_error(what + ": " + std::strerror(errno));
}

in_addr parse_ipv4(const std::string& s) {
  in_addr a{};
  if (inet_pton(AF_INET, s.c_str(), &a) != 1) throw std::runtime_error("bad IPv4 address: " + s);
  return a;
}

}  // namespace

UdpFeed::UdpFeed(UdpFeedConfig cfg) : cfg_(std::move(cfg)) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) fail("socket");

  const int one = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &cfg_.recv_buffer_bytes, sizeof cfg_.recv_buffer_bytes);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg_.port);
  addr.sin_addr = cfg_.multicast_group.empty() ? parse_ipv4(cfg_.bind_addr)
                                               : in_addr{htonl(INADDR_ANY)};
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    ::close(fd_);
    fail("bind port " + std::to_string(cfg_.port));
  }

  if (!cfg_.multicast_group.empty()) {
    ip_mreq mreq{};
    mreq.imr_multiaddr = parse_ipv4(cfg_.multicast_group);
    mreq.imr_interface = parse_ipv4(cfg_.interface_addr);
    if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0) {
      ::close(fd_);
      fail("join multicast " + cfg_.multicast_group);
    }
  }

  const int flags = ::fcntl(fd_, F_GETFL, 0);
  ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
}

UdpFeed::~UdpFeed() {
  if (fd_ >= 0) ::close(fd_);
}

ssize_t UdpFeed::read(char* buf, std::size_t cap) {
  // Drain as many datagrams as fit; datagram boundaries always coincide with line boundaries.
  std::size_t used = 0;
  while (cap - used >= 1500) {
    const ssize_t n = ::recv(fd_, buf + used, cap - used, 0);
    if (n > 0) {
      used += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
    if (n < 0) return used > 0 ? static_cast<ssize_t>(used) : -1;
    break;
  }
  return static_cast<ssize_t>(used);
}

std::string UdpFeed::describe() const {
  return "udp:" + (cfg_.multicast_group.empty() ? cfg_.bind_addr : cfg_.multicast_group) + ":" +
         std::to_string(cfg_.port);
}

struct UdpSender::Addr {
  sockaddr_in sa;
};

UdpSender::UdpSender(const std::string& host, std::uint16_t port, int ttl) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) fail("socket");
  const auto t = static_cast<unsigned char>(ttl);
  ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &t, sizeof t);
  addr_ = new Addr{};
  addr_->sa.sin_family = AF_INET;
  addr_->sa.sin_port = htons(port);
  addr_->sa.sin_addr = parse_ipv4(host);
}

UdpSender::~UdpSender() {
  if (fd_ >= 0) ::close(fd_);
  delete addr_;
}

bool UdpSender::send(const char* data, std::size_t len) {
  return ::sendto(fd_, data, len, 0, reinterpret_cast<const sockaddr*>(&addr_->sa),
                  sizeof addr_->sa) == static_cast<ssize_t>(len);
}

}  // namespace qm
