#pragma once

#include "portfolio/config.h"
#include "portfolio/database.h"
#include "portfolio/world.h"

#include <liburing.h>

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace portfolio {

class IoUringGameServer {
 public:
  IoUringGameServer(ServerConfig config, DbDispatcher& database);
  ~IoUringGameServer();

  IoUringGameServer(const IoUringGameServer&) = delete;
  IoUringGameServer& operator=(const IoUringGameServer&) = delete;

  void run();
  void stop();

 private:
  struct Session {
    int fd{-1};
    std::uint64_t generation{0};
    std::string input;
    std::deque<std::string> outbox;
    bool write_in_flight{false};
    bool close_after_flush{false};
    std::uint64_t pending_db{0};
  };

  ServerConfig config_;
  DbDispatcher& database_;
  SectorWorld world_;
  io_uring ring_{};
  bool ring_initialized_{false};
  int listen_fd_{-1};
  bool stopping_{false};
  std::uint64_t next_request_id_{1};
  std::uint64_t next_session_generation_{1};
  std::unordered_map<int, Session> sessions_;

  void setup_listen_socket();
  void close_listen_socket();
  void post_accept();
  void post_recv(int fd, std::uint64_t generation);
  void post_send(int fd);
  void post_eventfd_poll();
  void queue_response(int fd, std::uint64_t generation, std::string line);
  void deliver_world_events(const std::vector<WorldEvent>& events);
  void handle_client_line(Session& session, const std::string& line);
  void close_session(int fd);
  void maybe_close_after_flush(int fd);
  bool session_matches(int fd, std::uint64_t generation) const;
};

}  // namespace portfolio
