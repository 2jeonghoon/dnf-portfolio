#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace portfolio {

struct ServerConfig {
  std::uint16_t port{9090};
  int backlog{512};
  unsigned int ring_entries{512};
  std::size_t recv_buffer_size{4096};
  std::size_t max_line_size{8192};
};

struct DatabaseConfig {
  std::string host{"127.0.0.1"};
  unsigned int port{3306};
  std::string user{"root"};
  std::string password{};
  std::string schema{"dungeon_portfolio"};
  unsigned int pool_size{4};
};

struct AppConfig {
  ServerConfig server;
  DatabaseConfig database;
};

AppConfig load_config_from_env();

}  // namespace portfolio
