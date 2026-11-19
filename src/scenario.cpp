#include "scenario.hpp"

#include <algorithm>
#include <queue>
#include <random>
#include <stdexcept>
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
                              EquipmentCategory category, UnitId unit, std::uint32_t speed) {
        const EquipmentTypeId type{number};
        scenario.equipment_types.push_back({
            type, name, faction, category, preparation, {60}, scheduled, repair, 3600, 0, 0, false, speed
        });
        scenario.equipment.push_back({EquipmentId{number}, type, unit});
    };
    using enum EquipmentCategory;
    add_type(1, "AH-64A Apache", FactionId::west, helicopter, west_air, 0);
    add_type(2, "M1A1 Abrams", FactionId::west, tank, west_command, 30);
    add_type(3, "M977 HEMTT", FactionId::west, transport, west_command, 45);
    add_type(4, "M163 Vulcan", FactionId::west, air_defense_vehicle, west_command, 30);
    add_type(5, "Stinger team", FactionId::west, air_defense_team, west_command, 5);
    add_type(6, "Mi-24V/P Hind", FactionId::east, helicopter, east_air, 0);
    add_type(7, "T-72B", FactionId::east, tank, east_command, 30);
    add_type(8, "Ural-4320", FactionId::east, transport, east_command, 45);
    add_type(9, "ZSU-23-4 Shilka", FactionId::east, air_defense_vehicle, east_command, 30);
    add_type(10, "Igla team", FactionId::east, air_defense_team, east_command, 5);
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
    scenario.world = WorldDefinition(3, 2, 500, std::vector<SectorProperties>(6));
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
         {}, {}, {}, repair, 3600, 0, 110, true, 30},
        {EquipmentTypeId{2}, "Light game vehicle", FactionId::east, EquipmentCategory::tank,
         {}, {}, {}, repair, 3600, 0, 14, true, 30}
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

