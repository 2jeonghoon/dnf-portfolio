#include "portfolio/database.h"

#include <mysql.h>
#include <mysqld_error.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace portfolio {
namespace {

std::string mysql_error_message(MYSQL* connection, const std::string& context) {
  return context + ": " + mysql_error(connection);
}

std::string stmt_error_message(MYSQL_STMT* statement, const std::string& context) {
  return context + ": " + mysql_stmt_error(statement);
}

struct MysqlParam {
  enum class Type {
    kString,
    kUInt32,
    kUInt64,
    kInt32,
  };

  static MysqlParam string(std::string value) {
    MysqlParam param;
    param.type = Type::kString;
    param.string_value = std::move(value);
    param.length = static_cast<unsigned long>(param.string_value.size());
    return param;
  }

  static MysqlParam uint32(unsigned int value) {
    MysqlParam param;
    param.type = Type::kUInt32;
    param.u32_value = value;
    return param;
  }

  static MysqlParam uint64(unsigned long long value) {
    MysqlParam param;
    param.type = Type::kUInt64;
    param.u64_value = value;
    return param;
  }

  static MysqlParam int32(int value) {
    MysqlParam param;
    param.type = Type::kInt32;
    param.i32_value = value;
    return param;
  }

  Type type{Type::kString};
  std::string string_value;
  unsigned int u32_value{0};
  unsigned long long u64_value{0};
  int i32_value{0};
  unsigned long length{0};
};

class MysqlStatement {
 public:
  MysqlStatement(MYSQL* connection, const char* sql) : statement_(mysql_stmt_init(connection)) {
    if (statement_ == nullptr) {
      throw std::runtime_error(mysql_error_message(connection, "mysql_stmt_init failed"));
    }
    if (mysql_stmt_prepare(statement_, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
      std::string message = stmt_error_message(statement_, "mysql_stmt_prepare failed");
      mysql_stmt_close(statement_);
      statement_ = nullptr;
      throw std::runtime_error(message);
    }
  }

  ~MysqlStatement() {
    if (statement_ != nullptr) {
      mysql_stmt_close(statement_);
    }
  }

  MysqlStatement(const MysqlStatement&) = delete;
  MysqlStatement& operator=(const MysqlStatement&) = delete;

  MYSQL_STMT* get() { return statement_; }

  void bind_params(std::vector<MysqlParam>& params) {
    if (params.empty()) {
      return;
    }

    param_binds_.assign(params.size(), MYSQL_BIND{});
    for (std::size_t i = 0; i < params.size(); ++i) {
      MYSQL_BIND& bind = param_binds_[i];
      MysqlParam& param = params[i];

      switch (param.type) {
        case MysqlParam::Type::kString:
          bind.buffer_type = MYSQL_TYPE_STRING;
          bind.buffer = const_cast<char*>(param.string_value.data());
          bind.buffer_length = static_cast<unsigned long>(param.string_value.size());
          bind.length = &param.length;
          break;
        case MysqlParam::Type::kUInt32:
          bind.buffer_type = MYSQL_TYPE_LONG;
          bind.buffer = &param.u32_value;
          bind.is_unsigned = true;
          break;
        case MysqlParam::Type::kInt32:
          bind.buffer_type = MYSQL_TYPE_LONG;
          bind.buffer = &param.i32_value;
          bind.is_unsigned = false;
          break;
        case MysqlParam::Type::kUInt64:
          bind.buffer_type = MYSQL_TYPE_LONGLONG;
          bind.buffer = &param.u64_value;
          bind.is_unsigned = true;
          break;
      }
    }

    if (mysql_stmt_bind_param(statement_, param_binds_.data()) != 0) {
      throw std::runtime_error(stmt_error_message(statement_, "mysql_stmt_bind_param failed"));
    }
  }

  void execute() {
    if (mysql_stmt_execute(statement_) != 0) {
      throw std::runtime_error(stmt_error_message(statement_, "mysql_stmt_execute failed"));
    }
  }

 private:
  MYSQL_STMT* statement_{nullptr};
  std::vector<MYSQL_BIND> param_binds_;
};

class MysqlConnection {
 public:
  explicit MysqlConnection(const DatabaseConfig& config) : connection_(mysql_init(nullptr)) {
    if (connection_ == nullptr) {
      throw std::runtime_error("mysql_init failed");
    }

    const char* charset = "utf8mb4";
    mysql_options(connection_, MYSQL_SET_CHARSET_NAME, charset);

    if (mysql_real_connect(connection_, config.host.c_str(), config.user.c_str(),
                           config.password.c_str(), config.schema.c_str(), config.port, nullptr,
                           0) == nullptr) {
      std::string message = mysql_error_message(connection_, "mysql_real_connect failed");
      mysql_close(connection_);
      connection_ = nullptr;
      throw std::runtime_error(message);
    }
  }

