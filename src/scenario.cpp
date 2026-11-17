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
    const auto west_site = world.sector_id(0, 0);
    const auto east_site = world.sector_id(2, 0);
    ScenarioDefinition scenario{std::move(world), std::move(initial_sectors)};
    constexpr StockpileId west_stock{1};
    constexpr StockpileId east_stock{2};
    // Resource order: oil reserve, crude oil, fuel, materials, parts, supplies.
    scenario.stockpiles = {
        {west_stock, west_site, FactionId::west, {3000, 1000, 2400}, {3000, 0, 100}},
        {east_stock, east_site, FactionId::east, {0, 0, 0, 60, 30, 30}, {0, 0, 0, 60}}
    };
    scenario.installations = {
        {InstallationId{1}, west_stock, "Oil field", FactionId::west,
         {{1000}, {0, 1000}, 1}},
        {InstallationId{2}, west_stock, "Refinery", FactionId::west,
         {{0, 1000}, {0, 0, 800}, 2}},
        {InstallationId{3}, east_stock, "Factory", FactionId::east,
         {{0, 0, 0, 20}, {0, 0, 0, 0, 5, 10}, 2}}
    };
    return scenario;
}

ScenarioDefinition logistics_example_scenario()
{
    auto scenario = example_scenario();
    const auto origin = scenario.world.sector_id(0, 0);
    const auto destination = scenario.world.sector_id(0, 1);
    scenario.stockpiles.push_back({StockpileId{3}, destination, FactionId::west, {0, 0, 120}});
    scenario.routes.push_back({RouteId{1}, origin, destination, 3, 1, 2});
    scenario.carriers.push_back({CarrierId{1}, FactionId::west, origin, 100, 10});
    scenario.carriers.push_back({CarrierId{2}, FactionId::west, origin, 100, 10});
    return scenario;
}

}  // namespace openwar
