#ifndef OPENWAR_OPERATIONS_HPP
#define OPENWAR_OPERATIONS_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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
enum class MissionKind { recon, patrol, defend, reposition, attack };
enum class MissionStatus { scheduled, preparing, in_progress, completed, failed, cancelled };

struct OperationsSettings {
    bool enabled = false;
    std::uint32_t planner_interval_ticks = 0;
    std::uint32_t territory_half_life_seconds = 360 * 60;
    std::uint32_t contact_half_life_seconds = 30 * 60;
    std::uint32_t installation_half_life_seconds = 1440 * 60;
    std::optional<FactionId> player{};
    std::size_t max_autonomous_missions = 1;
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
};

struct ContactReport {
    ContactId id;
    SectorId estimated_sector;
    std::string_view classification;
    std::uint64_t observed_tick;
    std::uint16_t confidence;
};

struct InstallationReport {
    SectorId sector;
    std::uint64_t observed_tick;
    std::uint16_t confidence;
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
    std::size_t ground_leg = 0;
    bool preparation_reserved = false;
    std::uint64_t ground_progress = 0;
    bool operator==(const Mission&) const = default;
};

struct OperationsEvent {
    std::uint64_t tick;
    FactionId faction;
    SectorId sector;
    std::optional<MissionId> mission;
    std::string_view reason;
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
};

// A complete sector survey supersedes contacts that it did not observe again.
inline bool current_contact(const ContactReport& contact, const SectorReport& report)
{
    return contact.estimated_sector == report.sector &&
           (!report.surveyed || contact.observed_tick >= report.observed_tick);
}

struct OrderResult {
    std::optional<MissionId> mission;
    std::string_view reason;
};

std::string_view mission_name(MissionKind kind);
std::string_view mission_status_name(MissionStatus status);
std::string_view confidence_name(std::uint16_t confidence);

}  // namespace openwar

#endif
