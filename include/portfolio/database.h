#pragma once

#include "portfolio/blocking_queue.h"
#include "portfolio/config.h"

#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace portfolio {

enum class DbOperation {
  kRegisterPlayer,
  kLoginPlayer,
  kSaveCharacter,
  kLoadCharacters,
  kSaveInventoryItem,
  kLoadInventory,
};

struct DbJob {
  int client_fd{-1};
  std::uint64_t client_generation{0};
  std::uint64_t request_id{0};
  DbOperation operation{DbOperation::kLoginPlayer};
  std::string command_name;
  std::vector<std::string> args;
};

struct DbCompletion {
  int client_fd{-1};
  std::uint64_t client_generation{0};
  std::uint64_t request_id{0};
  bool ok{false};
  std::string line;
};

class MysqlLibrary {
 public:
  MysqlLibrary();
  ~MysqlLibrary();

  MysqlLibrary(const MysqlLibrary&) = delete;
  MysqlLibrary& operator=(const MysqlLibrary&) = delete;
};

class DbDispatcher {
 public:
  explicit DbDispatcher(DatabaseConfig config);
  ~DbDispatcher();

  DbDispatcher(const DbDispatcher&) = delete;
  DbDispatcher& operator=(const DbDispatcher&) = delete;

  void start();
  void stop();
  void submit(DbJob job);
  bool pop_completion(DbCompletion& completion);
  int event_fd() const { return event_fd_; }

 private:
  void worker_loop(unsigned int worker_index);
  void push_completion(DbCompletion completion);

  DatabaseConfig config_;
  int event_fd_{-1};
  BlockingQueue<DbJob> jobs_;
  std::vector<std::thread> workers_;

  std::mutex completions_mutex_;
  std::queue<DbCompletion> completions_;
  bool started_{false};
};

}  // namespace portfolio