namespace {

ScenarioDefinition build_theater(WorldDefinition world, std::span<const std::uint32_t> rows,
                                 std::array<std::uint32_t, 2> home_rows, bool strategic_command,
                                 const ProceduralSettings* settings = nullptr)
{
    ScenarioDefinition scenario{std::move(world), {}};
    const auto width = scenario.world.width();
    const auto river_first = (width - 1) / 2;
    const auto river_last = width / 2;
    const auto front = river_first - 1;
    const auto east_front = river_last + 1;
    const auto supply_line = front - 1;
    const bool dispersed_airfields = settings && scenario.world.height() > 25;
    const auto relay_lane = rows.size() / 2;
    const auto relay_row = rows[relay_lane];
    std::vector<SectorId> crossings;
    for (const auto row : rows) {
        for (auto x = river_first; x <= river_last; ++x) {
            crossings.push_back(scenario.world.sector_id(x, row));
        }
    }
    scenario.operations = {true, 10};
    scenario.operations.max_autonomous_missions = std::max<std::size_t>(4, rows.size());
    scenario.operations.strategy_review_ticks = strategic_command ? 60 : 0;
    scenario.combat.enabled = true;
    scenario.combat.interval_ticks = 5;
    scenario.combat.time_limit_seconds = settings ? std::uint64_t{settings->time_limit_minutes} * 60 : 48 * 3600;
    scenario.public_sectors = crossings;
    for (const auto& sector : scenario.world.sectors()) {
        const auto owner = settings && !sector.properties.traversable ? FactionId::neutral :
            sector.x < river_first ? FactionId::west :
            sector.x > river_last ? FactionId::east : FactionId::neutral;
        const auto points = (sector.x == 0 && sector.y == home_rows[0]) ||
                            (sector.x == width - 1 && sector.y == home_rows[1]) ||
                            (sector.x >= river_first && sector.x <= river_last && sector.y == relay_row) ? 2u : 0u;
        scenario.initial_sectors.push_back({sector.id, {owner, points}});
    }

    const auto road_ticks = (scenario.world.sector_size_m() + 499) / 500;
    const auto road_fuel = (scenario.world.sector_size_m() + 1999) / 2000;
    const auto road = [&](SectorId from, SectorId to) {
        const auto id = static_cast<std::uint32_t>(scenario.routes.size()) + 1;
        scenario.routes.push_back({RouteId{id}, from, to, road_ticks, 2, road_fuel});
        scenario.routes.push_back({RouteId{id + 1}, to, from, road_ticks, 2, road_fuel});
    };
    auto& orders = scenario.initial_supply_requests;
    const std::uint32_t east_offset = settings ? 1000 : 100;
    for (const auto faction : {FactionId::west, FactionId::east}) {
        const bool west = faction == FactionId::west;
        const auto base = west ? 0u : east_offset;
        const auto home_row = home_rows[west ? 0 : 1];
        const auto at = [&](std::uint32_t x, std::uint32_t y) {
            return scenario.world.sector_id(west ? x : width - 1 - x, y);
        };
        const auto industry = at(0, home_row);
        const StockpileId industry_stock{base + 1};
        const InstallationId relay{base + 4};
        const SectorId enemy_relay = scenario.world.sector_id(west ? east_front : front, relay_row);
        const InstallationId enemy_relay_id{(west ? east_offset : 0u) + 4};
        const auto oil = settings ? settings->oil : 600;
        const auto materials = settings ? settings->materials : 240;
        const auto ammunition = settings ? settings->ammunition : 90;
        const ResourceAmounts industrial_capacity{oil, 100, 250, materials, 80, 100, std::max(120u, ammunition)};
        scenario.stockpiles.push_back({
            industry_stock, industry, faction, industrial_capacity, {oil, 0, 0, materials, 0, 0, ammunition}
        });
        scenario.installations.insert(scenario.installations.end(), {
            {InstallationId{base + 1}, industry_stock, "Oil field", faction, {{20}, {0, 20}, 2}},
            {InstallationId{base + 2}, industry_stock, "Refinery", faction, {{0, 20}, {0, 0, 16}, 3}},
            {InstallationId{base + 3}, industry_stock, "Supply works", faction,
             {{0, 0, 0, 8}, {0, 0, 0, 0, 3, 10}, 3}},
        });
        for (std::uint32_t x = 0; x < front; ++x) {
            road(at(x, home_row), at(x + 1, home_row));
        }
        for (std::uint32_t y = 0; y + 1 < scenario.world.height(); ++y) {
            road(at(supply_line, y), at(supply_line, y + 1));
        }

        const auto shipment = [&](StockpileId target, Resource resource, std::uint64_t amount) {
            const auto id = static_cast<std::uint32_t>(orders.size()) + 1;
            orders.push_back({ShipmentId{id}, industry_stock, target, resource, amount});
            const auto tank = settings ? 2 * (front + scenario.world.height()) * road_fuel : 16;
            scenario.carriers.push_back({CarrierId{id}, faction, industry, 80, tank});
        };
        const auto stock_airfield = [&](StockpileId stock, FacilityId facility, SectorId sector) {
            scenario.stockpiles.push_back({stock, sector, faction, {0, 0, 180, 0, 24}, {0, 0, 40, 0, 2}});
            scenario.facilities.push_back({facility, stock, 2, 2});
            shipment(stock, Resource::fuel, 80);
            shipment(stock, Resource::spare_parts, 18);
            scenario.resupply.push_back({industry_stock, stock, Resource::fuel, 30, 80, 3});
            scenario.resupply.push_back({industry_stock, stock, Resource::spare_parts, 4, 12, 2});
        };
        if (!dispersed_airfields) {
            stock_airfield(StockpileId{base + 2}, FacilityId{base + 1}, at(supply_line, home_row));
        }

        const EquipmentTypeId armor{base + 1};
        const EquipmentTypeId scout{base + 2};
        ServiceProfile repair{300};
        repair.costs[static_cast<std::size_t>(Resource::spare_parts)] = 2;
        scenario.equipment_types.push_back({
            armor, "Armored vehicle", faction, EquipmentCategory::tank, {}, {}, {300}, repair, 180 * 60, 0, 10, true, 30
        });
        scenario.equipment_types.push_back({
            scout, "Reconnaissance helicopter", faction, EquipmentCategory::helicopter,
            {60}, {120}, {300, repair.costs}, repair, 60 * 60,
            settings ? std::min(39u, scenario.world.height() + 3) : 6
        });

        for (std::uint32_t lane = 0; lane < rows.size(); ++lane) {
            const auto y = rows[lane];
            const auto forward = at(front, y);
            const StockpileId shop_stock{base + 10 + lane};
            const FacilityId shop{base + 10 + lane};
            const UnitId formation{base + 10 + lane};
            const StockpileId stores{base + (rows.size() > 5 ? 100u : 20u) + lane};
            if (y != home_row) {
                road(at(supply_line, y), forward);
            }
            scenario.stockpiles.push_back({shop_stock, forward, faction,
                {0, 0, 0, 8, 16}, {0, 0, 0, 8, 4}});
            scenario.facilities.push_back({shop, shop_stock, 2, 2});
            scenario.stockpiles.push_back({stores, forward, faction,
                {0, 0, 48, 0, 0, 48, 48}, {0, 0, 16, 0, 0, 16, 16}});
            constexpr std::array names{"Northern armor", "Central armor", "Southern armor"};
            scenario.units.push_back({formation, settings ? "Armor formation " + std::to_string(lane + 1) : names[lane],
                                      faction, forward, {}, shop});
            scenario.combat_units.push_back({formation, stores});
            // The authored demo concentrates its initial armor on opposite flanks.
            const auto authored_armor = lane == 1 ? 3u : (lane == (west ? 0u : 2u) ? 4u : 2u);
            const auto vehicles = settings ? (west ? settings->west_armor : settings->east_armor) : authored_armor;
            const auto reserves = settings ? settings->reserve_armor : 1;
            for (std::uint32_t item = 0; item < vehicles + reserves; ++item) {
                const auto id = settings ? base + 100 + lane * 16 + item : base + 10 + lane * 5 + item;
                scenario.equipment.push_back({EquipmentId{id}, armor, formation,
                    item >= vehicles ? "Home reserve" : ""});
            }
            shipment(shop_stock, Resource::spare_parts, 5);
            shipment(stores, Resource::supplies, 8);
            shipment(stores, Resource::ammunition, 8);
            shipment(stores, Resource::fuel, 18);
            scenario.resupply.insert(scenario.resupply.end(), {
                {industry_stock, shop_stock, Resource::spare_parts, 2, 8, 2},
                {industry_stock, stores, Resource::fuel, 10, 28, 3},
                {industry_stock, stores, Resource::supplies, 8, 20, 3},
                {industry_stock, stores, Resource::ammunition, 8, 20, 3}
            });
            scenario.objectives.push_back({faction, at(river_first, y), y == relay_row ? 5u : 4u});
            if (river_first != river_last) {
                scenario.objectives.push_back({faction, at(river_last, y), 3});
            }
            scenario.objectives.push_back({faction, forward, 2});
        }
        scenario.installations.push_back({
            relay, StockpileId{base + 10 + static_cast<std::uint32_t>(relay_lane)}, "Central relay workshop", faction,
            {{0, 0, 0, 2}, {0, 0, 0, 0, 1}}
        });
        const auto flights = settings ? settings->scout_flights : 2;
        for (std::uint32_t wing = 0; wing < flights; ++wing) {
            const UnitId unit{base + 30 + wing};
            const auto row = dispersed_airfields ? (2 * wing + 1) * scenario.world.height() / (2 * flights) : home_row;
            const auto airfield = at(supply_line, row);
            const FacilityId facility{base + (dispersed_airfields ? 200 + wing : 1)};
            if (dispersed_airfields) {
                stock_airfield(StockpileId{base + 200 + wing}, facility, airfield);
            }
            scenario.units.push_back({unit, "Reconnaissance flight", faction, airfield, {}, facility});
            scenario.equipment.push_back({EquipmentId{base + 30 + wing}, scout, unit});
        }
        scenario.objectives.push_back({faction, enemy_relay, 3});
        if (!settings || settings->capitulation) {
            scenario.command_objectives.push_back({faction, {industry, at(front, relay_row)}});
        }
        VictoryObjective victory{faction, crossings, 60 * std::uint64_t{settings ? settings->hold_minutes : 120}};
        if (!settings || settings->require_relay) {
            victory.sectors.push_back(enemy_relay);
            victory.required_installations.push_back(enemy_relay_id);
        }
        scenario.victory_objectives.push_back(std::move(victory));
    }
    for (const auto y : rows) {
        for (auto x = front; x < east_front; ++x) {
            road(scenario.world.sector_id(x, y), scenario.world.sector_id(x + 1, y));
        }
    }
    return scenario;
}

}  // namespace

