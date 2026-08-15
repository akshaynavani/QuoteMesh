#include "quotemesh/config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "quotemesh/types.hpp"

namespace qm {
namespace {

const char* kDefaultExchanges[] = {"NYSE", "NASDAQ", "ARCA", "BATS",  "BATY", "EDGA",
                                   "EDGX", "IEX",    "MEMX", "MIAX",  "LTSE", "AMEX",
                                   "PHLX", "BX",     "CHX",  "NSX"};

const char* kDefaultSymbols[] = {"AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "META", "TSLA",
                                 "SPY",  "QQQ",  "IWM",  "AMD",  "NFLX",  "JPM",  "XOM",
                                 "BAC",  "INTC", "DIS",  "KO",   "PEP",   "COST"};

bool parse_bool(const std::string& v) {
  if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
  if (v == "false" || v == "0" || v == "no" || v == "off") return false;
  throw std::invalid_argument("expected boolean, got '" + v + "'");
}

long long parse_int(const std::string& v) {
  std::size_t pos = 0;
  const long long r = std::stoll(v, &pos, 0);
  if (pos != v.size()) throw std::invalid_argument("expected integer, got '" + v + "'");
  return r;
}

double parse_double(const std::string& v) {
  std::size_t pos = 0;
  const double r = std::stod(v, &pos);
  if (pos != v.size()) throw std::invalid_argument("expected number, got '" + v + "'");
  return r;
}

// "<NAME> synthetic" | "<NAME> udp:<port>" | "<NAME> udp:<host>:<port>" |
// "<NAME> mcast:<group>:<port>"
ExchangeConfig parse_exchange(const std::string& v) {
  std::istringstream in(v);
  ExchangeConfig e;
  std::string spec;
  in >> e.name >> spec;
  if (e.name.empty() || e.name.size() > 7) throw std::invalid_argument("bad exchange name: " + v);
  if (spec.empty() || spec == "synthetic") return e;

  const auto parts = split(spec, ':');
  if (parts[0] != "udp" && parts[0] != "mcast")
    throw std::invalid_argument("unknown exchange source: " + spec);
  e.kind = ExchangeConfig::Kind::Udp;
  e.multicast = parts[0] == "mcast";
  if (parts.size() == 2) {
    e.udp_host = "0.0.0.0";
    e.udp_port = static_cast<std::uint16_t>(parse_int(parts[1]));
  } else if (parts.size() == 3) {
    e.udp_host = parts[1];
    e.udp_port = static_cast<std::uint16_t>(parse_int(parts[2]));
  } else {
    throw std::invalid_argument("bad udp spec: " + spec);
  }
  if (e.multicast && parts.size() != 3) throw std::invalid_argument("mcast needs group:port");
  return e;
}

}  // namespace

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) { out.push_back(trim(cur)); cur.clear(); } else { cur.push_back(c); }
  }
  out.push_back(trim(cur));
  return out;
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

void AppConfig::set(const std::string& key, const std::string& value) {
  if (key == "shm.name") shm_name = value;
  else if (key == "shm.ring_capacity") ring_capacity = static_cast<std::uint32_t>(parse_int(value));
  else if (key == "shm.max_symbols") max_symbols = static_cast<std::uint32_t>(parse_int(value));
  else if (key == "shm.lock_pages") lock_pages = parse_bool(value);
  else if (key == "aggregator.cpu") aggregator_cpu = static_cast<int>(parse_int(value));
  else if (key == "aggregator.busy_poll") aggregator_busy_poll = parse_bool(value);
  else if (key == "aggregator.forward_venue_quotes") forward_venue_quotes = parse_bool(value);
  else if (key == "feed.first_cpu") first_feed_cpu = static_cast<int>(parse_int(value));
  else if (key == "feed.busy_poll") feed_busy_poll = parse_bool(value);
  else if (key == "feed.simd") use_simd = parse_bool(value);
  else if (key == "feed.queue_capacity") queue_capacity = static_cast<std::uint32_t>(parse_int(value));
  else if (key == "synthetic.rate") synthetic_rate = parse_double(value);
  else if (key == "synthetic.trade_ratio") trade_ratio = parse_double(value);
  else if (key == "synthetic.drop_ratio") drop_ratio = parse_double(value);
  else if (key == "stats.interval_ms") stats_interval_ms = static_cast<std::uint32_t>(parse_int(value));
  else if (key == "symbols") {
    symbols.clear();
    for (auto& s : split(value, ',')) if (!s.empty()) symbols.push_back(s);
  } else if (key.rfind("exchange.", 0) == 0) {
    const auto idx = static_cast<std::size_t>(parse_int(key.substr(9)));
    if (idx >= kMaxExchanges) throw std::invalid_argument("exchange index out of range: " + key);
    if (exchanges.size() <= idx) exchanges.resize(idx + 1);
    exchanges[idx] = parse_exchange(value);
  } else {
    throw std::invalid_argument("unknown config key: " + key);
  }
}

void AppConfig::validate() const {
  if (exchanges.empty()) throw std::invalid_argument("no exchanges configured");
  if (exchanges.size() > kMaxExchanges) throw std::invalid_argument("too many exchanges");
  for (std::size_t i = 0; i < exchanges.size(); ++i) {
    if (exchanges[i].name.empty())
      throw std::invalid_argument("exchange." + std::to_string(i) + " is missing");
  }
  if (symbols.empty()) throw std::invalid_argument("no symbols configured");
  for (const auto& s : symbols) {
    if (s.size() > kMaxSymbolLen) throw std::invalid_argument("symbol too long: " + s);
  }
  if (ring_capacity < 2 || (ring_capacity & (ring_capacity - 1)) != 0)
    throw std::invalid_argument("shm.ring_capacity must be a power of two");
}

std::string AppConfig::summary() const {
  std::ostringstream o;
  std::size_t udp = 0;
  for (const auto& e : exchanges) udp += e.kind == ExchangeConfig::Kind::Udp;
  o << exchanges.size() << " exchanges (" << exchanges.size() - udp << " synthetic, " << udp
    << " udp), " << symbols.size() << " symbols, shm=" << shm_name << " ring=" << ring_capacity
    << " simd=" << (use_simd ? "on" : "off");
  return o.str();
}

AppConfig default_config() {
  AppConfig c;
  for (const char* s : kDefaultSymbols) c.symbols.emplace_back(s);
  for (const char* n : kDefaultExchanges) {
    ExchangeConfig e;
    e.name = n;
    c.exchanges.push_back(e);
  }
  return c;
}

AppConfig load_config(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open config file: " + path);
  AppConfig c = default_config();
  bool exchanges_reset = false;
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    line = trim(line);
    if (line.empty()) continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos)
      throw std::invalid_argument(path + ":" + std::to_string(lineno) + ": expected key = value");
    const std::string key = trim(line.substr(0, eq));
    if (key.rfind("exchange.", 0) == 0 && !exchanges_reset) {
      c.exchanges.clear();
      exchanges_reset = true;
    }
    try {
      c.set(key, trim(line.substr(eq + 1)));
    } catch (const std::exception& e) {
      throw std::invalid_argument(path + ":" + std::to_string(lineno) + ": " + e.what());
    }
  }
  return c;
}

void apply_env_overrides(AppConfig& cfg) {
  if (const char* v = std::getenv("QUOTEMESH_SHM_NAME"); v && *v) cfg.shm_name = v;
  if (const char* v = std::getenv("QUOTEMESH_SYNTHETIC_RATE"); v && *v) cfg.set("synthetic.rate", v);
}

}  // namespace qm
