#include "campaign.hpp"

#include <utility>

namespace openwar {

ScenarioDefinition example_scenario()
{
    const SectorProperties plains{};
    const SectorProperties forest{Terrain::forest, true};
    const SectorProperties water{Terrain::water, false};

    WorldDefinition world(3, 2, 2000, {
        plains, forest, plains,
        plains, water,  plains,
    });
    std::vector<InitialSectorState> initial_sectors{
        {world.sector_id(0, 0), {FactionId::west, 1}},
        {world.sector_id(0, 1), {FactionId::west}},
        {world.sector_id(2, 0), {FactionId::east, 1}},
        {world.sector_id(2, 1), {FactionId::east}},
    };
    return {std::move(world), std::move(initial_sectors)};
}

}  // namespace openwar
