#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <unistd.h>

#include "quotemesh/config.hpp"

using namespace qm;

TEST(Config, DefaultsHaveFifteenPlusExchanges) {
  const AppConfig c = default_config();
  EXPECT_GE(c.exchanges.size(), 15u);
  EXPECT_FALSE(c.symbols.empty());
  EXPECT_NO_THROW(c.validate());
}

TEST(Config, SetParsesExchangeSpecs) {
  AppConfig c = default_config();
  c.set("exchange.16", "SIMU udp:31001");
  c.set("exchange.17", "SIMM mcast:239.1.1.1:31002");
  ASSERT_EQ(c.exchanges.size(), 18u);
  EXPECT_EQ(c.exchanges[16].kind, ExchangeConfig::Kind::Udp);
  EXPECT_EQ(c.exchanges[16].udp_port, 31001);
  EXPECT_FALSE(c.exchanges[16].multicast);
  EXPECT_TRUE(c.exchanges[17].multicast);
  EXPECT_EQ(c.exchanges[17].udp_host, "239.1.1.1");
}

TEST(Config, RejectsBadValues) {
  AppConfig c = default_config();
  EXPECT_THROW(c.set("nope", "1"), std::invalid_argument);
  EXPECT_THROW(c.set("feed.simd", "maybe"), std::invalid_argument);
  EXPECT_THROW(c.set("exchange.0", "X tcp:1"), std::invalid_argument);
  c.set("shm.ring_capacity", "1000");
  EXPECT_THROW(c.validate(), std::invalid_argument);
}

TEST(Config, LoadsFileAndReplacesExchanges) {
  const std::string path = "/tmp/qm_cfg_test_" + std::to_string(::getpid()) + ".conf";
  {
    std::ofstream out(path);
    out << "# comment\nshm.name = /qm_x  # trailing\nsymbols = AAA, BBB\n"
           "exchange.0 = ONE synthetic\nexchange.1 = TWO udp:4000\n";
  }
  const AppConfig c = load_config(path);
  std::remove(path.c_str());
  EXPECT_EQ(c.shm_name, "/qm_x");
  ASSERT_EQ(c.symbols.size(), 2u);
  EXPECT_EQ(c.symbols[1], "BBB");
  ASSERT_EQ(c.exchanges.size(), 2u);
  EXPECT_EQ(c.exchanges[1].name, "TWO");
}
