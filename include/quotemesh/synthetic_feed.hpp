#pragma once

#include <random>
#include <string>
#include <vector>

#include "quotemesh/feed_source.hpp"
#include "quotemesh/types.hpp"

namespace qm {

struct SyntheticFeedConfig {
  std::string exchange_name{"SIM"};
  std::vector<std::string> symbols{"AAPL", "MSFT", "SPY"};
  double messages_per_sec{100'000.0};  // <= 0 means unthrottled
  double trade_ratio{0.1};             // fraction of messages that are trades
  double drop_ratio{0.0};              // fraction of sequence numbers skipped (simulated loss)
  Price base_price{1'000'000};         // 100.0000
  Price tick{100};                     // 0.01
  std::uint64_t seed{1};
  std::uint64_t max_messages{0};       // 0 = infinite, otherwise close after this many
};

// Generates a random-walk quote/trade stream for one exchange. Each exchange gets its own
// seed so venues disagree slightly on price, which exercises the NBBO logic.
class SyntheticFeed final : public FeedSource {
 public:
  explicit SyntheticFeed(SyntheticFeedConfig cfg);

  ssize_t read(char* buf, std::size_t cap) override;
  std::string describe() const override;

  std::uint64_t generated() const noexcept { return generated_; }

 private:
  struct SymState {
    Price mid;
    Price half_spread;
  };

  std::size_t budget();
  std::size_t encode_next(char* out, std::size_t cap);

  SyntheticFeedConfig cfg_;
  std::mt19937_64 rng_;
  std::vector<SymState> state_;
  std::uint64_t seq_{0};
  std::uint64_t generated_{0};
  std::uint64_t start_ns_{0};
  std::uint64_t emitted_for_rate_{0};
};

}  // namespace qm
