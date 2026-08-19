#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace portfolio {

struct SessionRef {
  int fd{-1};
  std::uint64_t generation{0};

  bool operator==(const SessionRef& other) const {
    return fd == other.fd && generation == other.generation;
  }
};

struct SessionRefHash {
  std::size_t operator()(const SessionRef& ref) const {
    return std::hash<int>{}(ref.fd) ^ (std::hash<std::uint64_t>{}(ref.generation) << 1U);
  }
};

struct SectorCoord {
  int x{0};
  int y{0};

  bool operator==(const SectorCoord& other) const { return x == other.x && y == other.y; }
};

struct SectorCoordHash {
  std::size_t operator()(const SectorCoord& coord) const {
    return std::hash<int>{}(coord.x) ^ (std::hash<int>{}(coord.y) << 1U);
  }
};

struct WorldEvent {
  SessionRef receiver;
  std::string line;
};

class SectorWorld {
 public:
  explicit SectorWorld(int sector_size = 100, int view_radius = 1);

  std::vector<WorldEvent> enter(SessionRef ref, std::string account, std::string character, int x,
                                int y);
  std::vector<WorldEvent> move(SessionRef ref, int x, int y);
  std::vector<WorldEvent> attack(SessionRef ref, std::string skill, std::string target);
  std::vector<WorldEvent> leave(SessionRef ref);

 private:
  struct PlayerState {
    SessionRef ref;
    std::string account;
    std::string character;
    int x{0};
    int y{0};
    SectorCoord sector;
  };

  SectorCoord sector_for(int x, int y) const;
  std::unordered_set<SessionRef, SessionRefHash> watchers_around(SectorCoord sector) const;
  void insert_into_sector(const PlayerState& player);
  void erase_from_sector(SessionRef ref, SectorCoord sector);
  std::string sector_string(SectorCoord sector) const;
  std::string enter_line(const PlayerState& player) const;
  std::string move_line(const PlayerState& player) const;
  std::string despawn_line(const PlayerState& player, const std::string& reason) const;

  int sector_size_{100};
  int view_radius_{1};
  std::unordered_map<SessionRef, PlayerState, SessionRefHash> players_;
  std::unordered_map<SectorCoord, std::unordered_set<SessionRef, SessionRefHash>, SectorCoordHash>
      sectors_;
};

}  // namespace portfolio
