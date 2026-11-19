#ifndef OPENWAR_OPERATIONS_HPP
#define OPENWAR_OPERATIONS_HPP

#include "combat.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openwar {

enum class FactionId : std::uint8_t;
enum class SectorId : std::uint32_t;
enum class EquipmentId : std::uint32_t;
enum class StockpileId : std::uint32_t;
enum class FacilityId : std::uint32_t;
enum class UnitId : std::uint32_t;
enum class MissionId : std::uint32_t {};
enum class ContactId : std::uint32_t {};
enum class MissionKind { recon, patrol, defend, reposition, attack, move, hold, ground_recon, ground_patrol };
enum class MissionStatus { scheduled, preparing, in_progress, completed, failed, cancelled, holding, released, observing };
enum class ReconResult { none, partial, observed };

constexpr bool position_order(MissionKind kind)
{
    return kind == MissionKind::move || kind == MissionKind::hold || kind == MissionKind::ground_patrol;
}

constexpr bool geographic_ground_order(MissionKind kind)
{
    return position_order(kind) || kind == MissionKind::ground_recon;
}

constexpr bool mission_active(MissionStatus status)
{
    return status == MissionStatus::scheduled || status == MissionStatus::preparing ||
           status == MissionStatus::in_progress || status == MissionStatus::holding || status == MissionStatus::observing;
}

enum class StrategicDoctrine { balanced, offensive, defensive };

struct OperationsSettings {
    bool enabled = false;
    std::uint32_t planner_interval_ticks = 0;
    std::uint32_t territory_half_life_seconds = 360 * 60;
    std::uint32_t contact_half_life_seconds = 30 * 60;
    std::uint32_t installation_half_life_seconds = 1440 * 60;
    std::optional<FactionId> player{};
    std::size_t max_autonomous_missions = 1;
    std::uint32_t strategy_review_ticks = 0;
    std::uint32_t reinforcement_travel_ticks = 30;
    std::uint32_t coordination_window_ticks = 8;
    // WEST and EAST respectively.
    std::array<StrategicDoctrine, 2> doctrines{};

    bool strategic_planning_enabled(bool combat_enabled) const
    {
        return enabled && planner_interval_ticks && strategy_review_ticks && combat_enabled;
    }
};

struct StrategicPlan {
    std::optional<SectorId> focus{};
    std::uint64_t reviewed_tick = 0;
    std::size_t ground_losses = 0;
    bool recovering = false;
    bool operator==(const StrategicPlan&) const = default;
};

struct Objective {
    FactionId faction;
    SectorId sector;
    std::uint32_t priority;
};

struct SectorReport {
    SectorId sector;
    FactionId estimated_controller;
    std::uint64_t observed_tick;
    std::uint16_t confidence;
    bool internal = false;
    bool contested = false;
    bool surveyed = false;
    std::optional<std::uint64_t> earlier_survey_tick{};
};

struct ContactReport {
    ContactId id;
    SectorId estimated_sector;
    std::string classification;
    std::uint64_t observed_tick;
    std::uint16_t confidence;
};

struct InstallationReport {
    SectorId sector;
    std::uint64_t observed_tick;
    std::uint16_t confidence;
};

struct RoadReport {
    bool closed;
    std::uint64_t observed_tick;
    std::uint16_t confidence = 1000;
    bool confirmed() const { return confidence >= 750; }
    bool operator==(const RoadReport&) const = default;
};

inline constexpr std::size_t max_ground_waypoints = 16;

struct GroundWaypoint {
    geography::Point point;
    std::size_t end_leg;
    bool operator==(const GroundWaypoint&) const = default;
};

struct Mission {
    MissionId id;
    FactionId faction;
    MissionKind kind;
    SectorId target;
    EquipmentId equipment;
    StockpileId launch_stock;
    std::uint64_t fuel;
    std::uint64_t duration_ticks;
    MissionStatus status = MissionStatus::scheduled;
    std::uint64_t launch_tick = 0;
    std::uint32_t detections = 0;
    bool manual = false;
    std::optional<FacilityId> relocation{};
    std::optional<UnitId> ground_unit{};
    std::vector<SectorId> ground_path{};
    bool preparation_reserved = false;
    std::uint64_t earliest_launch_tick = 0;
    std::optional<geography::Point> ground_destination{};
    std::optional<GroundJourney> ground_journey{};
    ReconResult recon_result = ReconResult::none;
    std::vector<GroundWaypoint> waypoints{};
    std::size_t waypoints_reached = 0;
    bool operator==(const Mission&) const = default;
};

struct OperationsEvent {
    std::uint64_t tick;
    FactionId faction;
    SectorId sector;
    std::optional<MissionId> mission;
    std::string reason;
    std::uint64_t sequence = 0;
    bool operator==(const OperationsEvent&) const = default;
};

struct FactionView {
    std::map<SectorId, SectorReport> sectors;
    std::vector<ContactReport> contacts;
    std::vector<InstallationReport> installations;
    std::vector<Objective> objectives;
    std::vector<Mission> missions;
    std::vector<OperationsEvent> events;
    std::optional<StrategicPlan> strategy{};
    std::map<std::uint32_t, RoadReport> roads;
};

// A complete sector survey supersedes contacts that it did not observe again.
inline bool current_contact(const ContactReport& contact, const SectorReport& report)
{
    const auto survey = report.surveyed ? std::optional{report.observed_tick} : report.earlier_survey_tick;
    return contact.estimated_sector == report.sector && (!survey || contact.observed_tick >= *survey);
}

struct MissionPlan {
    std::optional<Mission> candidate;
    std::string_view reason;
};

struct OrderResult {
    std::optional<MissionId> mission;
    std::string_view reason;
};

std::string_view mission_name(MissionKind kind);
std::string_view mission_status_name(MissionStatus status);
std::string_view ground_recon_result(const Mission& mission);
std::string_view confidence_name(std::uint16_t confidence);

}  // namespace openwar

#endif
