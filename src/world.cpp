#include "portfolio/world.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace portfolio {
namespace {

int floor_div(int value, int divisor) {
  if (divisor <= 0) {
    throw std::runtime_error("sector size must be positive");
  }
  if (value >= 0) {
    return value / divisor;
  }
  return -((-value + divisor - 1) / divisor);
}

bool contains(const std::unordered_set<SessionRef, SessionRefHash>& set, SessionRef ref) {
  return set.find(ref) != set.end();
}

}  // namespace

SectorWorld::SectorWorld(int sector_size, int view_radius)
    : sector_size_(sector_size), view_radius_(view_radius) {
  if (sector_size_ <= 0) {
    throw std::runtime_error("sector size must be positive");
  }
  if (view_radius_ < 0) {
    throw std::runtime_error("view radius must be non-negative");
  }
}

std::vector<WorldEvent> SectorWorld::enter(SessionRef ref, std::string account,
                                           std::string character, int x, int y) {
  std::vector<WorldEvent> events = leave(ref);

  PlayerState player;
  player.ref = ref;
  player.account = std::move(account);
  player.character = std::move(character);
  player.x = x;
  player.y = y;
  player.sector = sector_for(x, y);

  players_[ref] = player;
  insert_into_sector(player);

  const auto watchers = watchers_around(player.sector);
  for (const SessionRef& receiver : watchers) {
    if (receiver == ref) {
      events.push_back({receiver, "WORLD ENTERED " + player.character + " x=" +
                                      std::to_string(player.x) + " y=" +
                                      std::to_string(player.y) + " sector=" +
                                      sector_string(player.sector)});
    } else {
      events.push_back({receiver, enter_line(player)});
    }
  }

  for (const SessionRef& receiver : watchers) {
    if (receiver == ref) {
      continue;
    }
    auto other_it = players_.find(receiver);
    if (other_it != players_.end()) {
      events.push_back({ref, "EVT SNAPSHOT " + other_it->second.character + " account=" +
                                  other_it->second.account + " x=" +
                                  std::to_string(other_it->second.x) + " y=" +
                                  std::to_string(other_it->second.y) + " sector=" +
                                  sector_string(other_it->second.sector)});
    }
  }

  return events;
}

std::vector<WorldEvent> SectorWorld::move(SessionRef ref, int x, int y) {
  auto player_it = players_.find(ref);
  if (player_it == players_.end()) {
    return {{ref, "ERR not in world; use ENTER first"}};
  }

  PlayerState& player = player_it->second;
  const SectorCoord old_sector = player.sector;
  const auto old_watchers = watchers_around(old_sector);
  const SectorCoord new_sector = sector_for(x, y);

  if (!(old_sector == new_sector)) {
    erase_from_sector(ref, old_sector);
  }

  player.x = x;
  player.y = y;
  player.sector = new_sector;

  if (!(old_sector == new_sector)) {
    insert_into_sector(player);
  }

  const auto new_watchers = watchers_around(new_sector);
  std::unordered_set<SessionRef, SessionRefHash> recipients = old_watchers;
  recipients.insert(new_watchers.begin(), new_watchers.end());

  std::vector<WorldEvent> events;
  for (const SessionRef& receiver : recipients) {
    if (receiver == ref) {
      events.push_back({receiver, "WORLD MOVED " + player.character + " x=" +
                                      std::to_string(player.x) + " y=" +
                                      std::to_string(player.y) + " sector=" +
                                      sector_string(player.sector)});
      continue;
    }

    const bool was_visible = contains(old_watchers, receiver);
    const bool is_visible = contains(new_watchers, receiver);
    if (was_visible && is_visible) {
      events.push_back({receiver, move_line(player)});
    } else if (!was_visible && is_visible) {
      events.push_back({receiver, enter_line(player)});
    } else if (was_visible && !is_visible) {
      events.push_back({receiver, despawn_line(player, "sector_out")});
    }
  }

  return events;
}

std::vector<WorldEvent> SectorWorld::attack(SessionRef ref, std::string skill,
                                            std::string target) {
  auto player_it = players_.find(ref);
  if (player_it == players_.end()) {
    return {{ref, "ERR not in world; use ENTER first"}};
  }

  const PlayerState& player = player_it->second;
  const auto watchers = watchers_around(player.sector);

  std::vector<WorldEvent> events;
  for (const SessionRef& receiver : watchers) {
    if (receiver == ref) {
      events.push_back({receiver, "WORLD ATTACK skill=" + skill + " target=" + target});
    } else {
      events.push_back({receiver, "EVT ATTACK attacker=" + player.character + " skill=" +
                                      skill + " target=" + target + " x=" +
                                      std::to_string(player.x) + " y=" +
                                      std::to_string(player.y)});
    }
  }

  return events;
}

std::vector<WorldEvent> SectorWorld::leave(SessionRef ref) {
  auto player_it = players_.find(ref);
  if (player_it == players_.end()) {
    return {};
  }

  const PlayerState player = player_it->second;
  const auto watchers = watchers_around(player.sector);
  erase_from_sector(ref, player.sector);
  players_.erase(player_it);

  std::vector<WorldEvent> events;
  for (const SessionRef& receiver : watchers) {
    if (receiver == ref) {
      continue;
    }
    events.push_back({receiver, despawn_line(player, "leave")});
  }
  return events;
}

SectorCoord SectorWorld::sector_for(int x, int y) const {
  return {floor_div(x, sector_size_), floor_div(y, sector_size_)};
}

std::unordered_set<SessionRef, SessionRefHash> SectorWorld::watchers_around(
    SectorCoord sector) const {
  std::unordered_set<SessionRef, SessionRefHash> watchers;
  for (int dy = -view_radius_; dy <= view_radius_; ++dy) {
    for (int dx = -view_radius_; dx <= view_radius_; ++dx) {
      const SectorCoord current{sector.x + dx, sector.y + dy};
      auto sector_it = sectors_.find(current);
      if (sector_it == sectors_.end()) {
        continue;
      }
      watchers.insert(sector_it->second.begin(), sector_it->second.end());
    }
  }
  return watchers;
}

void SectorWorld::insert_into_sector(const PlayerState& player) {
  sectors_[player.sector].insert(player.ref);
}

void SectorWorld::erase_from_sector(SessionRef ref, SectorCoord sector) {
  auto sector_it = sectors_.find(sector);
  if (sector_it == sectors_.end()) {
    return;
  }

  sector_it->second.erase(ref);
  if (sector_it->second.empty()) {
    sectors_.erase(sector_it);
  }
}

std::string SectorWorld::sector_string(SectorCoord sector) const {
  return std::to_string(sector.x) + "," + std::to_string(sector.y);
}

std::string SectorWorld::enter_line(const PlayerState& player) const {
  return "EVT ENTER " + player.character + " account=" + player.account +
         " x=" + std::to_string(player.x) + " y=" + std::to_string(player.y) +
         " sector=" + sector_string(player.sector);
}

std::string SectorWorld::move_line(const PlayerState& player) const {
  return "EVT MOVE " + player.character + " x=" + std::to_string(player.x) +
         " y=" + std::to_string(player.y) + " sector=" + sector_string(player.sector);
}

std::string SectorWorld::despawn_line(const PlayerState& player, const std::string& reason) const {
  return "EVT DESPAWN " + player.character + " reason=" + reason +
         " sector=" + sector_string(player.sector);
}

}  // namespace portfolio
