#include "portfolio/config.h"
#include "portfolio/database.h"
#include "portfolio/server.h"

#include <csignal>
#include <exception>
#include <iostream>

int main() {
  std::signal(SIGPIPE, SIG_IGN);

  try {
    portfolio::MysqlLibrary mysql_library;
    portfolio::AppConfig config = portfolio::load_config_from_env();

    portfolio::DbDispatcher database(config.database);
    database.start();

    portfolio::IoUringGameServer server(config.server, database);
    server.run();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << std::endl;
    return 1;
  }
}
