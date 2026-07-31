#include "quotemesh/synthetic_feed.hpp"

#include <algorithm>

#include "quotemesh/clock.hpp"
#include "quotemesh/wire_format.hpp"

namespace qm {

SyntheticFeed::SyntheticFeed(SyntheticFeedConfig cfg) : cfg_(std::move(cfg)), rng_(cfg_.seed) {
  state_.reserve(cfg_.symbols.size());
  for (std::size_t i = 0; i < cfg_.symbols.size(); ++i) {
    // Spread symbols across a range of price levels; jitter per exchange via the seed.
    const Price mid = cfg_.base_price * static_cast<Price>(1 + i % 7) +
                      static_cast<Price>(rng_() % 5) * cfg_.tick;
    state_.push_back({mid, cfg_.tick * static_cast<Price>(1 + rng_() % 3)});
  }
}

std::string SyntheticFeed::describe() const {
  return "synthetic:" + cfg_.exchange_name + " (" + std::to_string(cfg_.symbols.size()) +
         " symbols)";
}

std::size_t SyntheticFeed::budget() {
  if (cfg_.messages_per_sec <= 0) return ~std::size_t{0};
  const auto now = mono_ns();
  if (start_ns_ == 0) start_ns_ = now;
  const double elapsed = static_cast<double>(now - start_ns_) / 1e9;
  const auto allowed = static_cast<std::uint64_t>(elapsed * cfg_.messages_per_sec) + 1;
  return allowed > emitted_for_rate_ ? static_cast<std::size_t>(allowed - emitted_for_rate_) : 0;
}

std::size_t SyntheticFeed::encode_next(char* out, std::size_t cap) {
  std::uniform_real_distribution<double> u(0.0, 1.0);
  ++seq_;
  if (cfg_.drop_ratio > 0 && u(rng_) < cfg_.drop_ratio) ++seq_;  // simulate a lost packet

  const std::size_t idx = rng_() % state_.size();
  SymState& s = state_[idx];
  const int step = static_cast<int>(rng_() % 5) - 2;  // -2..+2 ticks
  s.mid = std::max<Price>(s.mid + step * cfg_.tick, 10 * cfg_.tick);
  if (rng_() % 16 == 0) s.half_spread = cfg_.tick * static_cast<Price>(1 + rng_() % 3);

  const Timestamp ts = wall_ns();
  const std::string& sym = cfg_.symbols[idx];
  if (u(rng_) < cfg_.trade_ratio) {
    const Price px = (rng_() & 1) ? s.mid - s.half_spread : s.mid + s.half_spread;
    Trade t{px, static_cast<Quantity>(100 * (1 + rng_() % 10))};
    return wire::encode_trade(out, cap, seq_, sym, ts, t);
  }
  Quote q{s.mid - s.half_spread, s.mid + s.half_spread,
          static_cast<Quantity>(100 * (1 + rng_() % 50)),
          static_cast<Quantity>(100 * (1 + rng_() % 50))};
  return wire::encode_quote(out, cap, seq_, sym, ts, q);
}

ssize_t SyntheticFeed::read(char* buf, std::size_t cap) {
  if (state_.empty()) return -1;
  if (cfg_.max_messages != 0 && generated_ >= cfg_.max_messages) return -1;
  std::size_t allowed = budget();
  std::size_t used = 0;
  while (allowed > 0 && cap - used >= wire::kMaxMessageLen) {
    if (cfg_.max_messages != 0 && generated_ >= cfg_.max_messages) break;
    const std::size_t n = encode_next(buf + used, cap - used);
    if (n == 0) break;
    used += n;
    --allowed;
    ++generated_;
    ++emitted_for_rate_;
  }
  return static_cast<ssize_t>(used);
}

}  // namespace qm
