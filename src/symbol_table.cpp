#include "quotemesh/symbol_table.hpp"

namespace qm {

SymbolTable::SymbolTable(std::size_t max_symbols) : max_symbols_(max_symbols) {
  ids_.reserve(max_symbols < 4096 ? max_symbols : 4096);
}

SymbolId SymbolTable::intern(std::string_view symbol) {
  if (symbol.empty() || symbol.size() > kMaxSymbolLen) return kInvalidSymbol;
  {
    std::shared_lock lock(mu_);
    if (auto it = ids_.find(symbol); it != ids_.end()) return it->second;
  }
  std::unique_lock lock(mu_);
  if (auto it = ids_.find(symbol); it != ids_.end()) return it->second;
  if (names_.size() >= max_symbols_) return kInvalidSymbol;
  const auto id = static_cast<SymbolId>(names_.size());
  names_.emplace_back(symbol);
  ids_.emplace(std::string(symbol), id);
  return id;
}

SymbolId SymbolTable::find(std::string_view symbol) const {
  std::shared_lock lock(mu_);
  if (auto it = ids_.find(symbol); it != ids_.end()) return it->second;
  return kInvalidSymbol;
}

std::string SymbolTable::name(SymbolId id) const {
  std::shared_lock lock(mu_);
  return id < names_.size() ? names_[id] : std::string{};
}

std::size_t SymbolTable::size() const {
  std::shared_lock lock(mu_);
  return names_.size();
}

}  // namespace qm
