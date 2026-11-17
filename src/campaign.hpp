#ifndef OPENWAR_CAMPAIGN_HPP
#define OPENWAR_CAMPAIGN_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openwar {

enum class FactionId : std::uint8_t { neutral = 0, west = 1, east = 2 };
enum class SectorId : std::uint32_t {};
enum class Terrain { plains, forest, water };

std::string_view faction_name(FactionId faction);
std::string_view terrain_name(Terrain terrain);

struct SectorProperties {
    Terrain terrain = Terrain::plains;
    bool traversable = true;
};

struct Sector {
    SectorId id;
    std::uint32_t x;
    std::uint32_t y;
    SectorProperties properties;
    std::vector<SectorId> neighbors;
};

class WorldDefinition {
public:
    // Sector properties are supplied in row-major order.
    WorldDefinition(std::uint32_t width, std::uint32_t height,
                    std::uint32_t sector_size_m, std::vector<SectorProperties> properties);

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint32_t sector_size_m() const { return sector_size_m_; }
    std::span<const Sector> sectors() const { return sectors_; }
    const Sector& sector(SectorId id) const;
    SectorId sector_id(std::uint32_t x, std::uint32_t y) const;

private:
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t sector_size_m_;
    std::vector<Sector> sectors_;
};

struct SectorState {
    FactionId controller = FactionId::neutral;
    std::uint32_t objective_points = 0;
};

struct InitialSectorState {
    SectorId sector;
    SectorState state;
};

enum class Resource { oil_reserve, crude_oil, fuel, materials, spare_parts, supplies, count };
using ResourceAmounts = std::array<std::uint64_t, static_cast<std::size_t>(Resource::count)>;
enum class StockpileId : std::uint32_t {};
enum class InstallationId : std::uint32_t {};

std::string_view resource_name(Resource resource);

struct Stockpile {
    StockpileId id;
    SectorId sector;
    FactionId owner;
    ResourceAmounts capacity{};
    ResourceAmounts quantity{};
};

struct ProductionRecipe {
    ResourceAmounts inputs{};
    ResourceAmounts outputs{};
    std::uint32_t intervals_per_batch = 1;
    std::uint32_t workers_required = 1;
    bool requires_power = true;
};

struct Installation {
    InstallationId id;
    StockpileId stockpile;
    std::string name;
    FactionId operator_faction;
    ProductionRecipe recipe;
    std::uint32_t workers = 1;
    bool has_power = true;
    std::uint32_t integrity = 100;
};

struct InstallationState {
    Installation installation;
    // Each interval contributes up to 100 work points, scaled by integrity.
    std::uint64_t work = 0;
};

struct EconomyEntry {
    std::uint64_t tick;
    StockpileId stockpile;
    // Present for production. Direct consumption has no producing installation.
    std::optional<InstallationId> producer;
    ResourceAmounts consumed;
    ResourceAmounts produced;

    bool operator==(const EconomyEntry&) const = default;
};

struct ScenarioDefinition {
    WorldDefinition world;
    // Unlisted sectors start neutral.
    std::vector<InitialSectorState> initial_sectors;
    std::uint32_t tick_seconds = 60;
    std::uint32_t economy_interval_ticks = 5;
    std::uint32_t minimum_integrity = 25;
    std::vector<Stockpile> stockpiles{};
    std::vector<Installation> installations{};
};

class SimulationClock {
public:
    explicit SimulationClock(std::uint32_t tick_seconds);
    void advance(std::uint64_t ticks);
    std::uint64_t tick() const { return tick_; }
    std::uint64_t elapsed_seconds() const { return tick_ * tick_seconds_; }

private:
    std::uint32_t tick_seconds_;
    std::uint64_t tick_ = 0;
};

class CampaignState {
public:
    CampaignState(ScenarioDefinition scenario, std::uint32_t seed);

    const WorldDefinition& world() const { return world_; }
    const SimulationClock& clock() const { return clock_; }
    const SectorState& sector_state(SectorId sector) const;
    void advance_ticks(std::uint64_t ticks);
    std::uint32_t next_random();
    const std::map<StockpileId, Stockpile>& stockpiles() const { return stockpiles_; }
    std::span<const InstallationState> installations() const { return installations_; }
    std::span<const EconomyEntry> economy_ledger() const { return economy_ledger_; }
    bool consume(StockpileId stockpile, const ResourceAmounts& amounts);
    void set_operating_conditions(InstallationId id, bool has_power, std::uint32_t workers,
                                  std::uint32_t integrity);

private:
    void initialize_economy(ScenarioDefinition& scenario);
    bool update_economy(std::uint64_t tick);

    WorldDefinition world_;
    SimulationClock clock_;
    std::vector<SectorState> sector_states_;
    std::mt19937 random_;
    std::uint32_t economy_interval_ticks_;
    std::uint32_t minimum_integrity_;
    std::map<StockpileId, Stockpile> stockpiles_;
    std::vector<InstallationState> installations_;
    std::vector<EconomyEntry> economy_ledger_;
};

ScenarioDefinition example_scenario();

}  // namespace openwar

#endif
