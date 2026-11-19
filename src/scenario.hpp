#ifndef OPENWAR_SCENARIO_HPP
#define OPENWAR_SCENARIO_HPP

#include "campaign.hpp"

#include <filesystem>

namespace openwar {

inline constexpr std::uint32_t procedural_generator_version = 1;

struct ProceduralSettings {
    std::uint64_t seed = 0;
    std::uint32_t width = 9;
    std::uint32_t height = 7;
    std::uint32_t sector_size_m = 2000;
    std::uint32_t crossings = 3;
    std::uint32_t forest_percent = 30;
    std::uint32_t water_percent = 10;
    std::uint32_t west_armor = 3;
    std::uint32_t east_armor = 3;
    std::uint32_t reserve_armor = 1;
    std::uint32_t scout_flights = 2;
    std::uint32_t oil = 600;
    std::uint32_t materials = 240;
    std::uint32_t ammunition = 90;
    std::uint32_t hold_minutes = 120;
    std::uint32_t time_limit_minutes = 48 * 60;
    std::uint32_t require_relay = 1;
    std::uint32_t capitulation = 1;
};

struct GeographicWorld {
    WorldDefinition grid;
    geography::Map map;
};

struct GeographicHeader {
    int version;
    std::int64_t columns, rows, sector_mm, tile_mm;
    std::string provenance;
};

struct MilitaryScenarioSummary {
    std::filesystem::path world;
    std::string name;
    std::string description;
    std::set<FactionId> factions;
    std::vector<std::pair<FactionId, std::string>> objectives;
    std::optional<std::uint64_t> time_limit_seconds;
};

GeographicHeader inspect_geographic_header(const std::filesystem::path& path);
std::filesystem::path referenced_military_world(const std::filesystem::path& path);
MilitaryScenarioSummary inspect_military_scenario(const std::filesystem::path& path);
GeographicWorld read_geographic_world(const std::filesystem::path& path);
bool is_geographic_scenario_file(const std::filesystem::path& path);
ScenarioDefinition read_geographic_scenario(const std::filesystem::path& path,
    std::optional<FactionId> player = {});

// Resolve authored settings before initializing any live campaign state.
ScenarioDefinition customize_scenario(ScenarioDefinition scenario, const std::filesystem::path& source,
                                     const CampaignOptions& options);
CampaignState open_campaign_scenario(const std::filesystem::path& path, std::optional<FactionId> player = {},
                                     std::optional<std::uint64_t> seed = {});
ProceduralSettings read_procedural_settings(const std::filesystem::path& path);
ScenarioDefinition resolve_procedural_scenario(const ProceduralSettings& settings);
CampaignState procedural_campaign(const ProceduralSettings& settings);

}  // namespace openwar

#endif
