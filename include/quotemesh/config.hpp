#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qm {

struct ExchangeConfig {
  enum class Kind { Synthetic, Udp };
  std::string name;
  Kind kind{Kind::Synthetic};
  std::string udp_host;  // bind address, or multicast group when `multicast`
  std::uint16_t udp_port{0};
  bool multicast{false};
};

// Top-level application config. Loaded from a simple `key = value` file (see
// config/quotemesh.conf), then overridden by environment variables and `--set k=v` flags.
struct AppConfig {
  // Shared memory
  std::string shm_name{"/quotemesh"};
  std::uint32_t ring_capacity{1u << 16};
  std::uint32_t max_symbols{4096};
  bool lock_pages{false};

  // Threads
  int aggregator_cpu{-1};
  int first_feed_cpu{-1};  // feed i is pinned to first_feed_cpu + i when >= 0
  bool aggregator_busy_poll{true};
  bool feed_busy_poll{false};
  bool forward_venue_quotes{false};

  // Feed handling
  bool use_simd{true};
  std::uint32_t queue_capacity{1u << 16};

  // Synthetic feed parameters (apply to every synthetic exchange)
  std::vector<std::string> symbols;
  double synthetic_rate{20'000.0};
  double trade_ratio{0.1};
  double drop_ratio{0.0};

  std::uint32_t stats_interval_ms{1000};
  std::vector<ExchangeConfig> exchanges;

  // Apply one setting. Throws std::invalid_argument on unknown keys or bad values.
  void set(const std::string& key, const std::string& value);
  void validate() const;
  std::string summary() const;
};

// 16 synthetic exchanges trading a basket of liquid US symbols.
AppConfig default_config();

// Load from file on top of defaults (exchanges in the file replace the default list).
AppConfig load_config(const std::string& path);

// Apply QUOTEMESH_SHM_NAME / QUOTEMESH_SYNTHETIC_RATE environment overrides if set.
void apply_env_overrides(AppConfig& cfg);

std::vector<std::string> split(const std::string& s, char sep);
std::string trim(const std::string& s);

}  // namespace qm