  ~MysqlConnection() {
    if (connection_ != nullptr) {
      mysql_close(connection_);
    }
  }

  MysqlConnection(const MysqlConnection&) = delete;
  MysqlConnection& operator=(const MysqlConnection&) = delete;

  void exec(const char* sql) {
    if (mysql_query(connection_, sql) != 0) {
      throw std::runtime_error(mysql_error_message(connection_, sql));
    }

    do {
      MYSQL_RES* result = mysql_store_result(connection_);
      if (result != nullptr) {
        mysql_free_result(result);
      }
    } while (mysql_next_result(connection_) == 0);
  }

  void execute_prepared(const char* sql, std::vector<MysqlParam> params) {
    MysqlStatement statement(connection_, sql);
    statement.bind_params(params);
    statement.execute();
  }

  void begin() { exec("BEGIN"); }
  void commit() { exec("COMMIT"); }
  void rollback_noexcept() noexcept {
    if (connection_ != nullptr) {
      mysql_query(connection_, "ROLLBACK");
    }
  }

  unsigned long long select_player_id_for_update(const std::string& account) {
    return select_player_id_impl("SELECT id FROM players WHERE account = ? FOR UPDATE", account);
  }

  unsigned long long select_player_id(const std::string& account) {
    return select_player_id_impl("SELECT id FROM players WHERE account = ?", account);
  }

  unsigned long long select_player_id_impl(const char* sql, const std::string& account) {
    MysqlStatement statement(connection_, sql);
    auto params = std::vector<MysqlParam>{MysqlParam::string(account)};
    statement.bind_params(params);
    statement.execute();

    unsigned long long id = 0;
    bool is_null = false;
    bool error = false;
    MYSQL_BIND result{};
    result.buffer_type = MYSQL_TYPE_LONGLONG;
    result.buffer = &id;
    result.is_unsigned = true;
    result.is_null = &is_null;
    result.error = &error;

    if (mysql_stmt_bind_result(statement.get(), &result) != 0) {
      throw std::runtime_error(stmt_error_message(statement.get(), "bind result failed"));
    }
    if (mysql_stmt_store_result(statement.get()) != 0) {
      throw std::runtime_error(stmt_error_message(statement.get(), "store result failed"));
    }

    const int fetch_status = mysql_stmt_fetch(statement.get());
    if (fetch_status == MYSQL_NO_DATA) {
      throw std::runtime_error("unknown account: " + account);
    }
    if (fetch_status != 0 && fetch_status != MYSQL_DATA_TRUNCATED) {
      throw std::runtime_error(stmt_error_message(statement.get(), "fetch player failed"));
    }
    if (is_null || error) {
      throw std::runtime_error("invalid player id for account: " + account);
    }
    return id;
  }

  DbCompletion register_player(const DbJob& job) {
    const std::string& account = job.args.at(0);
    const std::string& display_name = job.args.at(1);

    MysqlStatement statement(connection_, "INSERT INTO players(account, display_name) VALUES (?, ?)");
    auto params =
        std::vector<MysqlParam>{MysqlParam::string(account), MysqlParam::string(display_name)};
    statement.bind_params(params);
    if (mysql_stmt_execute(statement.get()) != 0) {
      if (mysql_stmt_errno(statement.get()) == ER_DUP_ENTRY) {
        throw std::runtime_error("account already exists: " + account);
      }
      throw std::runtime_error(stmt_error_message(statement.get(), "mysql_stmt_execute failed"));
    }

    DbCompletion completion = base_completion(job);
    completion.ok = true;
    completion.line = "OK REGISTER account=" + account + " display_name=" + display_name;
    return completion;
  }

