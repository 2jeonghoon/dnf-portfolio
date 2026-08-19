#include "portfolio/server.h"

#include "portfolio/protocol.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace portfolio {
namespace {

enum class CompletionType {
  kAccept,
  kRecv,
  kSend,
  kDatabaseEvent,
};

struct CompletionToken {
  CompletionType type{CompletionType::kAccept};
  int fd{-1};
  std::uint64_t generation{0};
  std::vector<char> buffer;
  std::string payload;
};

io_uring_sqe* get_sqe(io_uring& ring) {
  io_uring_sqe* sqe = io_uring_get_sqe(&ring);
  if (sqe != nullptr) {
    return sqe;
  }

  const int submitted = io_uring_submit(&ring);
  if (submitted < 0) {
    throw std::runtime_error("io_uring_submit failed while reclaiming SQEs: " +
                             std::string(std::strerror(-submitted)));
  }

  sqe = io_uring_get_sqe(&ring);
  if (sqe == nullptr) {
    throw std::runtime_error("io_uring submission queue is full");
  }
  return sqe;
}

void set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    throw std::runtime_error("fcntl(F_GETFL) failed: " + std::string(std::strerror(errno)));
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw std::runtime_error("fcntl(F_SETFL) failed: " + std::string(std::strerror(errno)));
  }
}

void set_reuseaddr(int fd) {
  int value = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) < 0) {
    throw std::runtime_error("setsockopt(SO_REUSEADDR) failed: " +
                             std::string(std::strerror(errno)));
  }
}

void set_tcp_nodelay(int fd) {
  int value = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
}

DbOperation operation_for(CommandType command) {
  switch (command) {
    case CommandType::kRegister:
      return DbOperation::kRegisterPlayer;
    case CommandType::kLogin:
      return DbOperation::kLoginPlayer;
    case CommandType::kSaveCharacter:
      return DbOperation::kSaveCharacter;
    case CommandType::kLoadCharacters:
      return DbOperation::kLoadCharacters;
    case CommandType::kSaveInventoryItem:
      return DbOperation::kSaveInventoryItem;
    case CommandType::kLoadInventory:
      return DbOperation::kLoadInventory;
    case CommandType::kEnterWorld:
    case CommandType::kMove:
    case CommandType::kAttack:
    case CommandType::kQuit:
    case CommandType::kUnknown:
      break;
  }
  throw std::runtime_error("command is not backed by a database operation");
}

bool is_database_command(CommandType command) {
  return command == CommandType::kRegister || command == CommandType::kLogin ||
         command == CommandType::kSaveCharacter || command == CommandType::kLoadCharacters ||
         command == CommandType::kSaveInventoryItem || command == CommandType::kLoadInventory;
}

bool is_world_command(CommandType command) {
  return command == CommandType::kEnterWorld || command == CommandType::kMove ||
         command == CommandType::kAttack;
}

}  // namespace

IoUringGameServer::IoUringGameServer(ServerConfig config, DbDispatcher& database)
    : config_(config), database_(database) {}

IoUringGameServer::~IoUringGameServer() {
  stop();
  if (ring_initialized_) {
    io_uring_queue_exit(&ring_);
  }
}