CampaignState long_example_campaign(std::uint32_t seed, bool strategic_command)
{
    const SectorProperties plains{};
    const SectorProperties forest{Terrain::forest, true};
    const SectorProperties water{Terrain::water, false};
    WorldDefinition world(9, 7, 2000, {
        plains, plains, forest, plains, plains, plains, forest, plains, plains,
        forest, plains, plains, forest, water,  forest, plains, plains, forest,
        plains, forest, plains, plains, water,  plains, plains, forest, plains,
        plains, plains, plains, plains, plains, plains, plains, plains, plains,
        plains, forest, plains, plains, water,  plains, plains, forest, plains,
        forest, plains, plains, forest, water,  forest, plains, plains, forest,
        plains, plains, forest, plains, plains, plains, forest, plains, plains,
    });
    constexpr std::array rows{0u, 3u, 6u};
    return CampaignState(build_theater(std::move(world), rows, {3, 3}, strategic_command), seed);
}

namespace {

void validate(const ProceduralSettings& settings)
{
    if (settings.width < 9 || settings.width > 128 ||
        settings.height < 5 || settings.height > 128) {
        throw std::invalid_argument("Procedural dimensions must be in 9..128 by 5..128");
    }
    if (settings.sector_size_m < 500 || settings.sector_size_m > 5000 ||
        settings.crossings < 1 || settings.crossings > std::min(16u, settings.height) ||
        settings.forest_percent > 100 || settings.water_percent > 100 ||
        settings.forest_percent + settings.water_percent > 100) {
        throw std::invalid_argument("Invalid sector size, crossing count or terrain percentages");
    }
    if (settings.west_armor < 1 || settings.west_armor > 8 ||
        settings.east_armor < 1 || settings.east_armor > 8 ||
        settings.reserve_armor > 4 || settings.scout_flights < 1 || settings.scout_flights > 4) {
        throw std::invalid_argument("Invalid armor, reserve or reconnaissance flight count");
    }
    if (settings.oil < 20 || settings.oil > 100000 ||
        settings.materials < 8 || settings.materials > 100000 ||
        settings.ammunition < 8 * settings.crossings || settings.ammunition > 100000) {
        throw std::invalid_argument("Invalid industrial starting stocks for the configured crossings");
    }
    // A clear advance still needs the first planning interval, one crossing leg and occupation.
    const auto minimum_minutes = std::uint64_t{settings.hold_minutes} + 120 + 10 +
                                 (settings.sector_size_m + 499) / 500;
    if (settings.hold_minutes == 0 || settings.time_limit_minutes < minimum_minutes ||
        settings.time_limit_minutes > 7 * 24 * 60 ||
        settings.require_relay > 1 || settings.capitulation > 1) {
        throw std::invalid_argument("Invalid victory settings or campaign time limit");
    }
}

std::mt19937 generator(std::uint64_t seed, std::uint32_t stream)
{
    std::seed_seq source{static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32),
                         procedural_generator_version, stream};
    return std::mt19937(source);
}