  DbCompletion login_player(const DbJob& job) {
    const std::string& account = job.args.at(0);

    MysqlStatement statement(connection_,
                             "SELECT id, display_name FROM players WHERE account = ?");
    auto params = std::vector<MysqlParam>{MysqlParam::string(account)};
    statement.bind_params(params);
    statement.execute();

    unsigned long long id = 0;
    char display_name[128]{};
    unsigned long display_name_length = 0;
    bool id_null = false;
    bool name_null = false;
    bool id_error = false;
    bool name_error = false;

    MYSQL_BIND results[2]{};
    results[0].buffer_type = MYSQL_TYPE_LONGLONG;
    results[0].buffer = &id;
    results[0].is_unsigned = true;
    results[0].is_null = &id_null;
    results[0].error = &id_error;
    results[1].buffer_type = MYSQL_TYPE_STRING;
    results[1].buffer = display_name;
    results[1].buffer_length = sizeof(display_name);
    results[1].length = &display_name_length;
    results[1].is_null = &name_null;
    results[1].error = &name_error;

    if (mysql_stmt_bind_result(statement.get(), results) != 0) {
      throw std::runtime_error(stmt_error_message(statement.get(), "bind result failed"));
    }
    if (mysql_stmt_store_result(statement.get()) != 0) {
      throw std::runtime_error(stmt_error_message(statement.get(), "store result failed"));
    }

    const int fetch_status = mysql_stmt_fetch(statement.get());
    if (fetch_status == MYSQL_NO_DATA) {
      throw std::runtime_error("unknown account: " + account);
    }
    if (fetch_status != 0 && fetch_status != MYSQL_DATA_TRUNCATED) {
      throw std::runtime_error(stmt_error_message(statement.get(), "fetch player failed"));
    }
    if (id_null || name_null || id_error || name_error) {
      throw std::runtime_error("invalid player row for account: " + account);
    }

    execute_prepared("UPDATE players SET last_login_at = NOW() WHERE id = ?",
                     {MysqlParam::uint64(id)});

    DbCompletion completion = base_completion(job);
    completion.ok = true;
    completion.line = "OK LOGIN player_id=" + std::to_string(id) + " account=" + account +
                      " display_name=" +
                      std::string(display_name, display_name + display_name_length);
    return completion;
  }

  DbCompletion save_character(const DbJob& job) {
    const std::string& account = job.args.at(0);
    const std::string& character = job.args.at(1);
    const unsigned int level = static_cast<unsigned int>(std::stoul(job.args.at(2)));
    const unsigned long long gold = std::stoull(job.args.at(3));

    begin();
    try {
      const unsigned long long player_id = select_player_id_for_update(account);
      execute_prepared(
          "INSERT INTO characters(player_id, name, level, gold) VALUES (?, ?, ?, ?) "
          "ON DUPLICATE KEY UPDATE level = VALUES(level), gold = VALUES(gold)",
          {MysqlParam::uint64(player_id), MysqlParam::string(character), MysqlParam::uint32(level),
           MysqlParam::uint64(gold)});
      commit();
    } catch (...) {
      rollback_noexcept();
      throw;
    }

    DbCompletion completion = base_completion(job);
    completion.ok = true;
    completion.line = "OK SAVE account=" + account + " character=" + character +
                      " level=" + std::to_string(level) + " gold=" + std::to_string(gold);
    return completion;
  }

