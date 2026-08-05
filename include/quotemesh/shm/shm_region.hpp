#pragma once

#include <cstddef>
#include <string>

namespace qm::shm {

// RAII wrapper over a POSIX shared memory object (shm_open + mmap).
class ShmRegion {
 public:
  ShmRegion() = default;
  ~ShmRegion();

  ShmRegion(ShmRegion&& o) noexcept;
  ShmRegion& operator=(ShmRegion&& o) noexcept;
  ShmRegion(const ShmRegion&) = delete;
  ShmRegion& operator=(const ShmRegion&) = delete;

  // Create (replacing any stale object with the same name) and map read-write.
  // If `unlink_on_close` the object is removed when this region is destroyed.
  static ShmRegion create(const std::string& name, std::size_t size, bool unlink_on_close = true);

  // Map an existing object. Read-only mappings are used by subscribers.
  static ShmRegion open(const std::string& name, bool read_only = true);

  // Anonymous private mapping with the same layout semantics; handy for tests.
  static ShmRegion anonymous(std::size_t size);

  void* data() const noexcept { return addr_; }
  std::size_t size() const noexcept { return size_; }
  const std::string& name() const noexcept { return name_; }
  explicit operator bool() const noexcept { return addr_ != nullptr; }

  // Ask the kernel to lock pages in RAM (best effort, avoids page faults on the hot path).
  bool lock_pages() noexcept;

 private:
  void reset() noexcept;

  void* addr_{nullptr};
  std::size_t size_{0};
  std::string name_;
  bool owner_{false};
};

// Normalise a user supplied name to POSIX form ("/name").
std::string normalize_name(const std::string& name);

}  // namespace qm::shm
