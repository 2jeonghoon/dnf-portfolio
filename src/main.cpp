#include "portfolio/config.h"
#include "portfolio/database.h"
#include "portfolio/server.h"

#include <csignal>
#include <exception>
#include <iostream>

int main() {
  std::signal(SIGPIPE, SIG_IGN);

  try {
    // Initializes the MySQL client library (mysql_library_init) and tears it
    // down on scope exit (mysql_library_end) via RAII.
    portfolio::MysqlLibrary mysql_library;
    // Reads server/DB settings from environment variables (SERVER_PORT,
    // MYSQL_HOST, etc.), falling back to defaults where unset.
    portfolio::AppConfig config = portfolio::load_config_from_env();

    // Constructs the DB worker pool dispatcher; workers aren't running yet.
    portfolio::DbDispatcher database(config.database);
    database.start();

    // Constructs the io_uring event-loop server, wired to the DB dispatcher
    // for offloading blocking MySQL calls off the network thread.
    portfolio::IoUringGameServer server(config.server, database);
    // Blocks here running the io_uring event loop until the server stops.
    server.run();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << std::endl;
    return 1;
  }
}
