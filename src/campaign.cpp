#include "campaign.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace openwar {

std::string_view faction_name(FactionId faction)
{
    switch (faction) {
    case FactionId::neutral: return "NEUTRAL";
    case FactionId::west: return "WEST";
    case FactionId::east: return "EAST";
    }
    throw std::invalid_argument("Unknown faction");
}

std::string_view terrain_name(Terrain terrain)
{
    switch (terrain) {
    case Terrain::plains: return "plains";
    case Terrain::forest: return "forest";
    case Terrain::water: return "water";
    }
    throw std::invalid_argument("Unknown terrain");
}

WorldDefinition::WorldDefinition(std::uint32_t width, std::uint32_t height,
                                 std::uint32_t sector_size_m,
                                 std::vector<SectorProperties> properties)
    : width_(width), height_(height), sector_size_m_(sector_size_m)
{
    const auto count = std::uint64_t{width} * height;
    if (width == 0 || height == 0 || sector_size_m == 0 ||
        count - 1 > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Invalid world dimensions or sector size");
    }
    if (properties.size() != count) {
        throw std::invalid_argument("Expected one property entry per sector");
    }

    sectors_.reserve(properties.size());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto id = sector_id(x, y);
            const auto& tile = properties[static_cast<std::uint32_t>(id)];
            terrain_name(tile.terrain);
            Sector sector{id, x, y, tile, {}};
            // The geometric graph is independent of terrain passability.
            if (y > 0) {
                sector.neighbors.push_back(sector_id(x, y - 1));
            }
            if (y + 1 < height) {
                sector.neighbors.push_back(sector_id(x, y + 1));
            }
            if (x + 1 < width) {
                sector.neighbors.push_back(sector_id(x + 1, y));
            }
            if (x > 0) {
                sector.neighbors.push_back(sector_id(x - 1, y));
            }
            sectors_.push_back(std::move(sector));
        }
    }
}

const Sector& WorldDefinition::sector(SectorId id) const
{
    return sectors_.at(static_cast<std::uint32_t>(id));
}

SectorId WorldDefinition::sector_id(std::uint32_t x, std::uint32_t y) const
{
    if (x >= width_ || y >= height_) {
        throw std::out_of_range("Sector coordinates are outside the world");
    }
    return SectorId{y * width_ + x};
}

SimulationClock::SimulationClock(std::uint32_t tick_seconds) : tick_seconds_(tick_seconds)
{
    if (tick_seconds == 0) {
        throw std::invalid_argument("Tick duration must be positive");
    }
}

void SimulationClock::advance(std::uint64_t ticks)
{
    const auto max_tick = std::numeric_limits<std::uint64_t>::max() / tick_seconds_;
    if (ticks > max_tick - tick_) {
        throw std::overflow_error("Simulated time would overflow");
    }
    tick_ += ticks;
}

CampaignState::CampaignState(ScenarioDefinition scenario, std::uint32_t seed)
    : world_(std::move(scenario.world)), clock_(scenario.tick_seconds),
      sector_states_(world_.sectors().size()), random_(seed),
      economy_interval_ticks_(scenario.economy_interval_ticks),
      minimum_integrity_(scenario.minimum_integrity), loading_ticks_(scenario.loading_ticks),
      unloading_ticks_(scenario.unloading_ticks)
{
    std::vector<bool> assigned(sector_states_.size());
    for (const auto& entry : scenario.initial_sectors) {
        faction_name(entry.state.controller);
        const auto index = static_cast<std::uint32_t>(entry.sector);
        if (assigned.at(index)) {
            throw std::invalid_argument("Duplicate initial state for sector");
        }
        assigned[index] = true;
        sector_states_[index] = entry.state;
    }
    initialize_economy(scenario);
    initialize_logistics(scenario);
    initialize_forces(scenario);
}

const SectorState& CampaignState::sector_state(SectorId sector) const
{
    return sector_states_.at(static_cast<std::uint32_t>(sector));
}

void CampaignState::advance_ticks(std::uint64_t ticks)
{
    auto target = clock_;
    target.advance(ticks);

    bool economy_awake = !installations_.empty();
    bool logistics_awake = !shipments_.empty();
    bool forces_awake = !equipment_.empty();
    while (clock_.tick() < target.tick()) {
        const auto boundary_step = economy_interval_ticks_ - clock_.tick() % economy_interval_ticks_;
        auto next = target.tick();
        bool activity_due = logistics_awake || economy_awake || forces_awake;
        if (logistics_awake || forces_awake) {
            next = clock_.tick() + 1;
        } else if (economy_awake && boundary_step <= next - clock_.tick()) {
            next = clock_.tick() + boundary_step;
        }
        if (!forces_awake) {
            for (const auto& [id, item] : equipment_) {
                if (item.reinforcement_eta && *item.reinforcement_eta > clock_.tick() &&
                    *item.reinforcement_eta <= next) {
                    next = *item.reinforcement_eta;
                    activity_due = true;
                }
            }
        }
        if (!activity_due) {
            break;
        }
        const auto step = next - clock_.tick();
        // Dispatch can use newly produced supplies starting on the following tick.
        logistics_awake = logistics_awake && update_logistics(next);
        economy_awake = economy_awake || (logistics_awake && !installations_.empty());
        bool produced_or_worked = false;
        if (economy_awake && step == boundary_step) {
            produced_or_worked = update_economy(next);
            economy_awake = produced_or_worked;
            logistics_awake = logistics_awake || (economy_awake && !shipments_.empty());
        }
        if (!equipment_.empty() && (forces_awake || logistics_awake || produced_or_worked ||
            std::ranges::any_of(equipment_, [next](const auto& entry) {
                return entry.second.reinforcement_eta == next;
            }))) {
            forces_awake = update_forces(next);
            economy_awake = economy_awake || (forces_awake && !installations_.empty());
        }
        clock_.advance(step);
    }
    clock_ = target;
}

std::uint32_t CampaignState::next_random()
{
    return static_cast<std::uint32_t>(random_());
}

}  // namespace openwar