  DbCompletion save_position(const DbJob& job) {
    const std::string& account = job.args.at(0);
    const std::string& character = job.args.at(1);
    const int x = std::stoi(job.args.at(2));
    const int y = std::stoi(job.args.at(3));

    begin();
    try {
      const unsigned long long player_id = select_player_id_for_update(account);
      execute_prepared(
          "INSERT INTO characters(player_id, name, level, gold, x, y) VALUES (?, ?, 1, 0, ?, ?) "
          "ON DUPLICATE KEY UPDATE x = VALUES(x), y = VALUES(y)",
          {MysqlParam::uint64(player_id), MysqlParam::string(character), MysqlParam::int32(x),
           MysqlParam::int32(y)});
      commit();
    } catch (...) {
      rollback_noexcept();
      throw;
    }

    DbCompletion completion = base_completion(job);
    completion.ok = true;
    completion.line = "OK SAVE_POSITION account=" + account + " character=" + character +
                      " x=" + std::to_string(x) + " y=" + std::to_string(y);
    return completion;
  }

  DbCompletion load_characters(const DbJob& job) {
    const std::string& account = job.args.at(0);
    select_player_id(account);

    MysqlStatement statement(
        connection_,
        "SELECT c.name, c.level, c.gold, c.x, c.y "
        "FROM players p JOIN characters c ON c.player_id = p.id "
        "WHERE p.account = ? ORDER BY c.updated_at DESC, c.id DESC");
    auto params = std::vector<MysqlParam>{MysqlParam::string(account)};
    statement.bind_params(params);
    statement.execute();

    char name[128]{};
    unsigned long name_length = 0;
    unsigned int level = 0;
    unsigned long long gold = 0;
    int x = 0;
    int y = 0;
    bool name_null = false;
    bool level_null = false;
    bool gold_null = false;
    bool x_null = false;
    bool y_null = false;
    bool name_error = false;
    bool level_error = false;
    bool gold_error = false;
    bool x_error = false;
    bool y_error = false;

    MYSQL_BIND results[5]{};
    results[0].buffer_type = MYSQL_TYPE_STRING;
    results[0].buffer = name;
    results[0].buffer_length = sizeof(name);
    results[0].length = &name_length;
    results[0].is_null = &name_null;
    results[0].error = &name_error;
    results[1].buffer_type = MYSQL_TYPE_LONG;
    results[1].buffer = &level;
    results[1].is_unsigned = true;
    results[1].is_null = &level_null;
    results[1].error = &level_error;
    results[2].buffer_type = MYSQL_TYPE_LONGLONG;
    results[2].buffer = &gold;
    results[2].is_unsigned = true;
    results[2].is_null = &gold_null;
    results[2].error = &gold_error;
    results[3].buffer_type = MYSQL_TYPE_LONG;
    results[3].buffer = &x;
    results[3].is_unsigned = false;
    results[3].is_null = &x_null;
    results[3].error = &x_error;
    results[4].buffer_type = MYSQL_TYPE_LONG;
    results[4].buffer = &y;
    results[4].is_unsigned = false;
    results[4].is_null = &y_null;
    results[4].error = &y_error;

    if (mysql_stmt_bind_result(statement.get(), results) != 0) {
      throw std::runtime_error(stmt_error_message(statement.get(), "bind result failed"));
    }
    if (mysql_stmt_store_result(statement.get()) != 0) {
      throw std::runtime_error(stmt_error_message(statement.get(), "store result failed"));
    }

    std::vector<std::string> characters;
    while (true) {
      const int fetch_status = mysql_stmt_fetch(statement.get());
      if (fetch_status == MYSQL_NO_DATA) {
        break;
      }
      if (fetch_status != 0 && fetch_status != MYSQL_DATA_TRUNCATED) {
        throw std::runtime_error(stmt_error_message(statement.get(), "fetch character failed"));
      }
      if (name_null || level_null || gold_null || x_null || y_null || name_error || level_error ||
          gold_error || x_error || y_error) {
        throw std::runtime_error("invalid character row for account: " + account);
      }
      std::ostringstream row;
      row << std::string(name, name + name_length) << ":" << level << ":" << gold << ":" << x << ":"
          << y;
      characters.push_back(row.str());
    }

    std::ostringstream output;
    output << "OK LOAD account=" << account << " character_count=" << characters.size();
    if (!characters.empty()) {
      output << " characters=";
      for (std::size_t i = 0; i < characters.size(); ++i) {
        if (i > 0) {
          output << ",";
        }
        output << characters[i];
      }
    }

    DbCompletion completion = base_completion(job);
    completion.ok = true;
    completion.line = output.str();
    return completion;
  }