void IoUringGameServer::run() {
  setup_listen_socket();

  const int init_result = io_uring_queue_init(config_.ring_entries, &ring_, 0);
  if (init_result < 0) {
    throw std::runtime_error("io_uring_queue_init failed: " +
                             std::string(std::strerror(-init_result)));
  }
  ring_initialized_ = true;

  post_accept();
  post_eventfd_poll();

  std::cout << "listening on 0.0.0.0:" << config_.port << std::endl;

  while (!stopping_) {
    const int wait_result = io_uring_submit_and_wait(&ring_, 1);
    if (wait_result < 0) {
      if (wait_result == -EINTR) {
        continue;
      }
      throw std::runtime_error("io_uring_submit_and_wait failed: " +
                               std::string(std::strerror(-wait_result)));
    }

    io_uring_cqe* cqe = nullptr;
    while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe != nullptr) {
      std::unique_ptr<CompletionToken> token(
          reinterpret_cast<CompletionToken*>(io_uring_cqe_get_data(cqe)));
      const int result = cqe->res;
      io_uring_cqe_seen(&ring_, cqe);

      switch (token->type) {
        case CompletionType::kAccept: {
          if (result >= 0) {
            set_tcp_nodelay(result);
            Session session;
            session.fd = result;
            session.generation = next_session_generation_++;
            const std::uint64_t generation = session.generation;
            sessions_.emplace(result, std::move(session));

            queue_response(result, generation,
                           "HELLO commands=REGISTER,LOGIN,SAVE,LOAD,ENTER,MOVE,ATTACK,"
                           "SAVE_ITEM,LOAD_INVENTORY,QUIT");
            post_recv(result, generation);
          } else if (result != -ECANCELED && result != -EINVAL) {
            std::cerr << "accept failed: " << std::strerror(-result) << std::endl;
          }

          if (!stopping_) {
            post_accept();
          }
          break;
        }

        case CompletionType::kRecv: {
          if (!session_matches(token->fd, token->generation)) {
            break;
          }

          if (result <= 0) {
            close_session(token->fd);
            break;
          }

          auto session_it = sessions_.find(token->fd);
          Session& session = session_it->second;
          session.input.append(token->buffer.data(), static_cast<std::size_t>(result));

          while (true) {
            const std::size_t newline = session.input.find('\n');
            if (newline == std::string::npos) {
              break;
            }

            std::string line = session.input.substr(0, newline);
            session.input.erase(0, newline + 1);
            handle_client_line(session, line);

            if (!session_matches(token->fd, token->generation) || session.close_after_flush) {
              break;
            }
          }

          if (!session_matches(token->fd, token->generation)) {
            break;
          }

          Session& refreshed_session = sessions_.find(token->fd)->second;
          if (refreshed_session.input.size() > config_.max_line_size) {
            queue_response(token->fd, token->generation, "ERR line too long");
            refreshed_session.close_after_flush = true;
          }

          if (!refreshed_session.close_after_flush) {
            post_recv(token->fd, token->generation);
          } else {
            maybe_close_after_flush(token->fd);
          }
          break;
        }

        case CompletionType::kSend: {
          if (!session_matches(token->fd, token->generation)) {
            break;
          }

          Session& session = sessions_.find(token->fd)->second;
          session.write_in_flight = false;

          if (result <= 0 || session.outbox.empty()) {
            close_session(token->fd);
            break;
          }

          std::string& front = session.outbox.front();
          const std::size_t bytes_sent = static_cast<std::size_t>(result);
          if (bytes_sent >= front.size()) {
            session.outbox.pop_front();
          } else {
            front.erase(0, bytes_sent);
          }

          post_send(token->fd);
          maybe_close_after_flush(token->fd);
          break;
        }

        case CompletionType::kDatabaseEvent: {
          std::uint64_t event_count = 0;
          while (read(database_.event_fd(), &event_count, sizeof(event_count)) ==
                 static_cast<ssize_t>(sizeof(event_count))) {
          }

          DbCompletion completion;
          while (database_.pop_completion(completion)) {
            if (!session_matches(completion.client_fd, completion.client_generation)) {
              continue;
            }

            Session& session = sessions_.find(completion.client_fd)->second;
            if (session.pending_db > 0) {
              --session.pending_db;
            }

            std::string prefix = completion.ok ? "DONE " : "FAIL ";
            queue_response(completion.client_fd, completion.client_generation,
                           prefix + std::to_string(completion.request_id) + " " +
                               completion.line);
            maybe_close_after_flush(completion.client_fd);
          }

          if (!stopping_) {
            post_eventfd_poll();
          }
          break;
        }
      }
    }
  }
}

void IoUringGameServer::stop() {
  stopping_ = true;
  close_listen_socket();

  std::vector<int> fds;
  fds.reserve(sessions_.size());
  for (const auto& entry : sessions_) {
    fds.push_back(entry.first);
  }
  for (int fd : fds) {
    close_session(fd);
  }
}

void IoUringGameServer::setup_listen_socket() {
  listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    throw std::runtime_error("socket failed: " + std::string(std::strerror(errno)));
  }

  set_reuseaddr(listen_fd_);
  set_nonblocking(listen_fd_);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(config_.port);

  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    const std::string message = "bind failed: " + std::string(std::strerror(errno));
    close_listen_socket();
    throw std::runtime_error(message);
  }

  if (listen(listen_fd_, config_.backlog) < 0) {
    const std::string message = "listen failed: " + std::string(std::strerror(errno));
    close_listen_socket();
    throw std::runtime_error(message);
  }
}

void IoUringGameServer::close_listen_socket() {
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
}

