#include "portfolio/protocol.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>

namespace portfolio {
namespace {

std::string upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  return value;
}

bool is_unsigned_number(const std::string& value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch); });
}

bool is_signed_number(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  const std::size_t start = value.front() == '-' ? 1 : 0;
  return start < value.size() &&
         std::all_of(value.begin() + static_cast<std::ptrdiff_t>(start), value.end(),
                     [](unsigned char ch) { return std::isdigit(ch); });
}

ParseResult error(std::string message) {
  ParseResult result;
  result.ok = false;
  result.error = std::move(message);
  return result;
}

ParseResult ok(CommandType type, std::string command_name, std::vector<std::string> args) {
  ParseResult result;
  result.ok = true;
  result.request.type = type;
  result.request.command_name = std::move(command_name);
  result.request.args = std::move(args);
  return result;
}

}  // namespace

ParseResult parse_line(const std::string& original_line) {
  std::string line = original_line;
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }

  std::istringstream input(line);
  std::string command;
  input >> command;
  if (command.empty()) {
    return error("empty command");
  }

  std::vector<std::string> args;
  std::string token;
  while (input >> token) {
    args.push_back(token);
  }

  const std::string normalized = upper(command);
  if (normalized == "REGISTER") {
    if (args.size() != 2) {
      return error("usage: REGISTER <account> <display_name>");
    }
    return ok(CommandType::kRegister, normalized, std::move(args));
  }

  if (normalized == "LOGIN") {
    if (args.size() != 1) {
      return error("usage: LOGIN <account>");
    }
    return ok(CommandType::kLogin, normalized, std::move(args));
  }

  if (normalized == "SAVE") {
    if (args.size() != 4) {
      return error("usage: SAVE <account> <character> <level> <gold>");
    }
    if (!is_unsigned_number(args[2]) || !is_unsigned_number(args[3])) {
      return error("level and gold must be unsigned integers");
    }
    return ok(CommandType::kSaveCharacter, normalized, std::move(args));
  }

  if (normalized == "LOAD") {
    if (args.size() != 1) {
      return error("usage: LOAD <account>");
    }
    return ok(CommandType::kLoadCharacters, normalized, std::move(args));
  }

  if (normalized == "ENTER") {
    if (args.size() != 4) {
      return error("usage: ENTER <account> <character> <x> <y>");
    }
    if (!is_signed_number(args[2]) || !is_signed_number(args[3])) {
      return error("x and y must be signed integers");
    }
    return ok(CommandType::kEnterWorld, normalized, std::move(args));
  }

  if (normalized == "MOVE") {
    if (args.size() != 2) {
      return error("usage: MOVE <x> <y>");
    }
    if (!is_signed_number(args[0]) || !is_signed_number(args[1])) {
      return error("x and y must be signed integers");
    }
    return ok(CommandType::kMove, normalized, std::move(args));
  }

  if (normalized == "ATTACK") {
    if (args.size() != 2) {
      return error("usage: ATTACK <skill> <target>");
    }
    return ok(CommandType::kAttack, normalized, std::move(args));
  }

  if (normalized == "SAVE_ITEM") {
    if (args.size() != 4) {
      return error("usage: SAVE_ITEM <account> <slot> <item_id> <count>");
    }
    if (!is_unsigned_number(args[1]) || !is_unsigned_number(args[2]) ||
        !is_unsigned_number(args[3])) {
      return error("slot, item_id, and count must be unsigned integers");
    }
    return ok(CommandType::kSaveInventoryItem, normalized, std::move(args));
  }

  if (normalized == "LOAD_INVENTORY") {
    if (args.size() != 1) {
      return error("usage: LOAD_INVENTORY <account>");
    }
    return ok(CommandType::kLoadInventory, normalized, std::move(args));
  }

  if (normalized == "QUIT") {
    if (!args.empty()) {
      return error("usage: QUIT");
    }
    return ok(CommandType::kQuit, normalized, {});
  }

  return error("unknown command: " + command);
}

}  // namespace portfolio