  DbCompletion save_inventory_item(const DbJob& job) {
    const std::string& account = job.args.at(0);
    const unsigned int slot = static_cast<unsigned int>(std::stoul(job.args.at(1)));
    const unsigned long long item_id = std::stoull(job.args.at(2));
    const unsigned int count = static_cast<unsigned int>(std::stoul(job.args.at(3)));

    begin();
    try {
      const unsigned long long player_id = select_player_id_for_update(account);
      if (count == 0) {
        execute_prepared("DELETE FROM inventory_items WHERE player_id = ? AND slot = ?",
                         {MysqlParam::uint64(player_id), MysqlParam::uint32(slot)});
      } else {
        execute_prepared(
            "INSERT INTO inventory_items(player_id, slot, item_id, stack_count) "
            "VALUES (?, ?, ?, ?) "
            "ON DUPLICATE KEY UPDATE item_id = VALUES(item_id), "
            "stack_count = VALUES(stack_count)",
            {MysqlParam::uint64(player_id), MysqlParam::uint32(slot), MysqlParam::uint64(item_id),
             MysqlParam::uint32(count)});
      }
      commit();
    } catch (...) {
      rollback_noexcept();
      throw;
    }

    DbCompletion completion = base_completion(job);
    completion.ok = true;
    completion.line = "OK SAVE_ITEM account=" + account + " slot=" + std::to_string(slot) +
                      " item_id=" + std::to_string(item_id) +
                      " count=" + std::to_string(count);
    return completion;
  }

  DbCompletion load_inventory(const DbJob& job) {
    const std::string& account = job.args.at(0);
    select_player_id(account);

    MysqlStatement statement(
        connection_,
        "SELECT i.slot, i.item_id, i.stack_count "
        "FROM players p JOIN inventory_items i ON i.player_id = p.id "
        "WHERE p.account = ? ORDER BY i.slot");
    auto params = std::vector<MysqlParam>{MysqlParam::string(account)};
    statement.bind_params(params);
    statement.execute();

    unsigned int slot = 0;
    unsigned long long item_id = 0;
    unsigned int count = 0;
    bool slot_null = false;
    bool item_null = false;
    bool count_null = false;
    bool slot_error = false;
    bool item_error = false;
    bool count_error = false;

    MYSQL_BIND results[3]{};
    results[0].buffer_type = MYSQL_TYPE_LONG;
    results[0].buffer = &slot;
    results[0].is_unsigned = true;
    results[0].is_null = &slot_null;
    results[0].error = &slot_error;
    results[1].buffer_type = MYSQL_TYPE_LONGLONG;
    results[1].buffer = &item_id;
    results[1].is_unsigned = true;
    results[1].is_null = &item_null;
    results[1].error = &item_error;
    results[2].buffer_type = MYSQL_TYPE_LONG;
    results[2].buffer = &count;
    results[2].is_unsigned = true;
    results[2].is_null = &count_null;
    results[2].error = &count_error;

    if (mysql_stmt_bind_result(statement.get(), results) != 0) {
      throw std::runtime_error(stmt_error_message(statement.get(), "bind result failed"));
    }
    if (mysql_stmt_store_result(statement.get()) != 0) {
      throw std::runtime_error(stmt_error_message(statement.get(), "store result failed"));
    }

    std::vector<std::string> items;
    while (true) {
      const int fetch_status = mysql_stmt_fetch(statement.get());
      if (fetch_status == MYSQL_NO_DATA) {
        break;
      }
      if (fetch_status != 0 && fetch_status != MYSQL_DATA_TRUNCATED) {
        throw std::runtime_error(stmt_error_message(statement.get(), "fetch inventory failed"));
      }
      if (slot_null || item_null || count_null || slot_error || item_error || count_error) {
        throw std::runtime_error("invalid inventory row for account: " + account);
      }

      std::ostringstream row;
      row << slot << ":" << item_id << ":" << count;
      items.push_back(row.str());
    }

    std::ostringstream output;
    output << "OK LOAD_INVENTORY account=" << account << " item_count=" << items.size();
    if (!items.empty()) {
      output << " items=";
      for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
          output << ",";
        }
        output << items[i];
      }
    }

