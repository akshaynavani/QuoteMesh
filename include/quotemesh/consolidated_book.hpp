#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "quotemesh/types.hpp"

namespace qm {

// National best bid/offer across all venues for one symbol.
struct Nbbo {
  Price bid_px{kNoBid};
  Price ask_px{kNoAsk};
  Quantity bid_sz{0};          // aggregated size of all venues at the best bid
  Quantity ask_sz{0};          // aggregated size of all venues at the best ask
  std::uint32_t bid_venues{0}; // bitmask of exchanges at the best bid
  std::uint32_t ask_venues{0}; // bitmask of exchanges at the best ask
  Timestamp updated_ts{0};

  bool has_bid() const noexcept { return bid_px != kNoBid; }
  bool has_ask() const noexcept { return ask_px != kNoAsk; }
  bool locked() const noexcept { return has_bid() && has_ask() && bid_px == ask_px; }
  bool crossed() const noexcept { return has_bid() && has_ask() && bid_px > ask_px; }

  bool same_market(const Nbbo& o) const noexcept {
    return bid_px == o.bid_px && ask_px == o.ask_px && bid_sz == o.bid_sz &&
           ask_sz == o.ask_sz && bid_venues == o.bid_venues && ask_venues == o.ask_venues;
  }
};

struct LastTrade {
  Price px{0};
  Quantity sz{0};
  ExchangeId exchange{0};
  Timestamp ts{0};
  std::uint64_t volume{0};  // cumulative volume across venues
};

// Consolidated per-symbol view: the top of book from every venue plus the derived NBBO.
// Owned and mutated exclusively by the aggregator thread.
class ConsolidatedBook {
 public:
  struct SymbolBook {
    std::array<Quote, kMaxExchanges> venues{};
    std::uint32_t active{0};  // bitmask of venues with at least one side
    Nbbo nbbo{};
    LastTrade last{};
  };

  explicit ConsolidatedBook(std::size_t reserve_symbols = 1024);

  // Apply a venue quote. Returns true if the NBBO changed.
  bool apply_quote(SymbolId sym, ExchangeId ex, const Quote& q, Timestamp ts);
  void apply_trade(SymbolId sym, ExchangeId ex, const Trade& t, Timestamp ts);

  // Drop all quotes from a venue (e.g. on feed disconnect). Returns symbols whose NBBO changed.
  std::vector<SymbolId> clear_venue(ExchangeId ex, Timestamp ts);

  const Nbbo* nbbo(SymbolId sym) const noexcept;
  const SymbolBook* book(SymbolId sym) const noexcept;
  std::size_t symbol_count() const noexcept { return books_.size(); }

 private:
  SymbolBook& ensure(SymbolId sym);
  static bool recompute(SymbolBook& b, Timestamp ts);

  std::vector<SymbolBook> books_;
};

}  // namespace qm