void IoUringGameServer::post_accept() {
  std::unique_ptr<CompletionToken> token(new CompletionToken);
  token->type = CompletionType::kAccept;

  io_uring_sqe* sqe = get_sqe(ring_);
  io_uring_prep_accept(sqe, listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
  io_uring_sqe_set_data(sqe, token.release());
}

void IoUringGameServer::post_recv(int fd, std::uint64_t generation) {
  std::unique_ptr<CompletionToken> token(new CompletionToken);
  token->type = CompletionType::kRecv;
  token->fd = fd;
  token->generation = generation;
  token->buffer.resize(config_.recv_buffer_size);

  io_uring_sqe* sqe = get_sqe(ring_);
  io_uring_prep_recv(sqe, fd, token->buffer.data(), token->buffer.size(), 0);
  io_uring_sqe_set_data(sqe, token.release());
}

void IoUringGameServer::post_send(int fd) {
  auto session_it = sessions_.find(fd);
  if (session_it == sessions_.end()) {
    return;
  }

  Session& session = session_it->second;
  if (session.write_in_flight || session.outbox.empty()) {
    return;
  }

  std::unique_ptr<CompletionToken> token(new CompletionToken);
  token->type = CompletionType::kSend;
  token->fd = fd;
  token->generation = session.generation;
  token->payload = session.outbox.front();

  session.write_in_flight = true;
  io_uring_sqe* sqe = get_sqe(ring_);
  io_uring_prep_send(sqe, fd, token->payload.data(), token->payload.size(), MSG_NOSIGNAL);
  io_uring_sqe_set_data(sqe, token.release());
}

void IoUringGameServer::post_eventfd_poll() {
  std::unique_ptr<CompletionToken> token(new CompletionToken);
  token->type = CompletionType::kDatabaseEvent;

  io_uring_sqe* sqe = get_sqe(ring_);
  io_uring_prep_poll_add(sqe, database_.event_fd(), POLLIN);
  io_uring_sqe_set_data(sqe, token.release());
}

void IoUringGameServer::queue_response(int fd, std::uint64_t generation, std::string line) {
  if (!session_matches(fd, generation)) {
    return;
  }

  if (line.empty() || line.back() != '\n') {
    line.push_back('\n');
  }

  Session& session = sessions_.find(fd)->second;
  session.outbox.push_back(std::move(line));
  post_send(fd);
}

void IoUringGameServer::deliver_world_events(const std::vector<WorldEvent>& events) {
  for (const WorldEvent& event : events) {
    queue_response(event.receiver.fd, event.receiver.generation, event.line);
  }
}

void IoUringGameServer::handle_client_line(Session& session, const std::string& line) {
  const ParseResult parsed = parse_line(line);
  if (!parsed.ok) {
    queue_response(session.fd, session.generation, "ERR " + parsed.error);
    return;
  }

  if (parsed.request.type == CommandType::kQuit) {
    deliver_world_events(world_.leave({session.fd, session.generation}));
    queue_response(session.fd, session.generation, "BYE");
    session.close_after_flush = true;
    return;
  }

  if (is_world_command(parsed.request.type)) {
    const SessionRef ref{session.fd, session.generation};
    switch (parsed.request.type) {
      case CommandType::kEnterWorld:
        deliver_world_events(world_.enter(ref, parsed.request.args.at(0), parsed.request.args.at(1),
                                          std::stoi(parsed.request.args.at(2)),
                                          std::stoi(parsed.request.args.at(3))));
        return;
      case CommandType::kMove:
        deliver_world_events(world_.move(ref, std::stoi(parsed.request.args.at(0)),
                                         std::stoi(parsed.request.args.at(1))));
        return;
      case CommandType::kAttack:
        deliver_world_events(
            world_.attack(ref, parsed.request.args.at(0), parsed.request.args.at(1)));
        return;
      case CommandType::kRegister:
      case CommandType::kLogin:
      case CommandType::kSaveCharacter:
      case CommandType::kLoadCharacters:
      case CommandType::kSaveInventoryItem:
      case CommandType::kLoadInventory:
      case CommandType::kQuit:
      case CommandType::kUnknown:
        break;
    }
  }

  if (!is_database_command(parsed.request.type)) {
    queue_response(session.fd, session.generation, "ERR unsupported command");
    return;
  }

  DbJob job;
  job.client_fd = session.fd;
  job.client_generation = session.generation;
  job.request_id = next_request_id_++;
  job.operation = operation_for(parsed.request.type);
  job.command_name = parsed.request.command_name;
  job.args = parsed.request.args;

  ++session.pending_db;
  queue_response(session.fd, session.generation,
                 "PENDING " + std::to_string(job.request_id) + " " + job.command_name);
  database_.submit(std::move(job));
}

void IoUringGameServer::close_session(int fd) {
  auto session_it = sessions_.find(fd);
  if (session_it == sessions_.end()) {
    return;
  }

  if (!stopping_) {
    deliver_world_events(world_.leave({session_it->second.fd, session_it->second.generation}));
  }
  close(session_it->second.fd);
  sessions_.erase(session_it);
}

void IoUringGameServer::maybe_close_after_flush(int fd) {
  auto session_it = sessions_.find(fd);
  if (session_it == sessions_.end()) {
    return;
  }

  const Session& session = session_it->second;
  if (session.close_after_flush && !session.write_in_flight && session.outbox.empty()) {
    close_session(fd);
  }
}

bool IoUringGameServer::session_matches(int fd, std::uint64_t generation) const {
  auto session_it = sessions_.find(fd);
  return session_it != sessions_.end() && session_it->second.generation == generation;
}

}  // namespace portfolio
