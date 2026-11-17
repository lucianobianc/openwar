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

CampaignState long_example_campaign(std::uint32_t seed)
{
    const SectorProperties plains{};
    const SectorProperties forest{Terrain::forest, true};
    const SectorProperties water{Terrain::water, false};
    ScenarioDefinition scenario{WorldDefinition(9, 7, 2000, {
        plains, plains, forest, plains, plains, plains, forest, plains, plains,
        forest, plains, plains, forest, water,  forest, plains, plains, forest,
        plains, forest, plains, plains, water,  plains, plains, forest, plains,
        plains, plains, plains, plains, plains, plains, plains, plains, plains,
        plains, forest, plains, plains, water,  plains, plains, forest, plains,
        forest, plains, plains, forest, water,  forest, plains, plains, forest,
        plains, plains, forest, plains, plains, plains, forest, plains, plains,
    }), {}};
    scenario.operations = {true, 10};
    scenario.operations.max_autonomous_missions = 4;
    scenario.combat.enabled = true;
    scenario.combat.interval_ticks = 5;
    scenario.combat.time_limit_seconds = 48 * 3600;
    const std::array crossings{
        scenario.world.sector_id(4, 0), scenario.world.sector_id(4, 3), scenario.world.sector_id(4, 6)
    };
    scenario.public_sectors.assign(crossings.begin(), crossings.end());
    for (const auto& sector : scenario.world.sectors()) {
        const auto owner = sector.x < 4 ? FactionId::west :
                           sector.x > 4 ? FactionId::east : FactionId::neutral;
        const auto points = sector.y == 3 && (sector.x == 0 || sector.x == 4 || sector.x == 8) ? 2u : 0u;
        scenario.initial_sectors.push_back({sector.id, {owner, points}});
    }

    const auto road = [&](SectorId from, SectorId to) {
        const auto id = static_cast<std::uint32_t>(scenario.routes.size()) + 1;
        scenario.routes.push_back({RouteId{id}, from, to, 4, 2});
        scenario.routes.push_back({RouteId{id + 1}, to, from, 4, 2});
    };
    std::vector<SupplyRequest> orders;
    for (const auto faction : {FactionId::west, FactionId::east}) {
        const bool west = faction == FactionId::west;
        const std::uint32_t base = west ? 0 : 100;
        const auto at = [&](std::uint32_t x, std::uint32_t y) {
            return scenario.world.sector_id(west ? x : 8 - x, y);
        };
        const auto industry = at(0, 3);
        const StockpileId industry_stock{base + 1};
        const StockpileId air_stock{base + 2};
        const InstallationId relay{base + 4};
        const SectorId enemy_relay = scenario.world.sector_id(west ? 5 : 3, 3);
        const InstallationId enemy_relay_id{west ? 104u : 4u};
        const auto airfield = at(2, 3);
        constexpr ResourceAmounts industrial_capacity{600, 100, 250, 240, 80, 100, 120};
        scenario.stockpiles.push_back({
            industry_stock, industry, faction, industrial_capacity, {600, 0, 0, 240, 0, 0, 90}
        });
        scenario.installations.insert(scenario.installations.end(), {
            {InstallationId{base + 1}, industry_stock, "Oil field", faction, {{20}, {0, 20}, 2}},
            {InstallationId{base + 2}, industry_stock, "Refinery", faction, {{0, 20}, {0, 0, 16}, 3}},
            {InstallationId{base + 3}, industry_stock, "Supply works", faction,
             {{0, 0, 0, 8}, {0, 0, 0, 0, 3, 10}, 3}},
        });
        scenario.stockpiles.push_back({air_stock, airfield, faction, {0, 0, 180, 0, 24}, {0, 0, 40, 0, 2}});
        scenario.facilities.push_back({FacilityId{base + 1}, air_stock, 2, 2});

        for (std::uint32_t x = 0; x < 3; ++x) {
            road(at(x, 3), at(x + 1, 3));
        }
        for (std::uint32_t y = 0; y < 6; ++y) {
            road(at(2, y), at(2, y + 1));
        }

        const auto shipment = [&](StockpileId target, Resource resource, std::uint64_t amount) {
            const auto id = static_cast<std::uint32_t>(orders.size()) + 1;
            orders.push_back({ShipmentId{id}, industry_stock, target, resource, amount});
            scenario.carriers.push_back({CarrierId{id}, faction, industry, 80, 16});
        };
        shipment(air_stock, Resource::fuel, 80);
        shipment(air_stock, Resource::spare_parts, 18);

        const EquipmentTypeId armor{base + 1};
        const EquipmentTypeId scout{base + 2};
        ServiceProfile repair{300};
        repair.costs[static_cast<std::size_t>(Resource::spare_parts)] = 2;
        scenario.equipment_types.push_back({
            armor, "Armored vehicle", faction, EquipmentCategory::tank, {}, {}, {300}, repair, 180 * 60, 0, 10, true
        });
        scenario.equipment_types.push_back({
            scout, "Reconnaissance helicopter", faction, EquipmentCategory::helicopter,
            {60}, {120}, {300, repair.costs}, repair, 60 * 60, 6
        });

        for (std::uint32_t lane = 0; lane < 3; ++lane) {
            const auto y = 3 * lane;
            const auto forward = at(3, y);
            const StockpileId shop_stock{base + 10 + lane};
            const FacilityId shop{base + 10 + lane};
            const UnitId formation{base + 10 + lane};
            const StockpileId stores{base + 20 + lane};
            if (y != 3) {
                road(at(2, y), forward);
            }
            scenario.stockpiles.push_back({shop_stock, forward, faction,
                {0, 0, 0, 8, 16}, {0, 0, 0, 8, 4}});
            scenario.facilities.push_back({shop, shop_stock, 2, 2});
            scenario.stockpiles.push_back({stores, forward, faction,
                {0, 0, 48, 0, 0, 48, 48}, {0, 0, 16, 0, 0, 16, 16}});
            constexpr std::array names{"Northern armor", "Central armor", "Southern armor"};
            scenario.units.push_back({formation, names[lane], faction, forward, {}, shop});
            scenario.combat_units.push_back({formation, stores});
            // Both sides have equal inventories, concentrated on opposite flanks.
            const std::uint32_t vehicles = lane == 1 ? 3 : (lane == (west ? 0u : 2u) ? 4 : 2);
            for (std::uint32_t item = 0; item <= vehicles; ++item) {
                scenario.equipment.push_back({EquipmentId{base + 10 + lane * 5 + item}, armor, formation,
                    item == vehicles ? "Home reserve" : ""});
            }
            shipment(shop_stock, Resource::spare_parts, 5);
            shipment(stores, Resource::supplies, 8);
            shipment(stores, Resource::ammunition, 8);
            shipment(stores, Resource::fuel, 18);
            scenario.objectives.push_back({faction, crossings[lane], y == 3 ? 5u : 4u});
            scenario.objectives.push_back({faction, forward, 2});
        }
        scenario.installations.push_back({
            relay, StockpileId{base + 11}, "Central relay workshop", faction,
            {{0, 0, 0, 2}, {0, 0, 0, 0, 1}}
        });
        for (std::uint32_t wing = 0; wing < 2; ++wing) {
            const UnitId unit{base + 30 + wing};
            scenario.units.push_back({unit, "Reconnaissance flight", faction, airfield, {}, FacilityId{base + 1}});
            scenario.equipment.push_back({EquipmentId{base + 30 + wing}, scout, unit});
        }
        scenario.objectives.push_back({faction, enemy_relay, 3});
        scenario.command_objectives.push_back({faction, {industry, at(3, 3)}});
        scenario.victory_objectives.push_back({
            faction, {crossings[0], crossings[1], crossings[2], enemy_relay}, 120 * 60, {enemy_relay_id}
        });
    }
    for (const auto crossing : crossings) {
        const auto y = scenario.world.sector(crossing).y;
        road(scenario.world.sector_id(3, y), crossing);
        road(crossing, scenario.world.sector_id(5, y));
    }
    CampaignState campaign(std::move(scenario), seed);
    for (const auto& order : orders) {
        campaign.request_supply(order);
    }
    return campaign;
}

}  // namespace openwar
