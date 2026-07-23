#pragma once

#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "quotemesh/types.hpp"

namespace qm {

// Interns symbol strings to dense ids. Lookups of already-known symbols take a shared lock;
// new symbols (rare after warm-up) take an exclusive lock. Feed handlers keep a thread-local
// cache on top of this so the hot path is lock-free in steady state.
class SymbolTable {
 public:
  explicit SymbolTable(std::size_t max_symbols = 65'536);

  SymbolId intern(std::string_view symbol);
  SymbolId find(std::string_view symbol) const;
  std::string name(SymbolId id) const;
  std::size_t size() const;
  std::size_t capacity() const noexcept { return max_symbols_; }

 private:
  struct Hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
      return std::hash<std::string_view>{}(s);
    }
  };

  std::size_t max_symbols_;
  mutable std::shared_mutex mu_;
  std::unordered_map<std::string, SymbolId, Hash, std::equal_to<>> ids_;
  std::vector<std::string> names_;
};

}  // namespace qm
