#include "quotemesh/shm/shm_region.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace qm::shm {
namespace {

[[noreturn]] void fail(const std::string& what) {
  throw std::runtime_error(what + ": " + std::strerror(errno));
}

}  // namespace

std::string normalize_name(const std::string& name) {
  if (name.empty()) throw std::invalid_argument("shared memory name must not be empty");
  return name.front() == '/' ? name : "/" + name;
}

ShmRegion::~ShmRegion() { reset(); }

ShmRegion::ShmRegion(ShmRegion&& o) noexcept
    : addr_(std::exchange(o.addr_, nullptr)),
      size_(std::exchange(o.size_, 0)),
      name_(std::move(o.name_)),
      owner_(std::exchange(o.owner_, false)) {}

ShmRegion& ShmRegion::operator=(ShmRegion&& o) noexcept {
  if (this != &o) {
    reset();
    addr_ = std::exchange(o.addr_, nullptr);
    size_ = std::exchange(o.size_, 0);
    name_ = std::move(o.name_);
    owner_ = std::exchange(o.owner_, false);
  }
  return *this;
}

void ShmRegion::reset() noexcept {
  if (addr_ != nullptr) ::munmap(addr_, size_);
  if (owner_ && !name_.empty()) ::shm_unlink(name_.c_str());
  addr_ = nullptr;
  size_ = 0;
  owner_ = false;
}

ShmRegion ShmRegion::create(const std::string& raw_name, std::size_t size, bool unlink_on_close) {
  const std::string name = normalize_name(raw_name);
  ::shm_unlink(name.c_str());  // remove a stale segment from a crashed run
  const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
  if (fd < 0) fail("shm_open(create " + name + ")");
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
    ::close(fd);
    ::shm_unlink(name.c_str());
    fail("ftruncate " + name);
  }
  void* addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (addr == MAP_FAILED) {
    ::shm_unlink(name.c_str());
    fail("mmap " + name);
  }
  std::memset(addr, 0, size);  // pre-fault every page now rather than on the hot path

  ShmRegion r;
  r.addr_ = addr;
  r.size_ = size;
  r.name_ = name;
  r.owner_ = unlink_on_close;
  return r;
}

ShmRegion ShmRegion::open(const std::string& raw_name, bool read_only) {
  const std::string name = normalize_name(raw_name);
  const int fd = ::shm_open(name.c_str(), read_only ? O_RDONLY : O_RDWR, 0);
  if (fd < 0) fail("shm_open(" + name + ")");
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    fail("fstat " + name);
  }
  const auto size = static_cast<std::size_t>(st.st_size);
  void* addr = ::mmap(nullptr, size, read_only ? PROT_READ : PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd, 0);
  ::close(fd);
  if (addr == MAP_FAILED) fail("mmap " + name);

  ShmRegion r;
  r.addr_ = addr;
  r.size_ = size;
  r.name_ = name;
  r.owner_ = false;
  return r;
}

ShmRegion ShmRegion::anonymous(std::size_t size) {
  void* addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  if (addr == MAP_FAILED) fail("mmap anonymous");
  ShmRegion r;
  r.addr_ = addr;
  r.size_ = size;
  return r;
}

bool ShmRegion::lock_pages() noexcept {
  return addr_ != nullptr && ::mlock(addr_, size_) == 0;
}

}  // namespace qm::shm
