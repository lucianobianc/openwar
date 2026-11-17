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

namespace {

ScenarioDefinition force_example_scenario()
{
    auto scenario = example_scenario();
    constexpr StockpileId west_stock{1};
    constexpr StockpileId east_stock{2};
    constexpr StockpileId parts_depot{3};
    constexpr FacilityId west_shop{1};
    constexpr FacilityId east_shop{2};
    constexpr UnitId west_command{1};
    constexpr UnitId east_command{2};
    constexpr UnitId west_air{3};
    constexpr UnitId east_air{4};
    const auto west = scenario.world.sector_id(0, 0);
    const auto east = scenario.world.sector_id(2, 0);
    const auto depot = scenario.world.sector_id(0, 1);
    const auto part = static_cast<std::size_t>(Resource::spare_parts);

    scenario.stockpiles.front().capacity[part] = 10;
    Stockpile spare_stock{parts_depot, depot, FactionId::west};
    spare_stock.capacity[part] = spare_stock.quantity[part] = 3;
    scenario.stockpiles.push_back(spare_stock);
    scenario.routes.push_back({RouteId{1}, depot, west, 3});
    scenario.carriers.push_back({CarrierId{1}, FactionId::west, depot, 10, 10});

    scenario.facilities = {{west_shop, west_stock, 1, 2}, {east_shop, east_stock, 1, 2}};
    scenario.units = {
        {west_command, "West formation", FactionId::west, west, {}, west_shop},
        {east_command, "East formation", FactionId::east, east, {}, east_shop},
        {west_air, "West air squadron", FactionId::west, west, west_command, west_shop},
        {east_air, "East air squadron", FactionId::east, east, east_command, east_shop}
    };

    ServiceProfile preparation{60};
    preparation.costs[static_cast<std::size_t>(Resource::fuel)] = 5;
    ServiceProfile scheduled{120};
    scheduled.costs[part] = 1;
    ServiceProfile repair{180};
    repair.costs[part] = 2;
    const auto add_type = [&](std::uint32_t number, const char* name, FactionId faction,
                              EquipmentCategory category, UnitId unit) {
        const EquipmentTypeId type{number};
        scenario.equipment_types.push_back({
            type, name, faction, category, preparation, {60}, scheduled, repair, 3600
        });
        scenario.equipment.push_back({EquipmentId{number}, type, unit});
    };
    using enum EquipmentCategory;
    add_type(1, "AH-64A Apache", FactionId::west, helicopter, west_air);
    add_type(2, "M1A1 Abrams", FactionId::west, tank, west_command);
    add_type(3, "M977 HEMTT", FactionId::west, transport, west_command);
    add_type(4, "M163 Vulcan", FactionId::west, air_defense_vehicle, west_command);
    add_type(5, "Stinger team", FactionId::west, air_defense_team, west_command);
    add_type(6, "Mi-24V/P Hind", FactionId::east, helicopter, east_air);
    add_type(7, "T-72B", FactionId::east, tank, east_command);
    add_type(8, "Ural-4320", FactionId::east, transport, east_command);
    add_type(9, "ZSU-23-4 Shilka", FactionId::east, air_defense_vehicle, east_command);
    add_type(10, "Igla team", FactionId::east, air_defense_team, east_command);
    scenario.equipment.front().operating_seconds = 3600;
    scenario.equipment.push_back({EquipmentId{11}, EquipmentTypeId{6}, east_air, "External air reserve"});

    return scenario;
}

}  // namespace

CampaignState maintenance_example_campaign(std::uint32_t seed)
{
    CampaignState campaign(force_example_scenario(), seed);
    campaign.damage_equipment(EquipmentId{1}, 20);
    campaign.request_supply({ShipmentId{1}, StockpileId{3}, StockpileId{1}, Resource::spare_parts, 3});
    campaign.authorize_reinforcement(EquipmentId{11}, 8);
    return campaign;
}

