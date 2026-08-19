#include "portfolio/config.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace portfolio {
namespace {

std::string env_string(const char* name, std::string fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  return value;
}

unsigned int env_uint(const char* name, unsigned int fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }

  try {
    return static_cast<unsigned int>(std::stoul(value));
  } catch (const std::exception&) {
    throw std::runtime_error(std::string("invalid unsigned integer in ") + name);
  }
}

}  // namespace

AppConfig load_config_from_env() {
  AppConfig config;

  config.server.port =
      static_cast<std::uint16_t>(env_uint("SERVER_PORT", config.server.port));
  config.server.backlog = static_cast<int>(env_uint("SERVER_BACKLOG", config.server.backlog));
  config.server.ring_entries = env_uint("URING_ENTRIES", config.server.ring_entries);

  config.database.host = env_string("MYSQL_HOST", config.database.host);
  config.database.port = env_uint("MYSQL_PORT", config.database.port);
  config.database.user = env_string("MYSQL_USER", config.database.user);
  config.database.password = env_string("MYSQL_PASSWORD", config.database.password);
  config.database.schema = env_string("MYSQL_DATABASE", config.database.schema);
  config.database.pool_size = env_uint("MYSQL_POOL_SIZE", config.database.pool_size);

  if (config.database.pool_size == 0) {
    throw std::runtime_error("MYSQL_POOL_SIZE must be greater than zero");
  }
  if (config.server.ring_entries < 32) {
    throw std::runtime_error("URING_ENTRIES must be at least 32");
  }

  return config;
}

}  // namespace portfolio
