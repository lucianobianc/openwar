#ifndef OPENWAR_COMBAT_HPP
#define OPENWAR_COMBAT_HPP

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace openwar {

enum class FactionId : std::uint8_t;
enum class SectorId : std::uint32_t;
enum class UnitId : std::uint32_t;
enum class StockpileId : std::uint32_t;
enum class InstallationId : std::uint32_t;

enum class ControlState { neutral, secure, contested, unstable };
enum class GroundStatus { holding, withdrawing, broken, surrendered, destroyed };
enum class WarStatus { ongoing, ceasefire_pending, west_victory, east_victory, draw };

struct CombatUnit {
    UnitId unit;
    StockpileId stores;
    std::uint32_t morale = 80;
    std::uint32_t cohesion = 80;
    std::uint32_t fatigue = 0;
    std::uint32_t experience = 25;
    GroundStatus status = GroundStatus::holding;
    std::optional<SectorId> withdrawal{};
    std::uint32_t suppression = 0;
};

struct VictoryObjective {
    FactionId faction;
    std::vector<SectorId> sectors;
    std::uint64_t hold_seconds;
    std::vector<InstallationId> required_installations{};
};

struct CommandObjectives {
    FactionId faction;
    std::vector<SectorId> sectors;
};

struct CombatSettings {
    bool enabled = false;
    std::uint32_t interval_ticks = 1;
    std::uint64_t occupation_seconds = 30 * 60;
    std::uint64_t critical_occupation_seconds = 120 * 60;
    std::uint64_t stabilization_seconds = 30 * 60;
    std::uint64_t capitulation_seconds = 6 * 3600;
    std::uint64_t reinforcement_horizon_seconds = 24 * 3600;
    std::optional<std::uint64_t> time_limit_seconds{};
};

struct BattleEvent {
    std::uint64_t tick;
    SectorId sector;
    std::string_view reason;
    std::optional<UnitId> unit{};
    std::uint32_t amount = 0;
    std::uint64_t sequence = 0;
    std::optional<FactionId> previous_controller{};
    bool operator==(const BattleEvent&) const = default;
};

struct WarResult {
    WarStatus status = WarStatus::ongoing;
    std::uint64_t tick = 0;
    std::string_view cause{};
    bool operator==(const WarResult&) const = default;
};

std::string_view control_name(ControlState state);
std::string_view ground_status_name(GroundStatus status);
std::string_view war_status_name(WarStatus status);

}  // namespace openwar

#endif
