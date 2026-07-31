#pragma once

#include <cstddef>
#include <string>
#include <sys/types.h>

namespace qm {

// A byte-stream source of wire-format messages for a single exchange.
// Implementations must be safe to call repeatedly from one feed-handler thread.
class FeedSource {
 public:
  virtual ~FeedSource() = default;

  // Copy up to `cap` bytes of wire data into `buf`.
  // Returns >0 bytes read, 0 if nothing is available right now, or -1 if the source is closed.
  virtual ssize_t read(char* buf, std::size_t cap) = 0;

  virtual std::string describe() const = 0;
};

}  // namespace qm
