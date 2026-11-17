#ifndef OPENWAR_CAMPAIGN_HPP
#define OPENWAR_CAMPAIGN_HPP

#include <cstdint>
#include <random>
#include <span>
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

struct ScenarioDefinition {
    WorldDefinition world;
    // Unlisted sectors start neutral.
    std::vector<InitialSectorState> initial_sectors;
    std::uint32_t tick_seconds = 60;
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

private:
    WorldDefinition world_;
    SimulationClock clock_;
    std::vector<SectorState> sector_states_;
    std::mt19937 random_;
};

ScenarioDefinition example_scenario();

}  // namespace openwar

#endif