ScenarioDefinition combat_example_scenario(std::optional<FactionId> player)
{
    auto scenario = example_scenario();
    scenario.world = WorldDefinition(3, 2, 2000, std::vector<SectorProperties>(6));
    scenario.initial_sectors.push_back({SectorId{1}, {FactionId::neutral, 1}});
    scenario.operations = {true, 4, 12 * 60, 4 * 60, 24 * 60, player};
    scenario.combat = {true, 1, 2 * 60, 3 * 60, 2 * 60, 6 * 3600, 24 * 3600, 90 * 60};
    scenario.public_sectors = {SectorId{1}};
    scenario.objectives = {{FactionId::west, SectorId{1}, 3}, {FactionId::east, SectorId{1}, 3}};
    scenario.victory_objectives = {{FactionId::west, {SectorId{1}}, 3 * 60},
                                   {FactionId::east, {SectorId{1}}, 3 * 60}};
    scenario.command_objectives = {{FactionId::west, {SectorId{0}}}, {FactionId::east, {SectorId{2}}}};
    const auto stock = [&](StockpileId id, SectorId sector, FactionId owner) {
        const ResourceAmounts load{0, 0, 20, 0, 0, 20, 20};
        scenario.stockpiles.push_back({id, sector, owner, load, load});
    };
    stock(StockpileId{10}, SectorId{0}, FactionId::west);
    stock(StockpileId{11}, SectorId{2}, FactionId::east);
    scenario.stockpiles.push_back({
        StockpileId{12}, SectorId{1}, FactionId::east, {0, 0, 0, 10, 10}, {0, 0, 0, 0, 7}
    });
    scenario.installations.push_back({
        InstallationId{4}, StockpileId{12}, "Central workshop", FactionId::east,
        {{0, 0, 0, 1}, {0, 0, 0, 0, 1}}, 1, false, 40
    });
    scenario.units = {{UnitId{1}, "West field group", FactionId::west, SectorId{0}, {}, FacilityId{1}},
                      {UnitId{2}, "East field group", FactionId::east, SectorId{2}, {}, FacilityId{2}}};
    scenario.facilities = {{FacilityId{1}, StockpileId{1}, 1, 1}, {FacilityId{2}, StockpileId{2}, 1, 1}};
    ServiceProfile repair{180, {0, 0, 0, 0, 1}};
    scenario.equipment_types = {
        {EquipmentTypeId{1}, "Heavy game vehicle", FactionId::west, EquipmentCategory::tank,
         {}, {}, {}, repair, 3600, 0, 110, true},
        {EquipmentTypeId{2}, "Light game vehicle", FactionId::east, EquipmentCategory::tank,
         {}, {}, {}, repair, 3600, 0, 14, true}
    };
    scenario.equipment = {{EquipmentId{1}, EquipmentTypeId{1}, UnitId{1}},
                          {EquipmentId{2}, EquipmentTypeId{2}, UnitId{2}},
                          {EquipmentId{3}, EquipmentTypeId{2}, UnitId{2}}};
    scenario.combat_units = {{UnitId{1}, StockpileId{10}}, {UnitId{2}, StockpileId{11}, 40}};
    return scenario;
}

CampaignState operations_example_campaign(std::uint32_t seed, std::optional<FactionId> player)
{
    auto scenario = force_example_scenario();
    scenario.operations = {true, 4, 12 * 60, 4 * 60, 24 * 60, player};
    scenario.objectives = {
        {FactionId::west, SectorId{2}, 3},
        {FactionId::west, SectorId{3}, 2},
        {FactionId::east, SectorId{0}, 3},
        {FactionId::east, SectorId{5}, 1}
    };
    const auto fuel = static_cast<std::size_t>(Resource::fuel);
    scenario.stockpiles[1].capacity[fuel] = scenario.stockpiles[1].quantity[fuel] = 100;
    scenario.stockpiles[2].capacity[fuel] = scenario.stockpiles[2].quantity[fuel] = 40;
    scenario.facilities.push_back({FacilityId{3}, StockpileId{3}, 1, 1});
    scenario.equipment.front().operating_seconds = 0;
    for (auto& type : scenario.equipment_types) {
        if (type.category == EquipmentCategory::helicopter) {
            type.operation_range = 2;
        }
    }
    return CampaignState(std::move(scenario), seed);
}

}  // namespace openwar
