#pragma once

#include <string>
#include <vector>

namespace portfolio {

enum class CommandType {
  kRegister,
  kLogin,
  kSaveCharacter,
  kLoadCharacters,
  kEnterWorld,
  kMove,
  kAttack,
  kSaveInventoryItem,
  kLoadInventory,
  kQuit,
  kUnknown,
};

struct Request {
  CommandType type{CommandType::kUnknown};
  std::string command_name;
  std::vector<std::string> args;
};

struct ParseResult {
  bool ok{false};
  Request request;
  std::string error;
};

ParseResult parse_line(const std::string& line);

}  // namespace portfolio