// Control the mapping as well as the engine so standard-library distributions cannot change the layout.
std::uint32_t sample(std::mt19937& random, std::uint32_t bound)
{
    constexpr auto range = std::uint64_t{1} << 32;
    const auto limit = range - range % bound;
    auto value = random();
    while (value >= limit) {
        value = random();
    }
    return static_cast<std::uint32_t>(value % bound);
}

}  // namespace

ScenarioDefinition resolve_procedural_scenario(const ProceduralSettings& settings)
{
    validate(settings);
    const auto width = settings.width;
    const auto height = settings.height;
    const auto river_first = (width - 1) / 2;
    const auto river_last = width / 2;
    auto terrain_random = generator(settings.seed, 0);
    auto site_random = generator(settings.seed, 1);
    std::vector<std::uint32_t> crossings;
    for (std::uint32_t i = 0; i < settings.crossings; ++i) {
        const auto first = i * height / settings.crossings;
        const auto end = (i + 1) * height / settings.crossings;
        crossings.push_back(first + sample(site_random, end - first));
    }
    const std::array home_rows{sample(site_random, height), sample(site_random, height)};

    const SectorProperties water{Terrain::water, false};
    const SectorProperties forest{Terrain::forest, true};
    std::vector<SectorProperties> terrain(width * height);
    for (auto& sector : terrain) {
        const auto roll = sample(terrain_random, 100);
        if (roll < settings.water_percent) {
            sector = water;
        } else if (roll < settings.water_percent + settings.forest_percent) {
            sector = forest;
        }
    }
    // The river and protected road network also define the accessible ground corridors.
    for (std::uint32_t y = 0; y < height; ++y) {
        for (auto x = river_first; x <= river_last; ++x) {
            terrain[y * width + x] = water;
        }
        terrain[y * width + river_first - 2] = {};
        terrain[y * width + river_last + 2] = {};
    }
    for (auto y : crossings) {
        for (auto x = river_first - 1; x <= river_last + 1; ++x) {
            terrain[y * width + x] = {};
        }
    }
    for (auto x = 0u; x < river_first; ++x) {
        terrain[home_rows[0] * width + x] = {};
        terrain[home_rows[1] * width + (width - 1 - x)] = {};
    }

    const WorldDefinition candidate(width, height, settings.sector_size_m, terrain);
    std::vector<bool> connected(terrain.size());
    std::queue<SectorId> pending;
    const auto start = candidate.sector_id(0, home_rows[0]);
    connected[static_cast<std::uint32_t>(start)] = true;
    pending.push(start);
    while (!pending.empty()) {
        const auto sector = pending.front();
        pending.pop();
        for (auto next : candidate.sector(sector).neighbors) {
            const auto index = static_cast<std::uint32_t>(next);
            if (terrain[index].traversable && !connected[index]) {
                connected[index] = true;
                pending.push(next);
            }
        }
    }
    for (std::size_t i = 0; i < terrain.size(); ++i) {
        if (!connected[i]) {
            terrain[i] = water;
        }
    }
    return build_theater(WorldDefinition(width, height, settings.sector_size_m, std::move(terrain)),
                         crossings, home_rows, true, &settings);
}

CampaignState procedural_campaign(const ProceduralSettings& settings)
{
    auto simulation_seed = generator(settings.seed, 2);
    return CampaignState(resolve_procedural_scenario(settings), static_cast<std::uint32_t>(simulation_seed()),
                         settings.seed, procedural_generator_version);
}

CampaignState open_campaign_scenario(const std::filesystem::path& path, std::optional<FactionId> player,
                                     std::optional<std::uint64_t> seed)
{
    if (is_geographic_scenario_file(path)) {
        const auto value = seed.value_or(0);
        if (value > UINT32_MAX)
            throw std::out_of_range("Seed must fit in 32 bits");
        auto scenario = read_geographic_scenario(path, player);
        if (path.extension() == ".owscenario" && value != 0) {
            CampaignOptions options;
            options.seed = static_cast<std::uint32_t>(value);
            scenario = customize_scenario(std::move(scenario), path, options);
        }
        return CampaignState(std::move(scenario), static_cast<std::uint32_t>(value));
    }
    auto settings = read_procedural_settings(path);
    settings.seed = seed.value_or(settings.seed);
    return procedural_campaign(settings);
}

}  // namespace openwar
