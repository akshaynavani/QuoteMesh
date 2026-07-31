#pragma once

#include <cstdint>
#include <string>

#include "quotemesh/feed_source.hpp"

namespace qm {

struct UdpFeedConfig {
  std::string bind_addr{"0.0.0.0"};
  std::uint16_t port{0};
  std::string multicast_group;  // empty = unicast
  std::string interface_addr{"0.0.0.0"};
  int recv_buffer_bytes{8 << 20};
};

// Non-blocking UDP receiver. Each datagram carries one or more complete wire-format lines.
class UdpFeed final : public FeedSource {
 public:
  explicit UdpFeed(UdpFeedConfig cfg);
  ~UdpFeed() override;

  UdpFeed(const UdpFeed&) = delete;
  UdpFeed& operator=(const UdpFeed&) = delete;

  ssize_t read(char* buf, std::size_t cap) override;
  std::string describe() const override;

 private:
  UdpFeedConfig cfg_;
  int fd_{-1};
};

// Small helper used by the feed simulator to send datagrams.
class UdpSender {
 public:
  UdpSender(const std::string& host, std::uint16_t port, int ttl = 1);
  ~UdpSender();
  UdpSender(const UdpSender&) = delete;
  UdpSender& operator=(const UdpSender&) = delete;

  bool send(const char* data, std::size_t len);

 private:
  int fd_{-1};
  struct Addr;
  Addr* addr_{nullptr};
};

}  // namespace qm