    DbCompletion completion = base_completion(job);
    completion.ok = true;
    completion.line = output.str();
    return completion;
  }

 private:
  static DbCompletion base_completion(const DbJob& job) {
    DbCompletion completion;
    completion.client_fd = job.client_fd;
    completion.client_generation = job.client_generation;
    completion.request_id = job.request_id;
    completion.notify_client = job.notify_client;
    return completion;
  }

  MYSQL* connection_{nullptr};
};

DbCompletion error_completion(const DbJob& job, const std::exception& error) {
  DbCompletion completion;
  completion.client_fd = job.client_fd;
  completion.client_generation = job.client_generation;
  completion.request_id = job.request_id;
  completion.ok = false;
  completion.line = "ERR " + job.command_name + " " + error.what();
  completion.notify_client = job.notify_client;
  return completion;
}

DbCompletion execute_job(MysqlConnection& connection, const DbJob& job) {
  switch (job.operation) {
    case DbOperation::kRegisterPlayer:
      return connection.register_player(job);
    case DbOperation::kLoginPlayer:
      return connection.login_player(job);
    case DbOperation::kSaveCharacter:
      return connection.save_character(job);
    case DbOperation::kLoadCharacters:
      return connection.load_characters(job);
    case DbOperation::kSaveInventoryItem:
      return connection.save_inventory_item(job);
    case DbOperation::kLoadInventory:
      return connection.load_inventory(job);
    case DbOperation::kSavePosition:
      return connection.save_position(job);
  }

  throw std::runtime_error("unsupported database operation");
}

}  // namespace

MysqlLibrary::MysqlLibrary() {
  if (mysql_library_init(0, nullptr, nullptr) != 0) {
    throw std::runtime_error("mysql_library_init failed");
  }
}

MysqlLibrary::~MysqlLibrary() {
  mysql_library_end();
}

DbDispatcher::DbDispatcher(DatabaseConfig config) : config_(std::move(config)) {
  event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (event_fd_ < 0) {
    throw std::runtime_error(std::string("eventfd failed: ") + std::strerror(errno));
  }
}

DbDispatcher::~DbDispatcher() {
  stop();
  if (event_fd_ >= 0) {
    close(event_fd_);
  }
}

void DbDispatcher::start() {
  if (started_) {
    return;
  }
  started_ = true;
  workers_.reserve(config_.pool_size);
  for (unsigned int i = 0; i < config_.pool_size; ++i) {
    workers_.emplace_back(&DbDispatcher::worker_loop, this, i);
  }
}

void DbDispatcher::stop() {
  if (!started_) {
    return;
  }

  jobs_.close();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  started_ = false;
}

void DbDispatcher::submit(DbJob job) {
  jobs_.push(std::move(job));
}

bool DbDispatcher::pop_completion(DbCompletion& completion) {
  std::lock_guard<std::mutex> lock(completions_mutex_);
  if (completions_.empty()) {
    return false;
  }
  completion = std::move(completions_.front());
  completions_.pop();
  return true;
}

void DbDispatcher::worker_loop(unsigned int) {
  mysql_thread_init();
  std::unique_ptr<MysqlConnection> connection;
  DbJob job;

  while (jobs_.pop(job)) {
    try {
      if (!connection) {
        connection.reset(new MysqlConnection(config_));
      }
      push_completion(execute_job(*connection, job));
    } catch (const std::exception& error) {
      push_completion(error_completion(job, error));
    }
  }

  mysql_thread_end();
}

void DbDispatcher::push_completion(DbCompletion completion) {
  {
    std::lock_guard<std::mutex> lock(completions_mutex_);
    completions_.push(std::move(completion));
  }

  std::uint64_t increment = 1;
  while (write(event_fd_, &increment, sizeof(increment)) < 0) {
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EBADF) {
      break;
    }
    break;
  }
}

}  // namespace portfolio
