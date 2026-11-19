#include "campaign.hpp"
#include "command.hpp"
#include "realtime.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace openwar;

void require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

template<class Exception, class Action>
void require_throws(Action action, std::string_view message)
{
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    require(false, message);
}

void test_clock()
{
    SimulationClock clock(60);
    require(clock.tick() == 0 && clock.elapsed_seconds() == 0, "Clock must start at zero");
    clock.advance(2);
    clock.advance(0);
    clock.advance(3);
    require(clock.tick() == 5 && clock.elapsed_seconds() == 300, "Wrong simulated time");
    require_throws<std::invalid_argument>([] { SimulationClock invalid(0); },
                                          "Zero tick duration accepted");

    SimulationClock limit(2);
    const auto max_tick = std::numeric_limits<std::uint64_t>::max() / 2;
    limit.advance(max_tick);
    require_throws<std::overflow_error>([&] { limit.advance(1); }, "Clock overflow accepted");
    require(limit.tick() == max_tick && limit.elapsed_seconds() == max_tick * 2,
            "Rejected advance changed the clock");
}

void test_topology()
{
    WorldDefinition world(3, 3, 1500, std::vector<SectorProperties>(9));
    const std::vector<std::vector<SectorId>> expected{
        {SectorId{3}, SectorId{1}},
        {SectorId{4}, SectorId{2}, SectorId{0}},
        {SectorId{5}, SectorId{1}},
        {SectorId{0}, SectorId{6}, SectorId{4}},
        {SectorId{1}, SectorId{7}, SectorId{5}, SectorId{3}},
        {SectorId{2}, SectorId{8}, SectorId{4}},
        {SectorId{3}, SectorId{7}},
        {SectorId{4}, SectorId{8}, SectorId{6}},
        {SectorId{5}, SectorId{7}},
    };
    require(world.sector_size_m() == 1500, "Incorrect sector size");
    for (std::uint32_t i = 0; i < expected.size(); ++i) {
        const auto id = SectorId{i};
        const auto& sector = world.sector(id);
        require(sector.id == id && sector.x == i % 3 && sector.y == i / 3 &&
                world.sector_id(sector.x, sector.y) == id, "Invalid row-major sector ID");
        require(sector.neighbors == expected[i], "Incorrect orthogonal adjacency");
        for (const auto neighbor : sector.neighbors) {
            const auto& reverse = world.sector(neighbor).neighbors;
            require(std::ranges::find(reverse, id) != reverse.end(), "One-way adjacency");
        }
    }

    WorldDefinition singleton(1, 1, 10, {SectorProperties{}});
    require(singleton.sector(SectorId{0}).neighbors.empty(), "Singleton has neighbors");
    require_throws<std::out_of_range>([&] { world.sector_id(3, 0); }, "Invalid coordinate accepted");
    require_throws<std::out_of_range>([&] { world.sector(SectorId{9}); }, "Invalid ID accepted");
    require_throws<std::invalid_argument>([] { WorldDefinition invalid(0, 1, 1, {}); },
                                          "Empty world accepted");
    require_throws<std::invalid_argument>([] { WorldDefinition invalid(1, 1, 0, {{}}); },
                                          "Zero sector size accepted");
    require_throws<std::invalid_argument>([] { WorldDefinition invalid(65536, 65537, 1, {}); },
                                          "Grid exceeds the sector ID range");
    require_throws<std::invalid_argument>([] { WorldDefinition invalid(2, 2, 1, {{}}); },
                                          "Mismatched properties accepted");
    require_throws<std::invalid_argument>(
        [] { WorldDefinition invalid(1, 1, 1, {{static_cast<Terrain>(99), true}}); },
        "Unknown terrain accepted");
}

void test_initialization()
{
    static_assert(static_cast<int>(FactionId::neutral) == 0);
    static_assert(static_cast<int>(FactionId::west) == 1);
    static_assert(static_cast<int>(FactionId::east) == 2);
    require(faction_name(FactionId::neutral) == "NEUTRAL" &&
            faction_name(FactionId::west) == "WEST" &&
            faction_name(FactionId::east) == "EAST", "Incorrect faction names");

    auto scenario = example_scenario();
    const CampaignState campaign(scenario, 0);
    const auto& world = campaign.world();
    require(world.width() == 3 && world.height() == 2 && world.sectors().size() == 6,
            "Incorrect example dimensions");
    require(campaign.sector_state(SectorId{0}).controller == FactionId::west &&
            campaign.sector_state(SectorId{2}).controller == FactionId::east &&
            campaign.sector_state(SectorId{4}).controller == FactionId::neutral,
            "Incorrect initial ownership");
    require(campaign.sector_state(SectorId{2}).objective_points == 1, "Incorrect objective points");
    const auto& water = world.sector(SectorId{4});
    require(water.properties.terrain == Terrain::water && !water.properties.traversable &&
            water.neighbors.size() == 3, "Passability changed geometric adjacency");
    require_throws<std::out_of_range>([&] { campaign.sector_state(SectorId{6}); },
                                       "Invalid campaign sector lookup accepted");

    scenario.initial_sectors.push_back({SectorId{0}, {FactionId::east}});
    require_throws<std::invalid_argument>([&] { CampaignState invalid(scenario, 0); },
                                          "Duplicate ownership accepted");
    scenario.initial_sectors.back() = {SectorId{6}, {FactionId::east}};
    require_throws<std::out_of_range>([&] { CampaignState invalid(scenario, 0); },
                                       "Invalid scenario sector accepted");
    scenario.initial_sectors.back() = {SectorId{1}, {static_cast<FactionId>(99)}};
    require_throws<std::invalid_argument>([&] { CampaignState invalid(scenario, 0); },
                                          "Invalid faction accepted");

    CampaignState custom({WorldDefinition(1, 1, 500, {{}}), {}, 7}, 1);
    custom.advance_ticks(2);
    require(custom.world().sector_size_m() == 500 &&
            custom.sector_state(SectorId{0}).controller == FactionId::neutral &&
            custom.clock().elapsed_seconds() == 14,
            "Campaign initialization depends on the example");
}

void test_reproducibility()
{
    CampaignState first(example_scenario(), 5489);
    CampaignState second(example_scenario(), 5489);
    CampaignState different(example_scenario(), 1);
    first.advance_ticks(10);
    second.advance_ticks(4);
    second.advance_ticks(6);

    require(first.clock().tick() == second.clock().tick() &&
            first.clock().elapsed_seconds() == second.clock().elapsed_seconds(),
            "Batching changes simulated time");
    for (const auto& sector : first.world().sectors()) {
        const auto& state = first.sector_state(sector.id);
        const auto& repeated = second.sector_state(sector.id);
        require(state.controller == repeated.controller &&
                state.objective_points == repeated.objective_points,
                "Repeated campaigns diverged");
    }

    const auto first_draw = first.next_random();
    require(first_draw == 3499211612u && first_draw == second.next_random(),
            "Unexpected standard MT19937 sequence");
    require(first_draw != different.next_random(), "Changing the seed has no effect");
    for (int i = 0; i < 16; ++i) {
        require(first.next_random() == second.next_random(), "Identical seeds diverged");
    }
}

void test_economy()
{
    constexpr StockpileId west{1};
    constexpr StockpileId east{2};
    constexpr InstallationId refinery{2};
    constexpr auto crude = static_cast<std::size_t>(Resource::crude_oil);
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    constexpr auto reserve = static_cast<std::size_t>(Resource::oil_reserve);
    constexpr auto materials = static_cast<std::size_t>(Resource::materials);

    auto scenario = example_scenario();
    auto reversed = scenario;
    std::ranges::reverse(reversed.installations);
    CampaignState first(scenario, 42);
    CampaignState second(reversed, 42);
    first.advance_ticks(30);
    second.advance_ticks(4);
    require(second.economy_ledger().empty(), "Production ran before the first interval");
    second.advance_ticks(1);
    require(second.installations()[1].work == 100, "Refinery did not use local output in ID order");
    second.advance_ticks(25);
    require(std::ranges::equal(first.economy_ledger(), second.economy_ledger()),
            "Economy depends on tick batching or scenario input order");
    require(first.next_random() == second.next_random(), "Economy RNG depends on tick batching");
    const auto& stock = first.stockpiles().at(west);
    require(stock.quantity[reserve] == 0 && stock.quantity[crude] == 1000 &&
            stock.quantity[fuel] == 1700 && first.installations()[1].work == 0,
            "Finite extraction or refinery output capacity failed");
    require(first.stockpiles().at(east).quantity[materials] == 0,
            "Factory failed to consume finite materials");

    const auto before = stock.quantity;
    const auto ledger_size = first.economy_ledger().size();
    ResourceAmounts demand{};
    demand[fuel] = 100;
    demand[crude] = 1001;
    require(!first.consume(west, demand) && stock.quantity == before &&
            first.economy_ledger().size() == ledger_size, "Failed consumption was not atomic");
    demand[crude] = 0;
    require(first.consume(west, demand) && stock.quantity[fuel] == 1600 &&
            first.economy_ledger().back() == EconomyEntry{30, west, std::nullopt, demand, {}, {}, {}, ledger_size},
            "Consumption did not debit and journal the request");
    first.advance_ticks(10);
    require(stock.quantity[fuel] == 2400 && stock.quantity[crude] == 0,
            "Refinery did not resume after space became available");
    first.advance_ticks(1'000'000'000);
    require(stock.quantity[fuel] == 2400, "Depleted field created more fuel");

    CampaignState interrupted(scenario, 42);
    interrupted.advance_ticks(5);
    interrupted.set_operating_conditions(refinery, false, 1, 100);
    interrupted.advance_ticks(10);
    interrupted.set_operating_conditions(refinery, true, 0, 100);
    interrupted.advance_ticks(5);
    interrupted.set_operating_conditions(refinery, true, 1, 24);
    interrupted.advance_ticks(5);
    require(interrupted.installations()[1].work == 100, "Blocked installation advanced work");
    interrupted.set_operating_conditions(refinery, true, 1, 50);
    interrupted.advance_ticks(5);
    require(interrupted.installations()[1].work == 150, "Integrity did not scale productivity");
    interrupted.advance_ticks(5);
    require(interrupted.stockpiles().at(west).quantity[fuel] == 900,
            "Refinery lost partial progress during downtime");

    scenario.stockpiles[0].quantity[fuel] = 2401;
    require_throws<std::invalid_argument>([&] { CampaignState invalid(scenario, 42); },
                                          "Stock capacity validation failed");
    scenario = example_scenario();
    scenario.installations[0].recipe.inputs = {};
    require_throws<std::invalid_argument>([&] { CampaignState invalid(scenario, 42); },
                                          "Recipe with free inputs accepted");
    scenario = example_scenario();
    scenario.installations[0].stockpile = east;
    require_throws<std::invalid_argument>([&] { CampaignState invalid(scenario, 42); },
                                          "Cross-faction installation stock accepted");
}

void test_logistics()
{
    constexpr StockpileId origin{1};
    constexpr StockpileId destination{3};
    constexpr ShipmentId order{1};
    constexpr CarrierId carrier{1};
    constexpr RouteId route{1};
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    constexpr SupplyRequest request{order, origin, destination, Resource::fuel, 80};
    ResourceAmounts demand{};
    demand[fuel] = 21;

    auto scenario = logistics_example_scenario();
    scenario.installations.clear();
    CampaignState batched(scenario, 42);
    CampaignState stepped(scenario, 42);
    batched.request_supply(request);
    stepped.request_supply(request);
    batched.advance_ticks(6);
    stepped.advance_ticks(1);
    require(stepped.shipments().at(order).status == ShipmentStatus::loading &&
            stepped.stockpiles().at(origin).quantity[fuel] == 100 &&
            stepped.stockpiles().at(origin).reserved[fuel] == 80 &&
            !stepped.consume(origin, demand), "Loading did not protect its reservation");
    stepped.advance_ticks(1);
    require(stepped.stockpiles().at(origin).quantity[fuel] == 20 &&
            stepped.shipments().at(order).cargo == 80 &&
            stepped.stockpiles().at(destination).quantity[fuel] == 0 &&
            !stepped.consume(origin, demand) && !stepped.consume(destination, demand),
            "Cargo was not moved from stock to transit");
    for (int i = 0; i < 4; ++i) {
        stepped.advance_ticks(1);
    }
    require(stepped.shipments().at(order).status == ShipmentStatus::delivered &&
            stepped.shipments().at(order).cargo == 0 &&
            stepped.stockpiles().at(origin).quantity[fuel] == 20 &&
            stepped.stockpiles().at(destination).quantity[fuel] == 80 &&
            stepped.carriers().at(carrier).fuel == 8, "Shipment did not arrive correctly");
    require(std::ranges::equal(stepped.logistics_log(), batched.logistics_log()) &&
            std::ranges::equal(stepped.economy_ledger(), batched.economy_ledger()),
            "Transport results depend on tick batching");

    auto small_carriers = scenario;
    for (auto& vehicle : small_carriers.carriers) {
        vehicle.capacity = 79;
    }
    CampaignState undersized(small_carriers, 42);
    undersized.request_supply(request);
    undersized.advance_ticks(10);
    require(undersized.shipments().at(order).status == ShipmentStatus::pending &&
            undersized.stockpiles().at(origin).reserved[fuel] == 0,
            "An undersized carrier accepted a shipment");

    constexpr ShipmentId second{2};
    CampaignState congested(scenario, 42);
    congested.request_supply({order, origin, destination, Resource::fuel, 40});
    congested.request_supply({second, origin, destination, Resource::fuel, 40});
    congested.advance_ticks(5);
    require(congested.shipments().at(order).status == ShipmentStatus::unloading &&
            congested.shipments().at(second).status == ShipmentStatus::delayed &&
            congested.logistics_log().back() == LogisticsEvent{
                5, order, LogisticsEventKind::arrived, 40, scenario.stockpiles.back().sector,
                congested.economy_ledger().size() + congested.logistics_log().size() - 1},
            "Route capacity or arrival timing failed");
    congested.advance_ticks(4);
    require(congested.shipments().at(second).status == ShipmentStatus::delivered &&
            congested.stockpiles().at(destination).quantity[fuel] == 80,
            "Waiting carrier did not deliver its cargo");

    auto local = scenario;
    local.stockpiles.back().sector = local.stockpiles.front().sector;
    local.unloading_ticks = 3;
    CampaignState same_sector(local, 42);
    same_sector.request_supply(request);
    same_sector.advance_ticks(4);
    require(same_sector.shipments().at(order).status == ShipmentStatus::unloading &&
            same_sector.shipments().at(order).cargo == 80 &&
            same_sector.stockpiles().at(destination).quantity[fuel] == 0,
            "A local transfer bypassed the configured unloading time");
    same_sector.advance_ticks(1);
    require(same_sector.shipments().at(order).status == ShipmentStatus::delivered &&
            same_sector.stockpiles().at(destination).quantity[fuel] == 80,
            "Local transfer missed its delivery tick");

    scenario.stockpiles.back().quantity[fuel] = 100;
    CampaignState blocked(scenario, 42);
    blocked.request_supply(request);
    blocked.advance_ticks(3);
    blocked.set_route_passable(route, false);
    blocked.advance_ticks(20);
    require(blocked.shipments().at(order).status == ShipmentStatus::delayed &&
            blocked.shipments().at(order).remaining_ticks == 2,
            "Blocked carrier continued to move");
    blocked.set_route_passable(route, true);
    blocked.advance_ticks(3);
    require(blocked.shipments().at(order).cargo == 60 &&
            blocked.stockpiles().at(destination).quantity[fuel] == 120,
            "Partial unload failed to retain its remaining cargo");
    demand[fuel] = 60;
    require(blocked.consume(destination, demand), "Could not free destination space");
    blocked.advance_ticks(1);
    require(blocked.shipments().at(order).status == ShipmentStatus::delivered &&
            blocked.shipments().at(order).cargo == 0, "Partial unload did not resume");

    CampaignState competing(scenario, 42);
    competing.request_supply({ShipmentId{3}, origin, destination, Resource::fuel, 80, 1});
    competing.request_supply({ShipmentId{2}, origin, destination, Resource::fuel, 80, 1});
    competing.request_supply(request);
    competing.advance_ticks(1);
    require(competing.shipments().at(ShipmentId{2}).status == ShipmentStatus::loading &&
            competing.shipments().at(order).status == ShipmentStatus::pending,
            "Dispatch priority or ID ordering failed");
    require(competing.cancel_shipment(ShipmentId{2}) &&
            competing.stockpiles().at(origin).reserved[fuel] == 0,
            "Cancellation did not release a loading reservation");
    competing.advance_ticks(2);
    require(competing.cancel_shipment(ShipmentId{3}) &&
            competing.shipments().at(ShipmentId{3}).cargo == 80 &&
            competing.stockpiles().at(origin).quantity[fuel] == 20,
            "Loaded cancellation moved physical cargo");
    require(competing.lose_carrier(carrier) && !competing.lose_carrier(carrier) &&
            competing.logistics_log().back().quantity == 80 &&
            competing.shipments().at(ShipmentId{3}).cargo == 0,
            "Cargo loss was missing or applied twice");

    CampaignState disconnected(scenario, 42);
    disconnected.set_route_passable(route, false);
    disconnected.request_supply(request);
    disconnected.advance_ticks(1'000'000);
    require(disconnected.shipments().at(order).status == ShipmentStatus::pending &&
            disconnected.stockpiles().at(origin).quantity[fuel] == 100,
            "Adjacency or blockage bypassed the transport graph");

    const auto start = scenario.carriers.front().sector;
    const auto end = scenario.stockpiles.back().sector;
    const auto junction = scenario.world.sector_id(1, 0);
    scenario.routes.push_back({RouteId{2}, start, junction, 2});
    scenario.routes.push_back({RouteId{3}, junction, end, 2});
    CampaignState detour(scenario, 42);
    detour.request_supply(request);
    detour.advance_ticks(2);
    detour.set_route_passable(route, false);
    detour.advance_ticks(1);
    require(detour.shipments().at(order).edge == RouteId{2}, "Carrier did not choose a usable detour");

    auto production = logistics_example_scenario();
    production.stockpiles.front().quantity[fuel] = 0;
    CampaignState supplied(production, 42);
    supplied.request_supply(request);
    supplied.advance_ticks(10);
    require(supplied.shipments().at(order).status == ShipmentStatus::pending &&
            supplied.stockpiles().at(origin).reserved[fuel] == 0 &&
            supplied.stockpiles().at(destination).quantity[fuel] == 0,
            "Dispatch reserved unavailable stock or used same-tick production");
    supplied.advance_ticks(1);
    require(supplied.shipments().at(order).status == ShipmentStatus::loading,
            "New production did not wake a pending shipment");
    supplied.advance_ticks(5);
    require(supplied.shipments().at(order).status == ShipmentStatus::delivered &&
            supplied.stockpiles().at(destination).quantity[fuel] == 80,
            "Produced fuel never reached the destination");
}

void test_recurring_supply()
{
    constexpr StockpileId depot{1};
    constexpr StockpileId base{3};
    constexpr CarrierId vehicle{1};
    constexpr ShipmentId initial{1};
    constexpr ShipmentId top_up{2};
    constexpr ShipmentId recurring{3};
    constexpr RouteId returning{2};
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    auto scenario = logistics_example_scenario();
    scenario.installations.clear();
    scenario.carriers.resize(1);
    const auto pickup = scenario.stockpiles.front().sector;
    const auto destination = scenario.stockpiles.back().sector;
    scenario.routes.push_back({returning, destination, pickup, 2, 1, 2});
    scenario.resupply = {{depot, base, Resource::fuel, 20, 30}};
    CampaignState campaign(scenario, 42);
    campaign.request_supply({initial, depot, base, Resource::fuel, 15});
    campaign.advance_ticks(2);
    require(campaign.shipments().size() == 2 && campaign.shipments().at(initial).cargo == 15 &&
            campaign.shipments().at(top_up).request.quantity == 15 &&
            campaign.stockpiles().at(base).quantity[fuel] == 0,
            "Recurring demand duplicated an existing order or counted cargo as local stock");
    campaign.advance_ticks(4);
    require(campaign.shipments().at(initial).status == ShipmentStatus::delivered &&
            campaign.carriers().at(vehicle).sector == destination &&
            campaign.carriers().at(vehicle).shipment == top_up &&
            campaign.stockpiles().at(depot).reserved[fuel] == 15,
            "Delivered vehicle did not accept the next pickup");
    campaign.advance_ticks(1);
    require(campaign.shipments().at(top_up).edge == returning &&
            campaign.shipments().at(top_up).cargo == 0 &&
            campaign.carriers().at(vehicle).sector == destination && campaign.carriers().at(vehicle).fuel == 6,
            "Empty pickup travel teleported the vehicle or skipped its fuel cost");

    auto interrupted = campaign;
    interrupted.set_route_passable(returning, false);
    interrupted.advance_ticks(3);
    require(interrupted.shipments().at(top_up).status == ShipmentStatus::delayed &&
            interrupted.carriers().at(vehicle).sector == destination &&
            interrupted.carriers().at(vehicle).fuel == 6 &&
            interrupted.shipments().size() == 2,
            "Blocked pickup moved, charged fuel twice, or duplicated its demand");
    require(interrupted.cancel_shipment(top_up) &&
            interrupted.stockpiles().at(depot).reserved[fuel] == 0,
            "Cancellation of an empty trip retained the pickup reservation");
    interrupted.set_route_passable(returning, true);
    interrupted.advance_ticks(1);
    require(interrupted.carriers().at(vehicle).sector == pickup &&
            interrupted.shipments().at(top_up).edge == std::nullopt,
            "An interrupted empty trip did not finish its current leg before reassignment");

    campaign.advance_ticks(6);
    require(campaign.shipments().size() == 2 && campaign.stockpiles().at(base).quantity[fuel] == 30 &&
            campaign.shipments().at(top_up).status == ShipmentStatus::delivered &&
            campaign.carriers().at(vehicle).fuel == 4,
            "The same vehicle did not complete a second delivery");
    ResourceAmounts spent{};
    spent[fuel] = 10;
    require(campaign.consume(base, spent), "Could not reach the reorder threshold");
    campaign.advance_ticks(1);
    require(campaign.shipments().size() == 2, "Demand triggered at the threshold");
    spent[fuel] = 15;
    require(campaign.consume(base, spent), "Could not consume supplies for the next cycle");
    campaign.advance_ticks(8);
    require(campaign.shipments().at(recurring).status == ShipmentStatus::delivered &&
            campaign.shipments().size() == 3 && campaign.stockpiles().at(base).quantity[fuel] == 30 &&
            campaign.stockpiles().at(depot).quantity[fuel] == 43 &&
            campaign.stockpiles().at(depot).reserved[fuel] == 0 &&
            campaign.carriers().at(vehicle).fuel == 2,
            "Recurring delivery or refuelling violated stock conservation");

    // A manual shipment already fills all physical space, even with a higher reorder target.
    scenario.stockpiles.back().capacity[fuel] = 30;
    scenario.stockpiles.back().quantity[fuel] = 18;
    CampaignState full(scenario, 42);
    full.request_supply({initial, depot, base, Resource::fuel, 12});
    full.advance_ticks(10);
    require(full.shipments().size() == 1 && full.stockpiles().at(base).quantity[fuel] == 30,
            "Replenishment exceeded target capacity");

    scenario.stockpiles.back().quantity[fuel] = 30;
    scenario.loading_ticks = 3;
    scenario.carriers.push_back({CarrierId{2}, FactionId::west, destination, 100, 10});
    CampaignState reserved(scenario, 42);
    reserved.request_supply({initial, base, depot, Resource::fuel, 20});
    reserved.advance_ticks(2);
    require(reserved.shipments().size() == 1 && reserved.stockpiles().at(base).reserved[fuel] == 20,
            "An outgoing reservation was mistaken for physical storage space");
    reserved.advance_ticks(2);
    require(reserved.shipments().size() == 2 && reserved.shipments().at(top_up).request.quantity == 20,
            "Loading did not expose space for valid replacement stock");
}

void test_realtime_pacing()
{
    using namespace std::chrono_literals;
    using openwar::cli::ticks_due;
    require(ticks_due(59s, 1, 60) == 0 && ticks_due(60s, 1, 60) == 1,
            "Normal speed did not honor the simulation tick duration");
    require(ticks_due(1500ms, 60, 60) == 1 && ticks_due(2s, 60, 60) == 2,
            "Minute-per-second speed did not round down to full ticks");
    require(ticks_due(1s, 3600, 60) == 60 && ticks_due(1s, 60, 30) == 2,
            "Pacing did not use the configured speed and tick duration");

    const SupplyRequest request{ShipmentId{1}, StockpileId{1}, StockpileId{3}, Resource::fuel, 80};
    CampaignState batch(logistics_example_scenario(), 42);
    batch.request_supply(request);
    batch.advance_ticks(60);
    for (const auto speed : {1u, 60u, 3600u}) {
        CampaignState paced(logistics_example_scenario(), 42);
        paced.request_supply(request);
        const auto duration = 3'600'000ms / speed;
        for (int frame = 1; frame <= 10; ++frame) {
            const auto elapsed = duration * frame / 10;
            const auto target = ticks_due(elapsed, speed, paced.clock().tick_seconds());
            paced.advance_ticks(target - paced.clock().tick());
        }
        require(paced.clock().tick() == batch.clock().tick() &&
                paced.clock().elapsed_seconds() == batch.clock().elapsed_seconds() &&
                std::ranges::equal(paced.logistics_log(), batch.logistics_log()) &&
                std::ranges::equal(paced.economy_ledger(), batch.economy_ledger()),
                "Campaign time or events depend on execution speed");
        for (const auto& [id, stock] : batch.stockpiles()) {
            require(paced.stockpiles().at(id).quantity == stock.quantity,
                    "Stockpile quantities depend on execution speed");
        }
    }
}

void test_forces_and_maintenance()
{
    constexpr EquipmentId apache{1};
    constexpr EquipmentId abrams{2};
    constexpr EquipmentId truck{3};
    constexpr EquipmentId hind_reserve{11};
    constexpr StockpileId west_stock{1};
    constexpr auto parts = static_cast<std::size_t>(Resource::spare_parts);
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    auto batch = maintenance_example_campaign(42);
    auto campaign = maintenance_example_campaign(42);
    const auto& aircraft = campaign.equipment().at(apache);
    const auto& reserve = campaign.equipment().at(hind_reserve);
    require(campaign.equipment_types().size() == 10 && campaign.units().size() == 4 &&
            campaign.equipment().size() == 11 &&
            campaign.units().at(aircraft.assignment.unit).faction == FactionId::west &&
            campaign.units().at(aircraft.assignment.unit).parent == UnitId{1},
            "Scenario did not establish stable equipment ownership and hierarchy");
    require(reserve.status == EquipmentStatus::in_transit && reserve.reinforcement_eta == 8 &&
            !campaign.authorize_reinforcement(hind_reserve, 9),
            "Reserved reinforcement was authorized more than once");
    require(!campaign.reserve_equipment(apache) && !campaign.begin_operation(apache),
            "Damaged aircraft was eligible to operate");
    campaign.advance_ticks(5);
    require(aircraft.status == EquipmentStatus::awaiting_parts && aircraft.job &&
            !aircraft.job->started && aircraft.job->work_seconds == 0,
            "Repair progressed before the parts delivery");
    campaign.advance_ticks(1);
    require(aircraft.status == EquipmentStatus::repair && aircraft.job->started &&
            campaign.stockpiles().at(west_stock).quantity[parts] == 1,
            "Delivered parts did not start and fund the repair");
    campaign.advance_ticks(2);
    require(aircraft.damage != 0 && aircraft.job->work_seconds == 120 &&
            reserve.status == EquipmentStatus::available && !reserve.reinforcement_eta,
            "Repair or reinforcement completed on the wrong tick");
    campaign.advance_ticks(1);
    require(aircraft.status == EquipmentStatus::maintenance && aircraft.damage == 0,
            "Due maintenance did not follow repair");
    campaign.advance_ticks(3);
    require(aircraft.status == EquipmentStatus::available && !aircraft.job &&
            aircraft.last_service_seconds == 3600 &&
            campaign.stockpiles().at(west_stock).quantity[parts] == 0,
            "Scheduled service did not complete at its configured duration");
    batch.advance_ticks(12);
    require(std::ranges::equal(batch.forces_log(), campaign.forces_log()) &&
            std::ranges::equal(batch.economy_ledger(), campaign.economy_ledger()) &&
            std::ranges::equal(batch.logistics_log(), campaign.logistics_log()),
            "Service or reinforcement events depend on tick batching");
    const auto debits = std::ranges::count(campaign.economy_ledger(), std::optional{apache},
                                           &EconomyEntry::equipment);
    require(debits == 2, "Service costs were charged more than once per job");
    require(campaign.reserve_equipment(apache) && campaign.prepare_equipment(apache),
            "Repaired aircraft could not be prepared");
    campaign.advance_ticks(2);
    require(campaign.begin_operation(apache), "Prepared aircraft could not operate");
    campaign.advance_ticks(1);
    require(campaign.finish_operation(apache) && aircraft.operating_seconds == 3660,
            "Aircraft return did not retain operating time");
    campaign.advance_ticks(2);
    require(aircraft.status == EquipmentStatus::preparing && !campaign.reserve_equipment(apache),
            "Aircraft skipped its post-operation preparation");
    campaign.advance_ticks(2);
    require(aircraft.status == EquipmentStatus::available,
            "Aircraft did not complete its post-operation preparation");

    auto queue = maintenance_example_campaign(42);
    queue.lose_equipment(apache);
    require(queue.reserve_equipment(abrams) && !queue.reserve_equipment(abrams) &&
            queue.cancel_equipment_reservation(abrams) && queue.reserve_equipment(abrams) &&
            queue.prepare_equipment(abrams) && !queue.prepare_equipment(abrams) &&
            queue.reserve_equipment(truck) && queue.prepare_equipment(truck),
            "Equipment reservation or preparation admitted incompatible assignments");
    queue.advance_ticks(1);
    require(queue.equipment().at(truck).status == EquipmentStatus::awaiting_crew &&
            queue.stockpiles().at(west_stock).quantity[fuel] == 95,
            "Both jobs used the same maintenance bay");
    queue.set_facility_conditions(FacilityId{1}, true, 0);
    queue.advance_ticks(2);
    require(queue.equipment().at(abrams).job->work_seconds == 0 &&
            queue.stockpiles().at(west_stock).quantity[fuel] == 95,
            "A job advanced or spent resources without technicians");
    queue.set_facility_conditions(FacilityId{1}, true, 2);
    queue.advance_ticks(2);
    require(queue.begin_operation(abrams) && !queue.begin_operation(abrams), "Prepared equipment could not start");
    queue.advance_ticks(1);
    require(queue.equipment().at(abrams).operating_seconds == 60 && queue.finish_operation(abrams),
            "Operation did not accumulate simulated usage");
    queue.advance_ticks(2);
    require(queue.equipment().at(abrams).status == EquipmentStatus::preparing &&
            !queue.reserve_equipment(abrams), "Return bypassed preparation after inspection");
    queue.advance_ticks(2);
    require(queue.equipment().at(abrams).status == EquipmentStatus::available,
            "Turnaround did not restore availability");
    require(queue.lose_equipment(abrams) && !queue.lose_equipment(abrams) &&
            !queue.damage_equipment(abrams, 10) && !queue.reserve_equipment(abrams),
            "Permanent loss could be repeated or reversed");

    auto delayed = maintenance_example_campaign(42);
    delayed.set_facility_conditions(FacilityId{2}, false, 2);
    delayed.advance_ticks(1'000);
    require(delayed.equipment().at(hind_reserve).status == EquipmentStatus::in_transit &&
            delayed.equipment().at(hind_reserve).arrival_delayed, "Closed entry received a reinforcement");
    delayed.set_facility_conditions(FacilityId{2}, true, 2);
    delayed.advance_ticks(1);
    require(delayed.equipment().at(hind_reserve).status == EquipmentStatus::available,
            "Reopened entry did not receive the reinforcement");

    auto invalid = example_scenario();
    invalid.units = {{UnitId{1}, "West", FactionId::west, SectorId{0}, UnitId{2}},
                     {UnitId{2}, "East", FactionId::east, SectorId{2}}};
    require_throws<std::invalid_argument>([&] { CampaignState state(invalid, 42); },
                                          "Cross-faction organizational ownership accepted");
}

void test_command_session()
{
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr UnitId formation{1}, opposing_formation{2};
    const auto setup = combat_example_scenario(west);
    CampaignState campaign(setup, 42), direct(setup, 42);
    CommandSession observer(campaign), commander(campaign, west), opposing(campaign, east);
    const auto initial = commander.snapshot(east);
    require(commander.permissions().may_issue_orders && commander.permissions().faction == west &&
            !observer.permissions().may_issue_orders && !opposing.permissions().may_issue_orders &&
            observer.actions_for(formation).empty() && commander.actions_for(opposing_formation).empty() &&
            !observer.submit(MissionKind::attack, SectorId{1}).mission &&
            !opposing.submit(MissionKind::attack, SectorId{1}).mission,
            "Observer or opposing commander obtained order permissions");
    require(std::ranges::any_of(initial.markers, [west](const auto& marker) { return marker.faction == west; }) &&
            std::ranges::none_of(initial.markers, [east](const auto& marker) { return marker.faction == east; }) &&
            std::ranges::any_of(observer.snapshot(east).markers, [east](const auto& marker) { return marker.faction == east; }),
            "Command perspective switched factions or observer inspection was lost");
    require_throws<std::invalid_argument>([&] { (void)opposing.snapshot(); }, "Unauthorized command snapshot was exposed");
    const auto actions = commander.actions_for(formation);
    require(actions.size() == 5 && std::ranges::all_of(actions, &OrderAction::geographic_destination) &&
            std::ranges::any_of(actions, [](const auto& action) { return action.kind == MissionKind::attack; }),
            "Ground formation did not offer its supported geographic commands");
    const auto rejected = commander.submit(MissionKind::attack, SectorId{2});
    const auto expected = direct.issue_order(west, MissionKind::attack, SectorId{2});
    require(!rejected.mission && rejected.reason == expected.reason &&
            campaign.faction_view(west).events.empty() && campaign.faction_view(west).missions.empty(),
            "Rejected command changed state or supplied a different intelligence diagnostic");
    constexpr geography::Point destination{751'000, 251'000, 0};
    const auto order = commander.submit(MissionKind::attack, destination);
    const auto direct_order = direct.issue_ground_order(west, MissionKind::attack, destination);
    require(order.mission && order.mission == direct_order.mission &&
            campaign.faction_view(west).missions == direct.faction_view(west).missions &&
            campaign.faction_view(west).missions.back().ground_destination == destination &&
            commander.actions_for(formation).empty(), "Command submission changed assignment or geographic validation");
    const auto id = *order.mission;
    require(commander.can_cancel(west, id) && !commander.can_cancel(east, id) &&
            !commander.cancel(east, id).cancelled && !observer.cancel(west, id).cancelled &&
            commander.cancel(west, id).cancelled && !commander.cancel(west, id).cancelled &&
            campaign.faction_view(west).missions.back().status == MissionStatus::cancelled &&
            !commander.actions_for(formation).empty(), "Cancellation bypassed its role or failed to release availability");
    const auto history = commander.snapshot();
    require(history.mission_markers.size() == 1 &&
            history.markers.at(history.mission_markers.front()).mission_status == MissionStatus::cancelled,
            "Same-tick snapshot omitted a cancelled mission from its history");
}

void test_operations_and_intelligence()
{
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr SectorId enemy{2};
    constexpr EquipmentId scout{1};
    constexpr StockpileId base{1};
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    auto campaign = operations_example_campaign(42, west);
    require(!campaign.faction_view(west).sectors.contains(enemy) &&
            campaign.faction_view(west).contacts.empty() &&
            campaign.faction_view(west).installations.size() == 1,
            "Initial faction view revealed hidden information");
    require(!campaign.issue_order(east, MissionKind::recon, SectorId{0}).mission &&
            !campaign.issue_order(west, MissionKind::defend, enemy).mission &&
            !campaign.issue_order(west, MissionKind::recon, SectorId{5}).mission &&
            !campaign.issue_order(west, MissionKind::recon, SectorId{99}).mission,
            "Order bypassed faction authority or target validation");
    const auto order = campaign.issue_order(west, MissionKind::recon, enemy);
    require(order.mission && campaign.stockpiles().at(base).reserved[fuel] == 10 &&
            campaign.equipment().at(scout).status == EquipmentStatus::reserved &&
            !campaign.issue_order(west, MissionKind::recon, enemy).mission &&
            !campaign.cancel_order(east, *order.mission), "Equipment or fuel was reserved twice");
    require(campaign.cancel_order(west, *order.mission) &&
            campaign.stockpiles().at(base).reserved[fuel] == 0 &&
            campaign.equipment().at(scout).status == EquipmentStatus::available,
            "Cancellation did not release the allocation");
    require(campaign.issue_order(west, MissionKind::recon, enemy).mission.has_value(),
            "Unknown territory prevented reconnaissance");
    campaign.advance_ticks(1);
    require(campaign.faction_view(west).missions.back().status == MissionStatus::preparing &&
            campaign.faction_view(west).contacts.empty(), "Preparation granted early intelligence");
    campaign.advance_ticks(2);
    require(campaign.faction_view(west).missions.back().status == MissionStatus::in_progress &&
            campaign.stockpiles().at(base).reserved[fuel] == 0 &&
            campaign.stockpiles().at(base).quantity[fuel] == 90,
            "Launch bypassed preparation or fuel consumption");
    campaign.advance_ticks(4);
    const auto fresh = campaign.faction_view(west);
    require(fresh.missions.back().status == MissionStatus::completed &&
            !fresh.contacts.empty() && fresh.contacts.front().confidence == 1000 &&
            fresh.contacts.front().observed_tick == 7 && fresh.installations.size() == 2 &&
            campaign.faction_view(east).contacts.empty() &&
            !campaign.faction_view(east).sectors.contains(SectorId{0}) &&
            campaign.equipment().at(scout).status == EquipmentStatus::returning,
            "Reconnaissance reports leaked or bypassed turnaround");
    campaign.advance_ticks(4);
    require(campaign.faction_view(west).contacts.front().confidence == 500 &&
            campaign.faction_view(west).contacts.front().estimated_sector == enemy,
            "Contact confidence did not halve from the observation timestamp");

    auto blocked = operations_example_campaign(42, west);
    ResourceAmounts all_fuel{};
    all_fuel[fuel] = 100;
    require(blocked.consume(base, all_fuel) &&
            !blocked.issue_order(west, MissionKind::recon, enemy).mission,
            "An unfunded mission was scheduled");
    auto empty = operations_example_campaign(42, west);
    require(empty.issue_order(west, MissionKind::recon, SectorId{1}).mission.has_value(),
            "Unknown sector was ineligible for reconnaissance");
    empty.advance_ticks(6);
    require(empty.faction_view(west).missions.front().status == MissionStatus::completed &&
            empty.faction_view(west).missions.front().detections == 0 &&
            empty.faction_view(west).contacts.empty(), "Reconnaissance invented a detection");

    auto guard = operations_example_campaign(42, west);
    const auto defense = guard.issue_order(west, MissionKind::defend, SectorId{3});
    require(defense.mission.has_value(), "Defense of a confirmed friendly sector was rejected");
    guard.advance_ticks(6);
    require(guard.faction_view(west).missions.front().status == MissionStatus::completed &&
            guard.sector_state(SectorId{3}).controller == west, "Defense did not complete cleanly");
    auto patrol = operations_example_campaign(42, west);
    require(patrol.issue_order(west, MissionKind::patrol, SectorId{3}).mission.has_value(),
            "Patrol of a friendly sector was rejected");
    patrol.advance_ticks(6);
    require(patrol.faction_view(west).sectors.at(SectorId{3}).observed_tick == 6,
            "Patrol failed to report its observation");
    auto lost = operations_example_campaign(42, west);
    require(lost.issue_order(west, MissionKind::recon, enemy).mission.has_value() &&
            lost.lose_equipment(scout), "Failed to set up unavailable equipment");
    lost.advance_ticks(1);
    require(lost.faction_view(west).missions.front().status == MissionStatus::failed &&
            lost.stockpiles().at(base).reserved[fuel] == 0 &&
            lost.stockpiles().at(base).quantity[fuel] == 100,
            "Failure did not release unspent mission fuel");

    auto moved = operations_example_campaign(42, west);
    require(moved.reserve_equipment(scout), "Could not hold the scout at its base");
    moved.advance_ticks(11);
    auto old_report = moved.faction_view(east);
    const auto air = std::ranges::find(old_report.contacts, "aircraft", &ContactReport::classification);
    require(air != old_report.contacts.end() && air->estimated_sector == SectorId{0},
            "East did not observe the aircraft before it moved");
    require(moved.cancel_equipment_reservation(scout) &&
            moved.issue_order(west, MissionKind::reposition, SectorId{3}).mission.has_value(),
            "Repositioning to a friendly facility was rejected");
    moved.advance_ticks(6);
    const auto after_move = moved.faction_view(east);
    const auto stale = std::ranges::find(after_move.contacts, air->id, &ContactReport::id);
    require(moved.units().at(UnitId{3}).sector == SectorId{3} &&
            stale != after_move.contacts.end() && stale->estimated_sector == air->estimated_sector &&
            stale->observed_tick == air->observed_tick && stale->confidence < air->confidence,
            "An old enemy contact silently followed a relocation");

    auto batch = operations_example_campaign(42);
    auto steps = operations_example_campaign(42);
    require(!batch.issue_order(west, MissionKind::recon, enemy).mission,
            "Observer could issue faction orders");
    batch.advance_ticks(32);
    for (int tick = 0; tick < 32; ++tick) {
        steps.advance_ticks(1);
    }
    for (auto faction : {west, east}) {
        const auto first = batch.faction_view(faction);
        const auto second = steps.faction_view(faction);
        require(!first.missions.empty() && !first.contacts.empty() &&
                first.missions == second.missions && first.events == second.events,
                "Autonomous missions depend on tick batching");
    }
    require(std::ranges::equal(batch.economy_ledger(), steps.economy_ledger()),
            "Mission fuel depends on tick batching");
}

void test_recon_revisit()
{
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    auto campaign = operations_example_campaign(42, west);
    campaign.advance_ticks(11);
    for (auto faction : {west, east}) {
        const auto view = campaign.faction_view(faction);
        require(view.missions.size() == 1, "Expected one reconnaissance per faction at tick 11");
        const auto& mission = view.missions.front();
        require(mission.status == MissionStatus::completed &&
                campaign.equipment().at(mission.equipment).status == EquipmentStatus::returning &&
                campaign.stockpiles().at(mission.launch_stock).reserved[fuel] == 0 &&
                !view.contacts.empty() && view.contacts.front().observed_tick == 11 &&
                view.contacts.front().confidence == 1000,
                "Completed reconnaissance retained a reservation or did not publish fresh contacts");
    }
    campaign.advance_ticks(1);
    const auto fresh = campaign.faction_view(west);
    require(fresh.missions.size() == 1 && !fresh.contacts.empty() && fresh.contacts.front().confidence == 841 &&
            campaign.equipment().at(fresh.missions.front().equipment).status == EquipmentStatus::inspection,
            "Fresh reports were revisited or the aircraft bypassed inspection");
    require(std::ranges::any_of(fresh.events, [](const auto& event) {
                return event.tick == 12 && event.sector == SectorId{3};
            }) && std::ranges::none_of(fresh.events, [](const auto& event) {
                return event.tick == 12 && event.sector == SectorId{2};
            }), "Planner considered a repeat reconnaissance while contacts were still confirmed");
    campaign.advance_ticks(2);
    require(campaign.equipment().at(fresh.missions.front().equipment).status == EquipmentStatus::preparing &&
            !campaign.issue_order(west, MissionKind::recon, SectorId{2}).mission,
            "Aircraft was assigned a second mission during turnaround");
    campaign.advance_ticks(1);
    require(campaign.equipment().at(fresh.missions.front().equipment).status == EquipmentStatus::available,
            "Aircraft did not become available after turnaround");

    campaign.advance_ticks(1);
    for (auto faction : {west, east}) {
        const auto view = campaign.faction_view(faction);
        const auto target = faction == west ? SectorId{2} : SectorId{0};
        require(view.sectors.at(target).confidence == 771 && !view.contacts.empty() &&
                view.contacts.front().confidence == 420 &&
                view.contacts.front().observed_tick == 11 &&
                view.missions.size() == 2 && view.missions[0].status == MissionStatus::completed &&
                view.missions[1].status == MissionStatus::scheduled &&
                view.missions[1].kind == MissionKind::recon && view.missions[1].target == target &&
                view.missions[0].equipment == view.missions[1].equipment,
                "An uncertain contact did not trigger a new reconnaissance of its priority sector");
        const auto& mission = view.missions[1];
        require(campaign.equipment().at(mission.equipment).status == EquipmentStatus::reserved &&
                campaign.stockpiles().at(mission.launch_stock).reserved[fuel] == mission.fuel + 5,
                "The new mission did not hold its flight and preparation fuel");
    }
}

void test_mission_preparation_reservation()
{
    constexpr auto west = FactionId::west;
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    constexpr auto parts = static_cast<std::size_t>(Resource::spare_parts);
    constexpr StockpileId base{1};
    constexpr SectorId target{0};
    constexpr EquipmentId scout{10};
    auto scenario = combat_example_scenario(west);
    scenario.operations.planner_interval_ticks = 0;
    scenario.stockpiles.front().quantity[fuel] = 6;
    scenario.stockpiles.front().quantity[parts] = scenario.stockpiles.front().capacity[parts] = 2;
    ServiceProfile preparation{60};
    preparation.costs[fuel] = 4;
    preparation.costs[parts] = 1;
    scenario.equipment_types.push_back({EquipmentTypeId{10}, "Scout", west,
        EquipmentCategory::helicopter, preparation, {}, {}, {}, 3600, 2});
    for (std::uint32_t id : {10, 11}) {
        scenario.units.push_back({UnitId{id}, "Scout flight", west, target, {}, FacilityId{1}});
        scenario.equipment.push_back({EquipmentId{id}, EquipmentTypeId{10}, UnitId{id}});
    }

    CampaignState campaign(scenario, 42);
    const auto first = campaign.issue_order(west, MissionKind::recon, target);
    require(first.mission && !campaign.issue_order(west, MissionKind::recon, target).mission,
            "Two missions overcommitted the same preparation stock");
    const auto& stock = campaign.stockpiles().at(base);
    ResourceAmounts competing{};
    competing[parts] = 2;
    require(stock.reserved[fuel] == 5 && stock.reserved[parts] == 1 && !campaign.consume(base, competing),
            "Preparation stock was available to a competing consumer");
    require(campaign.cancel_order(west, *first.mission) && stock.reserved == ResourceAmounts{} &&
            stock.quantity[fuel] == 6 && stock.quantity[parts] == 2,
            "Cancellation charged or retained unused preparation stock");
    require(campaign.issue_order(west, MissionKind::recon, target).mission.has_value(),
            "Freed stock could not be allocated again");
    campaign.advance_ticks(2);
    require(stock.quantity[fuel] == 2 && stock.quantity[parts] == 1 &&
            stock.reserved[fuel] == 1 && stock.reserved[parts] == 0,
            "Preparation failed to consume its reservation exactly once");
    campaign.advance_ticks(1);
    require(stock.quantity[fuel] == 1 && stock.quantity[parts] == 1 &&
            stock.reserved == ResourceAmounts{} &&
            campaign.faction_view(west).missions.back().status == MissionStatus::in_progress,
            "Launch charged preparation twice or retained the flight allocation");

    for (const auto ticks : {1u, 2u}) {
        CampaignState interrupted(scenario, 42);
        require(interrupted.issue_order(west, MissionKind::recon, target).mission.has_value(),
                "Could not schedule preparation failure fixture");
        interrupted.advance_ticks(ticks);
        require(interrupted.lose_equipment(scout), "Could not interrupt preparation");
        interrupted.advance_ticks(1);
        const auto& remaining = interrupted.stockpiles().at(base);
        require(interrupted.faction_view(west).missions.front().status == MissionStatus::failed &&
                remaining.reserved == ResourceAmounts{} &&
                remaining.quantity[fuel] == (ticks == 1 ? 6u : 2u) &&
                remaining.quantity[parts] == (ticks == 1 ? 2u : 1u),
                "Preparation failure did not release exactly the unspent stock");
    }
}

void test_destroyed_reinforcement()
{
    constexpr UnitId formation{1};
    constexpr EquipmentId vehicle{1};
    constexpr EquipmentId reserve{4};
    auto scenario = combat_example_scenario();
    scenario.equipment.push_back({reserve, EquipmentTypeId{1}, formation, "Reserve"});
    CampaignState destroyed(scenario, 42);
    destroyed.lose_equipment(vehicle);
    destroyed.advance_ticks(1);
    require(destroyed.combat_units().at(formation).status == GroundStatus::destroyed &&
            !destroyed.authorize_reinforcement(reserve, 3) &&
            destroyed.equipment().at(reserve).status == EquipmentStatus::reserve &&
            destroyed.unit_readiness(formation).ready == 0,
            "A destroyed formation accepted a new reinforcement");

    CampaignState en_route(scenario, 42);
    require(en_route.authorize_reinforcement(reserve, 3), "A surviving formation could not accept reinforcement");
    en_route.lose_equipment(vehicle);
    en_route.advance_ticks(3);
    const auto& item = en_route.equipment().at(reserve);
    require(en_route.combat_units().at(formation).status == GroundStatus::destroyed &&
            item.status == EquipmentStatus::in_transit && item.arrival_delayed &&
            en_route.unit_readiness(formation).ready == 0,
            "A reinforcement already in transit arrived at a destroyed formation");
}

void test_capture_cancels_preparation()
{
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr SectorId center{1};
    constexpr StockpileId base{12};
    constexpr EquipmentId scout{10};
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    constexpr auto parts = static_cast<std::size_t>(Resource::spare_parts);
    auto scenario = combat_example_scenario(east);
    scenario.operations.planner_interval_ticks = 1;
    scenario.objectives = {{west, center, 3}};
    auto& stock = scenario.stockpiles.back();
    stock.quantity[fuel] = stock.capacity[fuel] = 10;
    scenario.facilities.push_back({FacilityId{10}, base, 1, 1});
    scenario.units.push_back({UnitId{10}, "Scout flight", east, center, {}, FacilityId{10}});
    ServiceProfile preparation{600};
    preparation.costs[parts] = 1;
    scenario.equipment_types.push_back({EquipmentTypeId{10}, "Scout", east,
        EquipmentCategory::helicopter, preparation, {}, {}, {}, 3600, 2});
    scenario.equipment.push_back({scout, EquipmentTypeId{10}, UnitId{10}});
    // The capture can interrupt queued work, started work, or preparation that finished this tick.
    for (const auto seconds : {0u, 600u, 180u}) {
        scenario.facilities.back().technicians = seconds ? 1 : 0;
        scenario.equipment_types.back().preparation.seconds = seconds ? seconds : 600;
        CampaignState campaign(scenario, 42);
        require(campaign.issue_order(east, MissionKind::recon, center).mission.has_value(),
                "Could not schedule the capture fixture");
        campaign.advance_ticks(4);
        require(campaign.faction_view(east).missions.front().status == MissionStatus::preparing &&
                campaign.equipment().at(scout).job.has_value(), "Capture fixture did not reach preparation");
        campaign.advance_ticks(1);
        const auto& item = campaign.equipment().at(scout);
        const auto& captured = campaign.stockpiles().at(base);
        require(campaign.sector_state(center).controller == west &&
                campaign.faction_view(east).missions.front().status == MissionStatus::failed &&
                item.status == EquipmentStatus::grounded && !item.job &&
                captured.reserved == ResourceAmounts{} && captured.quantity[fuel] == 10 &&
                captured.quantity[parts] == (seconds ? 6u : 7u),
                "Capture retained mission preparation or incorrectly refunded its costs");
    }
}

void test_combat_and_territory()
{
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr SectorId center{1};
    constexpr UnitId west_unit{1};
    constexpr UnitId east_unit{2};
    constexpr EquipmentId west_vehicle{1};
    constexpr EquipmentId east_vehicle{2};
    constexpr StockpileId west_stores{10};
    constexpr StockpileId east_stores{11};
    constexpr StockpileId industry{12};
    constexpr auto ammo = static_cast<std::size_t>(Resource::ammunition);
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    constexpr auto supply = static_cast<std::size_t>(Resource::supplies);
    constexpr auto parts = static_cast<std::size_t>(Resource::spare_parts);

    auto scenario = combat_example_scenario(west);
    CampaignState campaign(scenario, 42);
    require(!campaign.issue_order(west, MissionKind::attack, SectorId{2}).mission &&
            !campaign.issue_order(east, MissionKind::attack, center).mission &&
            !campaign.issue_order(west, MissionKind::defend, center).mission,
            "Attack or defense bypassed intelligence and faction authority");
    auto manual = CampaignState(scenario, 42);
    require(manual.issue_order(west, MissionKind::defend, SectorId{0}).mission.has_value() &&
            manual.cancel_order(west, MissionId{1}) &&
            manual.issue_order(west, MissionKind::attack, center).mission.has_value() &&
            !manual.issue_order(west, MissionKind::attack, center).mission,
            "Ground orders bypassed the mission allocation lifecycle");
    auto public_territory = scenario;
    public_territory.public_sectors.push_back(SectorId{2});
    CampaignState no_contact(public_territory, 42);
    require(!no_contact.issue_order(west, MissionKind::attack, SectorId{2}).mission,
            "Territorial knowledge revealed a hidden target");
    campaign.advance_ticks(5);
    require(campaign.sector_state(center).controller == FactionId::neutral &&
            campaign.sector_state(center).control == ControlState::contested &&
            campaign.sector_state(center).occupier == west, "Engagement transferred control immediately");
    require(campaign.equipment().at(west_vehicle).damage > 0 &&
            campaign.equipment().at(east_vehicle).status == EquipmentStatus::lost &&
            campaign.equipment().size() == 3, "Combat damage or permanent equipment loss was not retained");
    for (const auto store : {west_stores, east_stores}) {
        const auto& stock = campaign.stockpiles().at(store);
        require(stock.quantity[ammo] == 19 && stock.quantity[fuel] == 19 &&
                stock.quantity[supply] == 19 && stock.sector == center,
                "Combat or movement did not debit the deployed depot once");
    }
    const auto defeated = campaign.combat_units().at(east_unit);
    require(defeated.status == GroundStatus::withdrawing && defeated.morale < 40 &&
            defeated.cohesion < 80 && defeated.fatigue > 0 && defeated.suppression > 0 &&
            campaign.combat_units().at(west_unit).experience == 26,
            "Engagement did not produce experience and loss-driven withdrawal");
    auto interrupted = campaign;
    interrupted.lose_equipment(west_vehicle);
    interrupted.advance_ticks(3);
    require(interrupted.sector_state(center).controller == FactionId::neutral &&
            !interrupted.sector_state(center).occupier, "Destroyed occupier still captured the sector");

    campaign.advance_ticks(2);
    require(campaign.units().at(east_unit).sector == SectorId{2} &&
            campaign.stockpiles().at(east_stores).sector == SectorId{2} &&
            campaign.combat_units().at(east_unit).fatigue < defeated.fatigue &&
            campaign.sector_state(center).controller == FactionId::neutral,
            "Withdrawal or occupation progressed at the wrong time");
    campaign.advance_ticks(1);
    const auto& installation = campaign.installations().back().installation;
    require(campaign.sector_state(center).controller == west &&
            campaign.sector_state(center).control == ControlState::unstable &&
            campaign.front_line() == std::vector{std::pair{center, SectorId{2}}} &&
            !campaign.war_ended(), "Consolidation did not update ownership and the derived front");
    require(installation.operator_faction == west && installation.integrity == 40 && !installation.has_power &&
            campaign.stockpiles().at(industry).owner == west &&
            campaign.stockpiles().at(industry).quantity[parts] == 7,
            "Capture reset damage, power constraints, or inventory");
    const auto other_view = campaign.faction_view(east);
    require(other_view.sectors.at(center).estimated_controller == FactionId::neutral &&
            other_view.sectors.at(center).observed_tick < campaign.clock().tick(),
            "Capture exposed the new owner to an absent opposing observer");

    CampaignState single(scenario, 42);
    single.advance_ticks(90);
    campaign.advance_ticks(82);
    require(single.war_ended() && single.war_result().cause == "scenario objective" &&
            single.war_result() == campaign.war_result() &&
            std::ranges::equal(single.economy_ledger(), campaign.economy_ledger()) &&
            std::ranges::equal(single.observer_battle_log(), campaign.observer_battle_log()),
            "Combat or war results depend on tick batching");
    const auto final_tick = single.clock().tick();
    const auto final_events = single.observer_battle_log().size();
    single.advance_ticks(10);
    require(single.clock().tick() == final_tick && single.observer_battle_log().size() == final_events &&
            !single.issue_order(west, MissionKind::defend, SectorId{0}).mission,
            "A concluded campaign still advanced or accepted orders");

    auto repair = scenario;
    repair.operations.planner_interval_ticks = 0;
    repair.units[1].sector = SectorId{0};
    repair.units[1].facility.reset();
    repair.stockpiles[3].sector = SectorId{0};
    repair.stockpiles[3].quantity[ammo] = 1;
    repair.stockpiles[2].quantity[ammo] = repair.stockpiles[2].quantity[fuel] = 0;
    repair.stockpiles[0].capacity[parts] = repair.stockpiles[0].quantity[parts] = 1;
    CampaignState repairs(repair, 42);
    repairs.advance_ticks(1);
    require(repairs.equipment().at(west_vehicle).damage > 0 &&
            repairs.equipment().at(west_vehicle).status == EquipmentStatus::repair &&
            repairs.equipment().at(east_vehicle).damage == 0,
            "Damage skipped maintenance or an ammunition-starved unit inflicted damage");
    repairs.advance_ticks(4);
    require(repairs.equipment().at(west_vehicle).damage == 0 &&
            repairs.stockpiles().at(StockpileId{1}).quantity[parts] == 0,
            "Battle damage was not repaired with actual depot resources");
    repair.combat_units[0].morale = 20;
    repair.combat_units[0].cohesion = 20;
    CampaignState cut_off(repair, 42);
    cut_off.advance_ticks(1);
    require(cut_off.combat_units().at(west_unit).status == GroundStatus::surrendered &&
            cut_off.equipment().at(west_vehicle).status == EquipmentStatus::lost,
            "A depleted unit without a withdrawal route did not surrender");

    auto air = scenario;
    air.operations.planner_interval_ticks = 0;
    air.equipment_types[0].category = EquipmentCategory::helicopter;
    air.equipment_types[0].can_occupy = false;
    air.units[0].sector = center;
    air.units[0].facility.reset();
    air.stockpiles[2].sector = center;
    air.units[1].sector = center;
    air.units[1].facility.reset();
    air.stockpiles[3].sector = center;
    air.stockpiles[3].quantity[ammo] = 0;
    CampaignState aircraft(air, 42);
    require(!aircraft.issue_order(west, MissionKind::attack, center).mission,
            "Air unit was accepted as an occupier");
    aircraft.advance_ticks(10);
    require(aircraft.sector_state(center).controller == FactionId::neutral &&
            !aircraft.sector_state(center).occupier &&
            aircraft.equipment().at(east_vehicle).status == EquipmentStatus::lost,
            "Air support failed to fight or captured a sector independently");
}

void test_war_endings()
{
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    auto scenario = combat_example_scenario();
    scenario.operations.planner_interval_ticks = 0;
    scenario.victory_objectives.clear();
    scenario.command_objectives.clear();
    scenario.combat.time_limit_seconds.reset();
    auto majority = scenario;
    majority.initial_sectors[1].state.controller = west;
    majority.initial_sectors[2].state.controller = west;
    majority.initial_sectors[3].state.controller = west;
    CampaignState percentage(majority, 42);
    percentage.advance_ticks(20);
    require(!percentage.war_ended(), "Territorial percentage alone ended the war");

    auto time = scenario;
    time.combat.time_limit_seconds = 120;
    CampaignState timeout(time, 42);
    timeout.advance_ticks(20);
    require(timeout.war_result() == WarResult{WarStatus::draw, 2, "time limit"}, "Invalid time-limit outcome");

    auto paired = scenario;
    paired.victory_objectives = {{west, {SectorId{0}}, 120}, {east, {SectorId{2}}, 120}};
    CampaignState objectives(paired, 42);
    objectives.advance_ticks(20);
    require(objectives.war_result() == WarResult{WarStatus::draw, 3, "simultaneous objectives"},
            "Simultaneous objectives favored one faction");
    auto industrial = scenario;
    industrial.victory_objectives = {{west, {SectorId{0}}, 120, {InstallationId{2}}}};
    CampaignState protected_site(industrial, 42);
    protected_site.set_operating_conditions(InstallationId{2}, false, 1, 100);
    protected_site.advance_ticks(3);
    require(!protected_site.war_ended(), "An inoperable required installation won an objective");
    protected_site.set_operating_conditions(InstallationId{2}, true, 1, 100);
    protected_site.advance_ticks(2);
    protected_site.set_operating_conditions(InstallationId{2}, false, 1, 100);
    protected_site.advance_ticks(1);
    protected_site.set_operating_conditions(InstallationId{2}, true, 1, 100);
    protected_site.advance_ticks(3);
    require(protected_site.war_result() == WarResult{WarStatus::west_victory, 9, "scenario objective"},
            "Objective timer accumulated across an interruption");

    auto command = scenario;
    command.command_objectives = {{west, {SectorId{2}}}, {east, {SectorId{0}}}};
    command.combat.capitulation_seconds = 120;
    CampaignState capitulation(command, 42);
    capitulation.advance_ticks(1);
    require(capitulation.war_result().status == WarStatus::ceasefire_pending, "Missing confirmation period");
    capitulation.advance_ticks(20);
    require(capitulation.war_result() == WarResult{WarStatus::draw, 3, "simultaneous capitulation"},
            "Simultaneous capitulation favored one faction");

    CampaignState surrender(scenario, 42);
    require(surrender.declare_surrender(east) && surrender.declare_surrender(west) &&
            !surrender.declare_surrender(east), "Surrender was applied twice");
    surrender.advance_ticks(5);
    require(surrender.war_result() == WarResult{WarStatus::draw, 1, "mutual surrender"},
            "Mutual surrender did not end in a draw");
}

void test_long_campaign()
{
    auto campaign = long_example_campaign(42, false);
    require(campaign.world().width() == 9 && campaign.world().height() == 7 &&
            !campaign.world().sector(SectorId{13}).properties.traversable &&
            !campaign.shipments().empty(), "Missing campaign geography or supply orders");
    const ContactReport old_contact{ContactId{1}, SectorId{31}, "ground", 40, 300};
    SectorReport report{SectorId{31}, FactionId::neutral, 47, 1000};
    require(current_contact(old_contact, report), "A partial report superseded an earlier observation");
    report.surveyed = true;
    require(!current_contact(old_contact, report) &&
            current_contact(ContactReport{ContactId{1}, SectorId{31}, "ground", 47, 1000}, report),
            "The planner did not distinguish obsolete from current contacts");

    campaign.advance_ticks(10);
    for (const auto faction : {FactionId::west, FactionId::east}) {
        const auto view = campaign.faction_view(faction, false);
        require(std::ranges::count(view.missions, MissionKind::attack, &Mission::kind) >= 2 &&
                std::ranges::all_of(view.missions, [](const auto& mission) {
                    return mission.kind != MissionKind::attack || mission.ground_path.size() == 2;
                }), "The planner serialized parallel fronts or bypassed the nearest formation");
    }
    while (!campaign.war_ended()) {
        campaign.advance_ticks(17);
        require(campaign.clock().tick() <= 2880, "The long scenario missed its terminal deadline");
    }
    auto repeated = long_example_campaign(42, false);
    repeated.advance_ticks(3000);
    require(campaign.clock().tick() > 355 && campaign.war_result() == repeated.war_result() &&
            std::ranges::equal(campaign.observer_battle_log(), repeated.observer_battle_log()) &&
            std::ranges::equal(campaign.observer_operations_log(), repeated.observer_operations_log()) &&
            std::ranges::equal(campaign.economy_ledger(), repeated.economy_ledger()) &&
            std::ranges::equal(campaign.logistics_log(), repeated.logistics_log()) &&
            std::ranges::equal(campaign.forces_log(), repeated.forces_log()) &&
            campaign.next_random() == repeated.next_random(), "Long campaign changed with tick batching");
    require(std::ranges::any_of(campaign.logistics_log(), [&](const auto& event) {
                const auto& order = campaign.shipments().at(event.shipment).request;
                return event.kind == LogisticsEventKind::delivered && order.resource == Resource::fuel &&
                       std::ranges::any_of(campaign.combat_units(), [&](const auto& entry) {
                           return entry.second.stores == order.destination;
                       });
            }) && std::ranges::any_of(campaign.forces_log(), [](const auto& event) {
                return event.kind == ForcesEventKind::service_completed && event.service == ServiceKind::repair;
            }), "Campaign failed to deliver fuel or service damaged equipment");
    require(std::ranges::any_of(campaign.logistics_log(), [](const auto& event) {
                return event.kind == LogisticsEventKind::collecting;
            }) && std::ranges::any_of(campaign.logistics_log(), [](const auto& event) {
                return event.kind == LogisticsEventKind::delivered &&
                       static_cast<std::uint32_t>(event.shipment) > 28 && event.tick > 255;
            }), "Long campaign did not complete recurring deliveries and reuse its carriers");
    require(std::ranges::any_of(campaign.observer_operations_log(), [](const auto& event) {
                return event.reason == "launched" && event.tick > 563;
            }) && std::ranges::any_of(campaign.observer_operations_log(), [](const auto& event) {
                return event.reason == "ground operation started" && event.tick > 355;
            }), "Resupply did not restore air and ground operations");
    for (const auto faction : {FactionId::west, FactionId::east}) {
        const auto view = campaign.faction_view(faction, false);
        require(std::ranges::count_if(view.missions, [](const auto& mission) {
                    return mission.kind == MissionKind::attack && mission.status == MissionStatus::completed;
                }) >= 2 && std::ranges::any_of(campaign.observer_battle_log(), [faction](const auto& event) {
                    return event.previous_controller && event.amount == static_cast<std::uint32_t>(faction);
                }), "A faction failed to complete repeated territorial attacks");
    }
}

void test_strategic_command()
{
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr SectorId home{0};
    constexpr SectorId approach{1};
    constexpr SectorId objective{2};
    auto scenario = combat_example_scenario(west);
    scenario.operations.planner_interval_ticks = 10;
    scenario.operations.strategy_review_ticks = 60;
    scenario.operations.max_autonomous_missions = 3;
    scenario.operations.territory_half_life_seconds = 360 * 60;
    scenario.operations.contact_half_life_seconds = 30 * 60;
    scenario.victory_objectives.clear();
    scenario.command_objectives.clear();
    scenario.public_sectors = {objective};
    scenario.objectives = {{west, objective, 6}};
    for (auto& sector : scenario.initial_sectors) {
        if (sector.sector == approach) {
            sector.state.controller = west;
        }
        if (sector.sector == objective) {
            sector.state.controller = FactionId::neutral;
        }
    }
    auto stores = *std::ranges::find(scenario.stockpiles, StockpileId{10}, &Stockpile::id);
    stores.id = StockpileId{20};
    stores.sector = approach;
    scenario.stockpiles.push_back(stores);
    scenario.units.push_back({UnitId{3}, "Second formation", west, approach});
    scenario.combat_units.push_back({UnitId{3}, stores.id});
    scenario.equipment.push_back({EquipmentId{4}, EquipmentTypeId{1}, UnitId{3}});
    scenario.units.push_back({UnitId{4}, "Scout flight", west, home, {}, FacilityId{1}});
    scenario.equipment_types.push_back({EquipmentTypeId{3}, "Scout", west, EquipmentCategory::helicopter,
                                        {}, {}, {}, {}, 3600, 3});
    scenario.equipment.push_back({EquipmentId{5}, EquipmentTypeId{3}, UnitId{4}});

    CampaignState coordinated(scenario, 42);
    require(coordinated.issue_order(west, MissionKind::recon, objective).mission.has_value(),
            "Could not survey the offensive objective");
    coordinated.advance_ticks(10);
    const auto orders = coordinated.faction_view(west);
    require(orders.strategy && orders.strategy->focus == objective && orders.strategy->reviewed_tick == 10 &&
            orders.missions.size() == 3 && orders.missions.front().manual &&
            orders.missions[1].ground_unit != orders.missions[2].ground_unit &&
            orders.missions[1].earliest_launch_tick == 12 && orders.missions[2].duration_ticks == 2,
            "Strategic command did not coordinate independent formations with different travel times");
    coordinated.advance_ticks(1);
    require(coordinated.units().at(UnitId{3}).sector == approach,
            "The nearer formation did not wait for its support");
    coordinated.advance_ticks(1);
    require(std::ranges::count_if(coordinated.observer_battle_log(), [](const auto& event) {
                return event.tick == 12 && event.sector == objective && event.reason == "unit moved";
            }) == 2, "The planned offensive did not arrive together");

    auto manual = scenario;
    manual.world = WorldDefinition(3, 2, 2000, std::vector<SectorProperties>(6));
    CampaignState player_order(manual, 42);
    player_order.advance_ticks(9);
    require(player_order.issue_order(west, MissionKind::attack, objective).mission.has_value(),
            "Could not establish a manual operation before the strategic review");
    player_order.advance_ticks(1);
    const auto player_view = player_order.faction_view(west);
    require(std::ranges::count(player_view.missions, objective, &Mission::target) == 1 &&
            player_view.missions.front().manual && player_view.missions.front().status == MissionStatus::in_progress,
            "The strategy duplicated or displaced a manual operation");

    auto defense = scenario;
    for (auto& sector : defense.initial_sectors) {
        if (sector.sector == objective) {
            sector.state.controller = west;
        }
    }
    CampaignState unaware(defense, 42);
    unaware.advance_ticks(10);
    require(unaware.faction_view(west).missions.empty(),
            "Strategic defense reacted to an unobserved hostile formation");
    CampaignState defending(defense, 42);
    require(defending.issue_order(west, MissionKind::recon, objective).mission.has_value(),
            "Could not survey the defensive objective");
    defending.advance_ticks(10);
    const auto response = defending.faction_view(west);
    require(response.missions.size() == 2 && response.missions.back().kind == MissionKind::defend &&
            response.missions.back().target == objective, "Strategic defense ignored a reported incursion");

    auto staging = combat_example_scenario();
    staging.operations.planner_interval_ticks = 1;
    staging.operations.strategy_review_ticks = 5;
    staging.victory_objectives.clear();
    staging.command_objectives.clear();
    staging.objectives = {{west, objective, 6}};
    staging.public_sectors.push_back(objective);
    for (auto& sector : staging.initial_sectors) {
        if (sector.sector == objective) {
            sector.state.controller = FactionId::neutral;
        }
    }
    CampaignState staged(staging, 42);
    staged.advance_ticks(1);
    const auto opening = staged.faction_view(west);
    require(opening.strategy->focus == objective && opening.missions.size() == 1 &&
            opening.missions.front().target == approach, "Command did not stage a reachable approach");
    staged.advance_ticks(2);
    require(staged.faction_view(west).strategy == opening.strategy,
            "Command discarded its objective between strategic reviews");

    staging.operations.strategy_review_ticks = 2;
    staging.operations.reinforcement_travel_ticks = 2;
    staging.equipment.push_back({EquipmentId{4}, EquipmentTypeId{1}, UnitId{1}});
    staging.equipment.push_back({EquipmentId{5}, EquipmentTypeId{1}, UnitId{1}});
    staging.equipment.push_back({EquipmentId{6}, EquipmentTypeId{1}, UnitId{1}, "Reserve"});
    CampaignState recovery(staging, 42);
    recovery.lose_equipment(EquipmentId{4});
    recovery.lose_equipment(EquipmentId{5});
    recovery.advance_ticks(1);
    const auto recovering = recovery.faction_view(west);
    require(recovering.strategy->recovering && recovering.strategy->ground_losses == 2 &&
            recovering.missions.empty() && recovery.equipment().at(EquipmentId{6}).reinforcement_eta == 3,
            "Losses did not lead to consolidation and a finite reserve commitment");
    recovery.advance_ticks(2);
    const auto resumed = recovery.faction_view(west);
    require(!resumed.strategy->recovering && !resumed.missions.empty() &&
            recovery.equipment().at(EquipmentId{6}).status != EquipmentStatus::in_transit,
            "An arriving replacement did not restore offensive activity");

    auto moving = combat_example_scenario(west);
    moving.operations.planner_interval_ticks = 0;
    moving.equipment.push_back({EquipmentId{6}, EquipmentTypeId{1}, UnitId{1}, "Reserve"});
    CampaignState displaced(moving, 42);
    require(displaced.authorize_reinforcement(EquipmentId{6}, 3) &&
            displaced.issue_order(west, MissionKind::defend, SectorId{3}).mission.has_value(),
            "Could not schedule reinforcement before the formation moved");
    displaced.advance_ticks(3);
    require(displaced.units().at(UnitId{1}).sector == SectorId{3} &&
            displaced.equipment().at(EquipmentId{6}).arrival_delayed,
            "A reinforcement bypassed its entry point to join a deployed formation");

    auto batch = long_example_campaign(42);
    auto stepped = long_example_campaign(42);
    batch.advance_ticks(1000);
    for (int tick = 0; tick < 1000 && !stepped.war_ended(); ++tick) {
        stepped.advance_ticks(1);
    }
    require(batch.faction_view(west, false).strategy == stepped.faction_view(west, false).strategy &&
            batch.faction_view(east, false).strategy == stepped.faction_view(east, false).strategy &&
            batch.war_result() == stepped.war_result() &&
            std::ranges::equal(batch.observer_operations_log(), stepped.observer_operations_log()) &&
            std::ranges::equal(batch.observer_battle_log(), stepped.observer_battle_log()) &&
            std::ranges::equal(batch.logistics_log(), stepped.logistics_log()),
            "Strategic command changed with tick batching");
}

void test_procedural_campaign()
{
    ProceduralSettings settings;
    settings.seed = 0x10000002a;
    settings.width = 13;
    settings.height = 11;
    settings.west_armor = 4;
    settings.east_armor = 2;
    settings.crossings = 4;
    settings.hold_minutes = 37;
    settings.time_limit_minutes = 1000;
    settings.require_relay = 0;
    settings.capitulation = 0;
    const auto scenario = resolve_procedural_scenario(settings);
    const auto repeated = resolve_procedural_scenario(settings);
    auto low_seed = settings;
    low_seed.seed = 42;
    const auto alternate = resolve_procedural_scenario(low_seed);
    const auto sector_equal = [](const Sector& left, const Sector& right) {
        return left.id == right.id && left.properties.terrain == right.properties.terrain &&
               left.properties.traversable == right.properties.traversable && left.neighbors == right.neighbors;
    };
    require(scenario.world.width() == 13 && scenario.world.height() == 11 &&
            std::ranges::equal(scenario.world.sectors(), repeated.world.sectors(), sector_equal) &&
            !std::ranges::equal(scenario.world.sectors(), alternate.world.sectors(), sector_equal),
            "Procedural geography ignored configuration, seed high bits or reproducibility");
    require(scenario.public_sectors == repeated.public_sectors && scenario.public_sectors.size() == 4 &&
            scenario.command_objectives.empty() && scenario.victory_objectives.size() == 2 &&
            scenario.victory_objectives.front().sectors == scenario.public_sectors &&
            scenario.victory_objectives.front().required_installations.empty() &&
            scenario.victory_objectives.front().hold_seconds == 2220 &&
            scenario.combat.time_limit_seconds == 60000,
            "Procedural objectives did not honor the configured victory rules");
    const auto armor_count = [&](FactionId faction) {
        const auto type = std::ranges::find_if(scenario.equipment_types, [faction](const auto& entry) {
            return entry.faction == faction && entry.can_occupy;
        })->id;
        return std::ranges::count_if(scenario.equipment, [type](const auto& item) {
            return item.type == type && item.reserve_source.empty();
        });
    };
    require(armor_count(FactionId::west) == 16 && armor_count(FactionId::east) == 8 &&
            scenario.initial_supply_requests.size() == 36,
            "Procedural deployments did not honor force budgets or create starting orders");

    auto wet = settings;
    wet.width = wet.height = 25;
    wet.sector_size_m = 5000;
    wet.crossings = 5;
    wet.forest_percent = 0;
    wet.water_percent = 100;
    wet.seed = std::numeric_limits<std::uint64_t>::max();
    const auto corridors = resolve_procedural_scenario(wet);
    require(std::ranges::all_of(corridors.routes, [](const auto& route) {
                return route.travel_ticks == 10 && route.fuel_cost == 3;
            }), "Generated roads did not scale with the sector size");
    std::set<SectorId> reached;
    std::queue<SectorId> pending;
    pending.push(corridors.public_sectors.front());
    reached.insert(pending.front());
    while (!pending.empty()) {
        const auto from = pending.front();
        pending.pop();
        for (const auto& route : corridors.routes) {
            const auto& sector = corridors.world.sector(route.to);
            require(sector.properties.traversable, "A generated road crosses impassable water");
            if (route.from == from && reached.insert(route.to).second) {
                pending.push(route.to);
            }
        }
    }
    require(std::ranges::all_of(corridors.stockpiles, [&](const auto& stock) {
                return reached.contains(stock.sector);
            }) && std::ranges::all_of(corridors.public_sectors, [&](SectorId sector) {
                return reached.contains(sector);
            }), "The generator isolated a crossing or installation from the road network");

    auto campaign = procedural_campaign(settings);
    auto stepped = procedural_campaign(settings);
    require(campaign.shipments().size() == scenario.initial_supply_requests.size() &&
            campaign.faction_view(FactionId::west).contacts.empty(),
            "Procedural initialization bypassed logistics or exposed enemy deployments");
    campaign.advance_ticks(400);
    for (std::uint32_t tick = 0; tick < 400; ++tick) {
        stepped.advance_ticks(1);
    }
    require(campaign.war_result() == stepped.war_result() &&
            std::ranges::equal(campaign.observer_battle_log(), stepped.observer_battle_log()) &&
            std::ranges::equal(campaign.observer_operations_log(), stepped.observer_operations_log()) &&
            std::ranges::equal(campaign.logistics_log(), stepped.logistics_log()) &&
            std::ranges::any_of(campaign.logistics_log(), [](const auto& event) {
                return event.kind == LogisticsEventKind::delivered;
            }), "Procedural campaigns did not run reproducibly through the shared simulator");

    settings.width = 129;
    require_throws<std::invalid_argument>([&] { resolve_procedural_scenario(settings); },
                                          "An incompatible procedural map size was accepted");
}

void test_large_procedural_campaigns()
{
    for (const auto size : {32u, 64u, 128u}) {
        ProceduralSettings settings;
        settings.seed = 42;
        settings.width = settings.height = size;
        settings.crossings = size / 8;
        settings.west_armor = settings.east_armor = 5;
        settings.reserve_armor = 2;
        settings.scout_flights = 4;
        settings.ammunition = 800;
        const auto scenario = resolve_procedural_scenario(settings);
        const auto field = [&scenario](StockpileId id) -> const Stockpile& {
            return *std::ranges::find(scenario.stockpiles, id, &Stockpile::id);
        };
        require(scenario.world.sectors().size() == size * size && scenario.world.sector_size_m() == 2000 &&
                scenario.combat_units.size() == 2 * settings.crossings &&
                scenario.equipment.size() == 14 * settings.crossings + 8 &&
                scenario.public_sectors.size() == 2 * settings.crossings,
                "Large generation changed the sector scale or lost formations or crossings");
        for (const auto& unit : scenario.units) {
            require(scenario.world.sector(unit.sector).properties.traversable &&
                    scenario.initial_sectors.at(static_cast<std::uint32_t>(unit.sector)).state.controller == unit.faction,
                    "A generated formation starts on impassable or hostile ground");
        }
        for (const auto& command : scenario.command_objectives) {
            std::set<SectorId> reached{command.sectors.front()};
            std::queue<SectorId> todo;
            todo.push(command.sectors.front());
            while (!todo.empty()) {
                const auto sector = todo.front();
                todo.pop();
                for (const auto& route : scenario.routes) {
                    if (route.from != sector) {
                        continue;
                    }
                    require(scenario.world.sector(route.to).properties.traversable,
                            "A large-world road lies on impassable terrain");
                    const auto owner = scenario.initial_sectors.at(static_cast<std::uint32_t>(route.to)).state.controller;
                    if ((owner == command.faction || owner == FactionId::neutral) && reached.insert(route.to).second) {
                        todo.push(route.to);
                    }
                }
            }
            require(std::ranges::all_of(scenario.stockpiles, [&](const auto& stock) {
                        return stock.owner != command.faction || reached.contains(stock.sector);
                    }) && std::ranges::all_of(scenario.public_sectors, [&](auto sector) {
                        return reached.contains(sector);
                    }), "A large faction has inaccessible stockpiles or crossings");
            const auto victory = std::ranges::find(scenario.victory_objectives, command.faction, &VictoryObjective::faction);
            require(victory != scenario.victory_objectives.end() &&
                    victory->sectors.size() == scenario.public_sectors.size() + 1 &&
                    scenario.initial_sectors.at(static_cast<std::uint32_t>(victory->sectors.back())).state.controller !=
                        command.faction, "A large faction has no opposing victory objective");
        }
        for (const auto& unit : scenario.combat_units) {
            const auto from = field(unit.stores).sector;
            require(std::ranges::any_of(scenario.world.sector(from).neighbors, [&](auto sector) {
                        return std::ranges::find(scenario.public_sectors, sector) != scenario.public_sectors.end();
                    }), "A generated ground formation has no accessible first objective");
        }
        auto batch = procedural_campaign(settings);
        auto stepped = procedural_campaign(settings);
        require(std::ranges::equal(batch.world().sectors(), stepped.world().sectors(),
                                   [](const Sector& left, const Sector& right) {
                                       return left.id == right.id && left.properties.terrain == right.properties.terrain;
                                   }), "A large procedural map changed when regenerated with the same seed");
        batch.advance_ticks(120);
        for (int i = 0; i < 12; ++i) {
            stepped.advance_ticks(10);
        }
        require(batch.clock().elapsed_seconds() == 7200 &&
                !batch.observer_battle_log().empty() &&
                std::ranges::equal(batch.observer_operations_log(), stepped.observer_operations_log()) &&
                std::ranges::equal(batch.observer_battle_log(), stepped.observer_battle_log()) &&
                std::ranges::equal(batch.logistics_log(), stepped.logistics_log()),
                "Large campaigns did not exercise shared rules deterministically");
    }
}

void test_ground_movement()
{
    constexpr auto west = FactionId::west;
    constexpr UnitId unit{1};
    constexpr StockpileId stores{10};
    constexpr SectorId target{3};
    constexpr std::size_t fuel = static_cast<std::size_t>(Resource::fuel);
    auto scenario = combat_example_scenario(west);
    scenario.operations.planner_interval_ticks = 0;
    scenario.world = WorldDefinition(3, 2, 2000, std::vector<SectorProperties>(6));
    CampaignState plains(scenario, 42);
    require(plains.issue_order(west, MissionKind::defend, target).mission.has_value() &&
            plains.ground_movement(unit) == GroundMovement{target, 0, 240},
            "Initial movement estimate did not account for sector size and equipment speed");
    plains.advance_ticks(3);
    require(plains.ground_movement(unit) == GroundMovement{target, 75, 60} &&
            plains.units().at(unit).sector == target &&
            plains.combat_units().at(unit).position == geography::Point{1'000'000, 2'500'000, 0} &&
            plains.stockpiles().at(stores).sector == target && plains.stockpiles().at(stores).quantity[fuel] == 19,
            "Plains traversal failed to retain progress or charged for the leg twice");
    plains.advance_ticks(1);
    require(!plains.ground_movement(unit) && plains.units().at(unit).sector == target &&
            plains.stockpiles().at(stores).sector == target,
            "The unit and its supply stock did not move together on arrival");

    auto shorter_tick = scenario;
    shorter_tick.tick_seconds = 30;
    CampaignState half_step(shorter_tick, 42);
    require(half_step.issue_order(west, MissionKind::defend, target).mission.has_value(),
            "Could not schedule movement with a different simulated tick duration");
    half_step.advance_ticks(7);
    require(half_step.ground_movement(unit).has_value() &&
            half_step.combat_units().at(unit).position == geography::Point{1'000'000, 2'750'000, 0},
            "Short ticks accelerated movement");
    half_step.advance_ticks(1);
    require(half_step.units().at(unit).sector == target &&
            half_step.clock().elapsed_seconds() == plains.clock().elapsed_seconds(),
            "Equal simulated duration yielded different arrival times");

    auto forest = scenario;
    std::vector<SectorProperties> terrain(6);
    terrain[3].terrain = Terrain::forest;
    forest.world = WorldDefinition(3, 2, 2000, terrain);
    auto& deployed = *std::ranges::find(forest.stockpiles, stores, &Stockpile::id);
    deployed.quantity[fuel] = 1;
    CampaignState short_of_fuel(forest, 42);
    require(!short_of_fuel.issue_order(west, MissionKind::defend, target).mission,
            "Route planning underestimated forest fuel requirements");
    deployed.quantity[fuel] = 2;
    CampaignState wooded(forest, 42);
    require(wooded.issue_order(west, MissionKind::defend, target).mission.has_value(),
            "Exact forest fuel allocation was rejected");
    wooded.advance_ticks(4);
    require(wooded.ground_movement(unit) == GroundMovement{target, 50, 240} &&
            wooded.stockpiles().at(stores).quantity[fuel] == 0 && wooded.units().at(unit).sector == target,
            "Forest movement did not use twice the plains travel time and fuel");
    wooded.advance_ticks(4);
    require(wooded.units().at(unit).sector == target, "A paid leg stalled after using its last fuel");

    auto mixed = scenario;
    mixed.equipment_types.push_back({EquipmentTypeId{10}, "Slow ground transport", west,
        EquipmentCategory::transport, {}, {}, {}, {}, 3600, 0, 0, false, 15});
    mixed.equipment.push_back({EquipmentId{10}, EquipmentTypeId{10}, unit});
    CampaignState convoy(mixed, 42);
    require(convoy.issue_order(west, MissionKind::defend, target).mission.has_value() &&
            convoy.ground_movement(unit) == GroundMovement{target, 0, 480},
            "A mixed formation ignored its slowest ready equipment");
    convoy.advance_ticks(2);
    require(convoy.ground_movement(unit) == GroundMovement{target, 25, 360} &&
            convoy.lose_equipment(EquipmentId{10}) &&
            convoy.ground_movement(unit) == GroundMovement{target, 25, 180},
            "Changing equipment readiness lost progress or kept an unavailable speed limit");
    convoy.advance_ticks(3);
    require(convoy.units().at(unit).sector == target && convoy.stockpiles().at(stores).quantity[fuel] == 19,
            "Mixed formation arrival or leg fuel debit was incorrect");

    auto detour = scenario;
    terrain.assign(15, {});
    for (auto i : {1, 2, 3}) {
        terrain[i].terrain = Terrain::forest;
    }
    detour.world = WorldDefinition(5, 3, 2000, terrain);
    detour.initial_sectors.clear();
    for (std::uint32_t i = 0; i < 10; ++i) {
        detour.initial_sectors.push_back({SectorId{i}, {west}});
    }
    detour.initial_sectors.push_back({SectorId{14}, {FactionId::east}});
    detour.units[1].sector = detour.stockpiles[1].sector = detour.stockpiles[3].sector = SectorId{14};
    detour.command_objectives.clear();
    detour.victory_objectives.clear();
    CampaignState roads(detour, 42);
    require(roads.issue_order(west, MissionKind::defend, SectorId{4}).mission.has_value() &&
            roads.ground_movement(unit) == GroundMovement{SectorId{5}, 0, 1440},
            "Pathfinding preferred a shorter forest route to a faster plains route");
    terrain[1] = {Terrain::water, true};
    terrain[5] = {Terrain::plains, false};
    detour.world = WorldDefinition(5, 3, 2000, terrain);
    CampaignState blocked(detour, 42);
    require(!blocked.issue_order(west, MissionKind::defend, SectorId{4}).mission &&
            !blocked.issue_order(west, MissionKind::defend, SectorId{1}).mission &&
            blocked.stockpiles().at(stores).quantity[fuel] == 20,
            "Ground movement bypassed water or an impassable neighbor");

    terrain.assign(15, {});
    detour.world = WorldDefinition(5, 3, 2000, terrain);
    detour.equipment_types.front().ground_speed_kph = 36;
    CampaignState consecutive(detour, 42);
    require(consecutive.issue_order(west, MissionKind::defend, SectorId{2}).mission.has_value(),
            "Could not schedule a multi-leg movement");
    consecutive.advance_ticks(4);
    require(consecutive.units().at(unit).sector == SectorId{1} &&
            consecutive.ground_movement(unit) == GroundMovement{SectorId{2}, 20, 180},
            "Unused travel distance was lost at a sector boundary");
    consecutive.advance_ticks(3);
    require(consecutive.units().at(unit).sector == SectorId{2} &&
            consecutive.stockpiles().at(stores).quantity[fuel] == 18,
            "Multi-leg arrival time or fuel use was rounded independently for each sector");
}

struct CheckpointFiles {
    std::filesystem::path directory;

    CheckpointFiles()
    {
        const auto temporary = std::filesystem::temp_directory_path();
        for (unsigned suffix = 0;; ++suffix) {
            directory = temporary / ("openwar-checkpoint-tests-" + std::to_string(suffix));
            if (std::filesystem::create_directory(directory)) {
                break;
            }
        }
    }

    ~CheckpointFiles()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

void test_explicit_formation_assignment()
{
    constexpr auto west = FactionId::west;
    constexpr UnitId fast{1}, enemy{2}, slow{3}, first_flight{4}, second_flight{5};
    constexpr EquipmentId slow_item{4}, flight_item{6};
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    constexpr SectorId sector{1};
    constexpr geography::Point destination{751'000, 251'000, 0};
    auto scenario = combat_example_scenario(west);
    scenario.operations.planner_interval_ticks = 0;
    scenario.units.push_back({slow, "Slow ground formation", west, SectorId{0}});
    auto slow_type = scenario.equipment_types.front();
    slow_type.id = EquipmentTypeId{3};
    slow_type.ground_speed_kph = 10;
    scenario.equipment_types.push_back(slow_type);
    scenario.equipment.push_back({slow_item, slow_type.id, slow});
    auto slow_stock = *std::ranges::find(scenario.stockpiles, StockpileId{10}, &Stockpile::id);
    slow_stock.id = StockpileId{13};
    scenario.stockpiles.push_back(slow_stock);
    scenario.combat_units.push_back({slow, slow_stock.id});

    for (const auto unit : {first_flight, second_flight}) {
        scenario.units.push_back({unit, "West flight", west, SectorId{0}, {}, FacilityId{1}});
    }
    EquipmentType flight{EquipmentTypeId{4}, "Test helicopter", west, EquipmentCategory::helicopter,
                         {}, {}, {}, {}, 3600, 2};
    flight.preparation.costs[fuel] = 2;
    scenario.equipment_types.push_back(flight);
    scenario.equipment.push_back({EquipmentId{5}, flight.id, first_flight});
    scenario.equipment.push_back({flight_item, flight.id, second_flight});
    auto& launch_stock = *std::ranges::find(scenario.stockpiles, StockpileId{1}, &Stockpile::id);
    launch_stock.quantity[fuel] = 30;

    CampaignState campaign(scenario, 42);
    CommandSession command(campaign, west);
    const auto automatic = command.preview(MissionKind::attack, destination);
    const auto explicit_plan = command.preview(MissionKind::attack, destination, slow);
    require(automatic.formation == fast && explicit_plan.formation == slow &&
            automatic.route && explicit_plan.route && explicit_plan.route->duration_us > automatic.route->duration_us,
            "Preview replaced the requested slower formation");
    require(campaign.equipment().at(slow_item).status == EquipmentStatus::available &&
            campaign.faction_view(west).missions.empty(), "Preview reserved ground equipment");
    require(command.submit(MissionKind::attack, destination, enemy).reason ==
                "selected formation is not under your command" &&
            command.submit(MissionKind::attack, destination, UnitId{999}).reason ==
                "selected formation is not under your command",
            "Unknown and opposing formation identifiers disclosed different information");

    const auto order = command.submit(MissionKind::attack, destination, slow);
    const auto missions = campaign.faction_view(west).missions;
    require(order.mission && missions.size() == 1 && missions.front().ground_unit == slow &&
            missions.front().equipment == slow_item && missions.front().ground_journey &&
            missions.front().ground_journey->route.duration_us == explicit_plan.route->duration_us &&
            campaign.equipment().at(slow_item).status == EquipmentStatus::reserved &&
            campaign.equipment().at(EquipmentId{1}).status == EquipmentStatus::available,
            "Confirmation did not commit the explicit ground preview");
    require(command.submit(MissionKind::attack, sector, slow).reason ==
                "selected formation already has an active order" &&
            campaign.faction_view(west).missions == missions,
            "A conflicting order replaced the active mission");

    CampaignState changed(scenario, 42);
    CommandSession changed_session(changed, west);
    require(changed_session.preview(MissionKind::attack, sector, slow).formation == slow &&
            changed.lose_equipment(slow_item), "Could not change ground readiness after preview");
    const auto declined = changed_session.submit(MissionKind::attack, sector, slow);
    require(!declined.mission && declined.reason.starts_with("selected formation") &&
            changed.faction_view(west).missions.empty() &&
            changed.equipment().at(EquipmentId{1}).status == EquipmentStatus::available,
            "Confirmation failed to revalidate lost equipment or reserved a substitute");
    require(changed_session.submit(MissionKind::attack, sector).mission.has_value() &&
            changed.faction_view(west).missions.front().ground_unit == fast,
            "Automatic assignment without a formation stopped selecting the eligible candidate");

    const auto air = command.preview(MissionKind::recon, sector, second_flight);
    require(command.preview(MissionKind::recon, sector).formation == first_flight &&
            air.formation == second_flight && air.launch_reservation[fuel] == 5 &&
            campaign.stockpiles().at(StockpileId{1}).reserved[fuel] == 0,
            "Air preview substituted a candidate or prematurely reserved launch stock");
    require(command.submit(MissionKind::recon, sector, second_flight).mission.has_value() &&
            campaign.faction_view(west).missions.back().equipment == flight_item &&
            campaign.stockpiles().at(StockpileId{1}).reserved[fuel] == air.launch_reservation[fuel],
            "Explicit air order did not commit the previewed equipment and resources");

    const CheckpointFiles files;
    const auto halfway = files.directory / "explicit-formation.ows";
    campaign.advance_ticks(1);
    campaign.save(halfway);
    auto resumed = CampaignState::load(halfway);
    resumed.advance_ticks(8);
    campaign.advance_ticks(8);
    const auto first = files.directory / "resumed.ows";
    const auto second = files.directory / "continuous.ows";
    resumed.save(first);
    campaign.save(second);
    std::ifstream resumed_bytes(first, std::ios::binary), continuous_bytes(second, std::ios::binary);
    require(std::equal(std::istreambuf_iterator<char>(resumed_bytes), std::istreambuf_iterator<char>(),
                       std::istreambuf_iterator<char>(continuous_bytes), std::istreambuf_iterator<char>()),
            "Explicit missions diverged after checkpoint resume");
}

void test_ground_position_orders()
{
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr UnitId formation{1};
    constexpr EquipmentId vehicle{1};
    constexpr StockpileId stores{10};
    constexpr SectorId neutral{1};
    constexpr geography::Point origin{250'000, 250'000, 0};
    constexpr geography::Point destination{751'000, 251'000, 0};
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    constexpr auto ammo = static_cast<std::size_t>(Resource::ammunition);
    auto scenario = combat_example_scenario(west);
    scenario.operations.planner_interval_ticks = 0;
    scenario.operations.territory_half_life_seconds = 360 * 60;
    scenario.objectives.clear();
    scenario.victory_objectives.clear();
    CampaignState campaign(scenario, 42);
    CommandSession command(campaign, west), opponent(campaign, east);
    const auto actions = command.actions_for(formation);
    for (const auto kind : {MissionKind::move, MissionKind::hold}) {
        require(std::ranges::any_of(actions, [kind](const auto& action) {
            return action.kind == kind && action.geographic_destination;
        }), "Ground position order was unavailable in the command interface");
    }
    require(!command.submit(MissionKind::move, neutral, formation).mission &&
            !command.submit(MissionKind::hold, destination, formation).mission,
            "A position order accepted a sector-only or neutral Hold objective");
    const auto preview = command.preview(MissionKind::move, destination, formation);
    require(preview.formation == formation && preview.route && preview.route->destination == destination &&
            preview.route->fuel_ml == 1000 && campaign.equipment().at(vehicle).status == EquipmentStatus::available,
            "Explicit Move preview used the wrong formation, goal or fuel");
    const auto move = command.submit(MissionKind::move, destination, formation);
    require(move.mission.has_value(), "Cannot Move to a reported neutral position without a campaign objective");
    campaign.advance_ticks(6);
    const auto moved = campaign.faction_view(west).missions.front();
    require(moved.ground_unit == formation && moved.status == MissionStatus::completed &&
            campaign.combat_units().at(formation).position == destination &&
            campaign.units().at(formation).sector == neutral && campaign.stockpiles().at(stores).sector == neutral &&
            campaign.stockpiles().at(stores).quantity[fuel] == 19 &&
            campaign.sector_state(neutral).controller == FactionId::neutral &&
            !campaign.sector_state(neutral).occupier,
            "Move did not reach the exact point or initiated a conquest");

    const auto hold = command.submit(MissionKind::hold, origin, formation);
    require(hold.mission && hold.reason == "scheduled", "Cannot order Hold after repositioning");
    campaign.advance_ticks(3);
    require(campaign.faction_view(west).missions.back().status == MissionStatus::holding &&
            campaign.combat_units().at(formation).position == origin &&
            campaign.equipment().at(vehicle).status == EquipmentStatus::in_operation &&
            command.actions_for(formation).empty() && command.can_cancel(west, *hold.mission) &&
            !opponent.cancel(west, *hold.mission).cancelled &&
            !command.submit(MissionKind::move, destination).mission &&
            command.submit(MissionKind::move, destination, formation).reason ==
                "selected formation already has an active order",
            "Arrival failed to commit Hold or exposed it to reassignment or unauthorized release");
    const auto snapshot = command.snapshot();
    require(snapshot.mission_markers.size() == 2 &&
            snapshot.markers.at(snapshot.mission_markers.front()).mission_status == MissionStatus::holding &&
            snapshot.markers.at(snapshot.mission_markers.back()).mission_status == MissionStatus::completed,
            "Command snapshot did not distinguish standing Hold from completed Move");

    const CheckpointFiles files;
    const auto checkpoint = files.directory / "holding.ows";
    campaign.save(checkpoint);
    auto restored = CampaignState::load(checkpoint);
    CommandSession resumed(restored, west);
    for (auto* state : {&campaign, &restored}) {
        state->advance_ticks(8);
        require(state->faction_view(west).missions.back().status == MissionStatus::holding &&
                state->combat_units().at(formation).position == origin,
                "Hold expired or displaced the formation");
    }
    require(command.cancel(west, *hold.mission).cancelled && resumed.cancel(west, *hold.mission).cancelled &&
            campaign.faction_view(west).missions.back().status == MissionStatus::released &&
            campaign.combat_units().at(formation).position == origin &&
            campaign.equipment().at(vehicle).status == EquipmentStatus::available,
            "Authorized release failed to free the formation at its current position");
    const auto uninterrupted = files.directory / "continuous.ows";
    const auto reloaded = files.directory / "resumed.ows";
    campaign.save(uninterrupted);
    restored.save(reloaded);
    std::ifstream continuous_bytes(uninterrupted, std::ios::binary), resumed_bytes(reloaded, std::ios::binary);
    require(std::ranges::equal(std::istreambuf_iterator<char>(continuous_bytes), std::istreambuf_iterator<char>(),
                               std::istreambuf_iterator<char>(resumed_bytes), std::istreambuf_iterator<char>()),
            "Hold and release changed after checkpoint resume");

    auto threatened = scenario;
    threatened.units[1].sector = SectorId{0};
    threatened.units[1].facility.reset();
    std::ranges::find(threatened.stockpiles, StockpileId{11}, &Stockpile::id)->sector = SectorId{0};
    CampaignState fighting(threatened, 42);
    const auto at_origin = fighting.issue_ground_order(west, MissionKind::hold, origin, formation);
    require(at_origin.mission && at_origin.reason == "holding" &&
            fighting.faction_view(west).missions.back().status == MissionStatus::holding &&
            fighting.stockpiles().at(stores).quantity[fuel] == 20,
            "Hold at the current position was delayed or charged movement fuel");
    fighting.advance_ticks(1);
    require(fighting.faction_view(west).missions.back().status == MissionStatus::holding &&
            fighting.stockpiles().at(stores).quantity[ammo] == 19 &&
            std::ranges::any_of(fighting.observer_battle_log(), [](const auto& event) {
                return event.reason == "engagement";
            }), "Holding bypassed the existing combat rules");

    auto changing = scenario;
    changing.operations.planner_interval_ticks = 1;
    changing.combat.critical_occupation_seconds = 60;
    changing.objectives = {{east, neutral, 3}};
    changing.equipment_types.front().ground_speed_kph = 4;
    CampaignState access(changing, 42);
    require(access.issue_ground_order(west, MissionKind::move, destination, formation).mission.has_value(),
            "Initial neutral destination was unavailable to Move");
    access.advance_ticks(6);
    const auto interrupted = access.faction_view(west);
    require(access.sector_state(neutral).controller == east && access.units().at(formation).sector == SectorId{0} &&
            interrupted.missions.front().status == MissionStatus::failed &&
            std::ranges::any_of(interrupted.events, [](const auto& event) {
                return event.reason == "movement stopped: territorial access lost";
            }),
            "Move crossed into a destination lost to an opposing Attack");
}

void test_ground_patrol_waypoints()
{
    using geography::Point;
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr UnitId formation{1};
    constexpr StockpileId stores{10};
    constexpr SectorId target{1};
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    constexpr Point waypoint{250'000, 350'000, 0};
    constexpr Point destination{751'000, 350'000, 0};
    constexpr std::array via{waypoint};
    auto scenario = combat_example_scenario(west);
    scenario.operations.planner_interval_ticks = 0;
    scenario.victory_objectives.clear();
    scenario.equipment_types.front().ground_speed_kph = 20;
    scenario.units.push_back({UnitId{3}, "East site detail", east, target});
    scenario.equipment.push_back({EquipmentId{4}, EquipmentTypeId{2}, UnitId{3}});

    CampaignState campaign(scenario, 42);
    CommandSession command(campaign, west);
    const auto preview = command.preview(MissionKind::ground_patrol, destination, formation, via);
    require(preview.formation == formation && preview.route && preview.route->destination == destination &&
            preview.route->fuel_ml == 2000 &&
            std::ranges::find(preview.route->vertices, waypoint) != preview.route->vertices.end(),
            "Ground Patrol preview omitted its explicit formation, ordered stop or route fuel");
    constexpr std::array unsafe{destination};
    require(!command.submit(MissionKind::ground_patrol, waypoint, formation, unsafe).mission &&
            !command.submit(MissionKind::ground_patrol, target, formation, via).mission &&
            campaign.faction_view(west).missions.empty(),
            "Ground Patrol accepted an unauthorized midpoint or a sector-only order");
    const auto order = command.submit(MissionKind::ground_patrol, destination, formation, via);
    require(order.mission.has_value(), "Ground Patrol rejected a feasible itinerary");
    campaign.advance_ticks(1);
    const auto partial = campaign.faction_view(west).missions.back();
    require(partial.ground_unit == formation && partial.status == MissionStatus::in_progress &&
            partial.waypoints_reached == 1 && partial.waypoints.front().point == waypoint &&
            campaign.faction_view(west).contacts.empty() &&
            campaign.combat_units().at(formation).position != destination,
            "Ground Patrol did not advance through its route and observe only reached points");

    const CheckpointFiles files;
    const auto midway = files.directory / "patrol-midway.ows";
    campaign.save(midway);
    auto resumed = CampaignState::load(midway);
    auto cancelled = CampaignState::load(midway);
    const auto stopped = cancelled.combat_units().at(formation).position;
    require(cancelled.cancel_order(west, *order.mission), "Could not cancel Ground Patrol mid-route");
    cancelled.advance_ticks(2);
    require(cancelled.faction_view(west).missions.back().waypoints_reached == 1 &&
            cancelled.faction_view(west).missions.back().status == MissionStatus::cancelled &&
            cancelled.combat_units().at(formation).position == stopped && cancelled.faction_view(west).contacts.empty(),
            "Cancelled patrol continued or forgot its reached point");

    campaign.advance_ticks(2);
    resumed.advance_ticks(1);
    resumed.advance_ticks(1);
    const auto finished = campaign.faction_view(west);
    const auto& mission = finished.missions.back();
    require(mission.status == MissionStatus::completed && mission.waypoints_reached == 2 &&
            campaign.combat_units().at(formation).position == destination &&
            campaign.stockpiles().at(stores).quantity[fuel] == 18 &&
            finished.contacts.size() == 1 && !finished.sectors.at(target).surveyed &&
            campaign.faction_view(east).contacts.empty() &&
            campaign.sector_state(target).controller == FactionId::neutral && !campaign.sector_state(target).occupier,
            "Completed patrol lost its point, fuel, observation, or territorial restrictions");
    require(std::ranges::count_if(finished.events, [](const auto& event) {
                return event.reason.starts_with("ground waypoint ");
            }) == 2 &&
            resumed.faction_view(west).events == finished.events &&
            resumed.faction_view(west).missions == finished.missions &&
            resumed.stockpiles().at(stores).quantity == campaign.stockpiles().at(stores).quantity,
            "Checkpoint resume duplicated or changed waypoint observations or fuel");

    CampaignState moving(scenario, 42);
    const auto move = moving.issue_ground_order(west, MissionKind::move, destination, formation, via);
    moving.advance_ticks(2);
    require(move.mission && moving.faction_view(west).missions.back().waypoints_reached == 2 &&
            moving.faction_view(west).missions.back().status == MissionStatus::completed &&
            moving.combat_units().at(formation).position == destination,
            "A Move order did not use the same waypoint execution");

    constexpr std::array distant_via{Point{250'000, 600'000, 0}};
    CampaignState dry(scenario, 42);
    require(dry.issue_ground_order(west, MissionKind::ground_patrol, destination, formation, distant_via).mission.has_value(),
            "Could not schedule the fuel interruption fixture");
    dry.advance_ticks(1);
    ResourceAmounts drained{};
    drained[fuel] = 19;
    require(dry.consume(stores, drained), "Could not drain patrol fuel after departure");
    dry.advance_ticks(2);
    const auto stranded = dry.faction_view(west);
    require(stranded.missions.back().status == MissionStatus::failed && stranded.missions.back().waypoints_reached == 1 &&
            stranded.events.back().reason == "movement stopped: no fuel" &&
            dry.combat_units().at(formation).position == distant_via.front(),
            "Insufficient fuel did not stop the patrol at the next unpaid segment");
    require(!dry.preview_order(west, MissionKind::ground_patrol, destination, formation, via).candidate,
            "A patrol with insufficient fuel passed route validation");

    auto forest = scenario;
    std::vector<SectorProperties> terrain(6);
    terrain[1].terrain = Terrain::forest;
    forest.world = WorldDefinition(3, 2, 500, terrain);
    CampaignState obscured(forest, 42);
    require(obscured.issue_ground_order(west, MissionKind::ground_patrol, destination, formation, via).mission.has_value(),
            "Could not schedule forest patrol");
    obscured.advance_ticks(4);
    const auto obscured_view = obscured.faction_view(west);
    require(obscured_view.missions.back().status == MissionStatus::completed && obscured_view.contacts.empty() &&
            std::ranges::any_of(obscured_view.events, [](const auto& event) {
                return event.reason == "ground waypoint 2 reached, territorial report only";
            }), "A patrol point in forest exposed contacts");

    auto contested = scenario;
    contested.units[1].sector = target;
    contested.units[1].facility.reset();
    std::ranges::find(contested.stockpiles, StockpileId{11}, &Stockpile::id)->sector = target;
    contested.combat_units[1].morale = 80;
    for (auto& type : contested.equipment_types) {
        type.combat_power = 1;
    }
    CampaignState opposed(contested, 42);
    require(opposed.issue_ground_order(west, MissionKind::ground_patrol, destination, formation, via).mission.has_value(),
            "Hidden opposition leaked through patrol confirmation");
    opposed.advance_ticks(3);
    const auto interrupted = opposed.faction_view(west);
    require(interrupted.missions.back().status == MissionStatus::failed &&
            interrupted.missions.back().waypoints_reached == 1 &&
            interrupted.events.back().reason == "ground patrol interrupted by combat",
            "Ground Patrol continued through opposition or lost its reached waypoint");
}

void test_ground_reconnaissance()
{
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr SectorId target{1};
    constexpr UnitId scout{1};
    constexpr StockpileId stores{10};
    constexpr geography::Point objective{730'000, 260'000, 0};
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    auto scenario = combat_example_scenario(west);
    scenario.operations.planner_interval_ticks = 0;
    scenario.operations.contact_half_life_seconds = 120;
    scenario.command_objectives.clear();
    scenario.victory_objectives.clear();
    const auto expected = [&](CampaignState& campaign, MissionStatus status, ReconResult result) {
        const auto missions = campaign.faction_view(west).missions;
        require(missions.size() == 1 && missions.front().ground_unit == scout &&
                missions.front().status == status && missions.front().recon_result == result,
                "Ground reconnaissance mission had an unexpected assignment, phase or result");
        return missions.front();
    };
    CampaignState untrained(scenario, 42);
    require(untrained.issue_ground_order(west, MissionKind::ground_recon, objective, scout).reason ==
                "selected formation has no ground reconnaissance role",
            "Ground Recon assigned an ordinary combat formation");
    scenario.units.front().identity = {MilitaryDomain::land, MilitaryFunction::reconnaissance, MilitaryEchelon::platoon};
    scenario.public_sectors.clear();
    scenario.equipment_types.front().ground_speed_kph = 20;
    scenario.units.push_back({UnitId{3}, "East site detail", east, target});
    scenario.equipment.push_back({EquipmentId{4}, EquipmentTypeId{2}, UnitId{3}});

    CampaignState campaign(scenario, 42);
    CommandSession session(campaign, west);
    const auto actions = session.actions_for(scout);
    require(std::ranges::any_of(actions, [](const auto& action) {
        return action.kind == MissionKind::ground_recon && action.geographic_destination;
    }), "A ready scout did not offer a geographic Ground Recon action");
    const auto preview = session.preview(MissionKind::ground_recon, objective, scout);
    require(preview.formation == scout && preview.route && preview.route->destination == objective &&
            preview.route->fuel_ml == 1000 && !campaign.faction_view(west).sectors.contains(target),
            "Ground Recon could not preview a point with unknown control or exposed its report");
    const auto order = session.submit(MissionKind::ground_recon, objective, scout);
    require(order.mission.has_value(), "Ground Recon confirmation rejected an eligible explicit assignment");
    campaign.advance_ticks(1);
    require(expected(campaign, MissionStatus::in_progress, ReconResult::none).detections == 0 &&
            campaign.combat_units().at(scout).position != objective,
            "Ground Recon did not execute geographic travel before observing");

    const CheckpointFiles files;
    const auto travelling = files.directory / "scout-travelling.ows";
    const auto observing = files.directory / "scout-observing.ows";
    campaign.save(travelling);
    auto resumed_travel = CampaignState::load(travelling);
    campaign.advance_ticks(1);
    expected(campaign, MissionStatus::observing, ReconResult::partial);
    require(campaign.combat_units().at(scout).position == objective && session.actions_for(scout).empty() &&
            session.can_cancel(west, *order.mission) && campaign.faction_view(west).contacts.empty() &&
            campaign.faction_view(west).sectors.at(target).observed_tick == 2,
            "Recon arrival skipped the territorial report, observation phase or formation commitment");
    const auto frame = session.snapshot();
    require(frame.mission_markers.size() == 1 &&
            frame.markers.at(frame.mission_markers.front()).mission_status == MissionStatus::observing,
            "The command snapshot did not expose the observation phase");
    campaign.save(observing);
    auto resumed_observation = CampaignState::load(observing);
    resumed_travel.advance_ticks(2);
    resumed_observation.advance_ticks(1);
    campaign.advance_ticks(1);
    const auto bytes = [&](const CampaignState& state, const char* name) {
        const auto path = files.directory / name;
        state.save(path);
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    require(bytes(campaign, "continuous.ows") == bytes(resumed_travel, "resumed-travel.ows") &&
            bytes(campaign, "continuous.ows") == bytes(resumed_observation, "resumed-observation.ows"),
            "Ground Recon diverged after resuming either active phase");
    const auto completed = expected(campaign, MissionStatus::completed, ReconResult::observed);
    const auto view = campaign.faction_view(west);
    require(completed.detections == 1 && view.contacts.size() == 1 && view.contacts.front().confidence == 1000 &&
            !view.sectors.at(target).surveyed && campaign.faction_view(east).contacts.empty() &&
            campaign.faction_view(east).events.empty() && campaign.faction_view(east).missions.empty() &&
            campaign.combat_units().at(scout).position == objective &&
            campaign.stockpiles().at(stores).quantity[fuel] == 19 &&
            campaign.sector_state(target).controller == FactionId::neutral && !campaign.sector_state(target).occupier &&
            !session.cancel(west, *order.mission).cancelled,
            "Recon completion leaked, captured the objective, moved the scout back or misreported contacts");
    campaign.advance_ticks(2);
    require(campaign.faction_view(west).contacts.front().confidence == 500,
            "Ground Recon contacts did not use existing intelligence aging");

    for (const auto* path : {&travelling, &observing}) {
        auto abandoned = CampaignState::load(*path);
        const auto position = abandoned.combat_units().at(scout).position;
        require(abandoned.cancel_order(west, *order.mission), "Could not cancel Ground Recon during travel or observation");
        abandoned.advance_ticks(2);
        expected(abandoned, MissionStatus::cancelled, path == &observing ? ReconResult::partial : ReconResult::none);
        require(abandoned.combat_units().at(scout).position == position &&
                abandoned.stockpiles().at(stores).quantity[fuel] == 19 &&
                abandoned.faction_view(west).contacts.empty(),
                "Cancelled Recon spent new resources or continued to observe");
        if (path == &observing) {
            require(abandoned.faction_view(west).sectors.at(target).observed_tick == 2,
                    "Cancellation discarded the existing territorial report");
        }
    }

    auto forest = scenario;
    std::vector<SectorProperties> terrain(6);
    terrain[1].terrain = Terrain::forest;
    forest.world = WorldDefinition(3, 2, 500, terrain);
    CampaignState obscured(forest, 42);
    require(obscured.issue_order(west, MissionKind::ground_recon, target).mission.has_value(),
            "Automatic Ground Recon did not select a scout for a sector order");
    obscured.advance_ticks(5);
    require(expected(obscured, MissionStatus::completed, ReconResult::partial).detections == 0 &&
            obscured.faction_view(west).contacts.empty() &&
            !obscured.faction_view(west).sectors.at(target).surveyed,
            "Forest observation exposed enemy equipment or claimed a complete survey");

    auto clear = scenario;
    clear.units.pop_back();
    clear.equipment.pop_back();
    CampaignState empty(clear, 42);
    require(empty.issue_ground_order(west, MissionKind::ground_recon, objective).mission.has_value(),
            "Automatic Ground Recon did not select an eligible scout");
    empty.advance_ticks(3);
    require(expected(empty, MissionStatus::completed, ReconResult::observed).detections == 0 &&
            !empty.faction_view(west).sectors.at(target).surveyed,
            "Zero detections were treated as confirmed sector clearance");

    auto contact = clear;
    contact.equipment_types.front().ground_speed_kph = 30;
    for (auto& type : contact.equipment_types) {
        type.combat_power = 1;
    }
    contact.combat_units[1].morale = 80;
    contact.operations.planner_interval_ticks = 1;
    contact.public_sectors = {target};
    contact.objectives = {{east, target, 3}};
    CampaignState interrupted(contact, 42);
    require(interrupted.issue_ground_order(west, MissionKind::ground_recon, objective, scout).mission.has_value(),
            "Could not start the contested reconnaissance fixture");
    interrupted.advance_ticks(1);
    expected(interrupted, MissionStatus::observing, ReconResult::partial);
    interrupted.advance_ticks(1);
    expected(interrupted, MissionStatus::failed, ReconResult::partial);
    require(std::ranges::any_of(interrupted.observer_battle_log(), [](const auto& event) {
                return event.reason == "engagement";
            }), "Ground Recon escaped existing combat while observing");

    CampaignState dry(scenario, 42);
    ResourceAmounts drained{};
    drained[fuel] = 20;
    require(dry.consume(stores, drained) &&
            dry.issue_ground_order(west, MissionKind::ground_recon, objective, scout).reason ==
                "selected formation has insufficient fuel for the route",
            "An unaffordable recon order was accepted or reported as inaccessible");
    terrain[1].traversable = false;
    scenario.world = WorldDefinition(3, 2, 500, terrain);
    CampaignState inaccessible(scenario, 42);
    require(!inaccessible.issue_ground_order(west, MissionKind::ground_recon, objective, scout).mission,
            "Ground Recon entered an inaccessible objective");
}

void test_geographic_ground_operations()
{
    using namespace geography;
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr UnitId formation{1};
    constexpr StockpileId stores{10};
    constexpr SectorId target{3};
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    constexpr Point origin{200'000, 400'000, 0};
    constexpr Point destination{1'700'000, 400'000, 60'000};

    auto scenario = combat_example_scenario(west);
    scenario.world = WorldDefinition(5, 2, 500, std::vector<SectorProperties>(10));
    scenario.initial_sectors.clear();
    for (std::uint32_t id = 0; id < 5; ++id) {
        scenario.initial_sectors.push_back({SectorId{id}, {west}});
    }
    scenario.initial_sectors.push_back({SectorId{9}, {east}});
    scenario.units[1].sector = SectorId{9};
    scenario.units[1].facility.reset();
    std::ranges::find(scenario.stockpiles, StockpileId{11}, &Stockpile::id)->sector = SectorId{9};
    scenario.operations.planner_interval_ticks = 0;
    scenario.victory_objectives.clear();
    scenario.command_objectives.clear();
    scenario.combat_units[0].position = origin;
    scenario.ground_destinations.emplace(target, destination);
    auto& type = scenario.equipment_types.front();
    type.ground_speed_kph = 36;
    Profile profile{};
    profile.terrain = {Rate{5000, 2000}, Rate{2500, 4000}, {}};
    profile.roads = {Rate{10'000, 1000, 5000}, {}};
    type.mobility = profile;
    Map map{5, 2, 500'000, 100'000, {}, {}, {}, {}, {}};
    for (std::int64_t row = 0; row < 10; ++row) {
        for (std::int64_t column = 11; column < 25; ++column) {
            map.tiles[{column, row}] = {Cover::plain, 60'000};
        }
    }
    map.barriers = {{900'000, 0, 1'100'000, 1'000'000}};
    map.nodes = {{1, {200'000, 200'000, 0}}, {2, {1'700'000, 200'000, 60'000}}};
    map.roads = {{1, 1, 2, {{800'000, 200'000, 60'000}}, Surface::paved, Direction::both,
        Crossing::bridge, 10'000, false}};
    scenario.geographic_world = map;

    const auto plan = [&](CampaignState& campaign) {
        require(campaign.issue_order(west, MissionKind::defend, target).mission.has_value(),
                "Sector objective did not resolve to an authored geographic point");
        const auto mission = campaign.faction_view(west).missions.back();
        require(mission.ground_destination == destination && mission.ground_journey &&
                mission.ground_journey->route.duration_us == 290'000'000 &&
                mission.ground_journey->route.fuel_ml == 2300 &&
                mission.ground_path == std::vector<SectorId>{SectorId{0}, SectorId{1}, SectorId{2}, target},
                "Ground planner did not select the uphill mixed journey");
        return mission.id;
    };
    CampaignState batched(scenario, 42);
    plan(batched);
    batched.advance_ticks(5);
    require(batched.combat_units().at(formation).position == destination &&
            batched.stockpiles().at(stores).sector == target &&
            batched.stockpiles().at(stores).quantity[fuel] == 17 &&
            batched.combat_units().at(formation).movement_fuel_credit_ml == 700 &&
            batched.faction_view(west).missions.back().status == MissionStatus::completed,
            "Arrival, mobile stock, or sub-litre fuel accounting was incorrect");

    CampaignState stepped(scenario, 42);
    plan(stepped);
    stepped.advance_ticks(1);
    require(stepped.combat_units().at(formation).position == Point{300'000, 200'000, 10'000} &&
            stepped.faction_view(west).missions.back().ground_journey->progress.entry_paid,
            "An uphill leg did not preserve partial geographic progress and debit");
    const CheckpointFiles files;
    const auto partial = files.directory / "partial.ows";
    stepped.save(partial);
    auto restored = CampaignState::load(partial);
    for (int n = 0; n < 4; ++n) {
        stepped.advance_ticks(1);
        restored.advance_ticks(1);
    }
    const auto bytes = [&](auto& campaign, const char* name) {
        const auto path = files.directory / name;
        campaign.save(path);
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto continuous = bytes(batched, "batched.ows");
    require(bytes(stepped, "stepped.ows") == continuous && bytes(restored, "restored.ows") == continuous,
            "The geographic journey changed across batching or a checkpoint");

    const auto move = [&](CampaignState& campaign) {
        const auto order = campaign.issue_ground_order(west, MissionKind::move, destination, formation);
        require(order.mission.has_value(), "Could not order an exact geographic Move across the bridge");
        return *order.mission;
    };
    CampaignState moving(scenario, 42);
    move(moving);
    const auto journey = moving.faction_view(west).missions.back().ground_journey;
    require(journey && journey->route.duration_us == 290'000'000 && journey->route.fuel_ml == 2300 &&
            std::ranges::any_of(journey->route.legs, [](const auto& leg) { return leg.road.has_value(); }) &&
            std::ranges::any_of(journey->route.legs, [](const auto& leg) { return !leg.road; }),
            "Move did not reuse the mixed road/off-road profile or exact cost");
    moving.advance_ticks(1);
    moving.save(partial);
    auto resumed_move = CampaignState::load(partial);
    resumed_move.advance_ticks(4);
    moving.advance_ticks(4);
    require(bytes(resumed_move, "resumed-move.ows") == bytes(moving, "continuous-move.ows") &&
            moving.combat_units().at(formation).position == destination &&
            moving.faction_view(west).missions.back().status == MissionStatus::completed &&
            moving.stockpiles().at(stores).quantity[fuel] == 17 &&
            moving.combat_units().at(formation).movement_fuel_credit_ml == 700,
            "Mixed Move changed after resume or did not account for fuel exactly");

    CampaignState cancelled_move(scenario, 42), dry_move(scenario, 42), blocked_move(scenario, 42);
    const auto cancel_id = move(cancelled_move);
    move(dry_move);
    blocked_move.set_road_closed(1, true);
    move(blocked_move);
    require(blocked_move.faction_view(west).missions.back().ground_journey == journey,
            "Move leaked the hidden bridge closure while planning");
    cancelled_move.advance_ticks(1);
    const auto stopped_move = cancelled_move.combat_units().at(formation).position;
    require(cancelled_move.cancel_order(west, cancel_id), "Could not cancel an active Move");
    cancelled_move.advance_ticks(1);
    require(cancelled_move.combat_units().at(formation).position == stopped_move &&
            cancelled_move.stockpiles().at(stores).quantity[fuel] == 19,
            "Cancelling Move refunded fuel or changed the actual position");
    dry_move.advance_ticks(1);
    ResourceAmounts drained_move{};
    drained_move[fuel] = 19;
    require(dry_move.consume(stores, drained_move), "Could not deplete fuel during Move");
    dry_move.advance_ticks(3);
    blocked_move.advance_ticks(1);
    for (const auto* state : {&dry_move, &blocked_move}) {
        const auto view = state->faction_view(west);
        require(view.missions.back().status == MissionStatus::failed &&
                view.events.back().reason == (state == &dry_move ? "movement stopped: no fuel" :
                                              "movement stopped: route obstructed") &&
                state->equipment().at(EquipmentId{1}).status == EquipmentStatus::available,
                "Interrupted Move retained the mission or lost the cause");
    }
    require(blocked_move.faction_view(west).roads.at(1).closed && blocked_move.faction_view(east).roads.empty(),
            "Move did not learn the obstruction through faction observation");
    const auto refuel = dry_move.preview_order(west, MissionKind::move, destination, formation);
    require(!refuel.candidate && refuel.reason == "selected formation has insufficient fuel for the route",
            "Move preview accepted an unaffordable route");

    require(restored.issue_ground_order(west, MissionKind::defend, origin).mission.has_value() &&
            restored.faction_view(west).missions.back().ground_journey->route.duration_us == 230'000'000,
            "The same journey did not cost less downhill");
    restored.advance_ticks(4);
    require(restored.combat_units().at(formation).position == origin &&
            restored.stockpiles().at(stores).quantity[fuel] == 15 &&
            restored.combat_units().at(formation).movement_fuel_credit_ml == 400,
            "A second order did not reuse fuel already drawn from the mobile stock");

    CampaignState interrupted(scenario, 42);
    const auto mission = plan(interrupted);
    interrupted.advance_ticks(1);
    const auto stopped = interrupted.combat_units().at(formation).position;
    require(interrupted.cancel_order(west, mission), "Cannot interrupt a manual geographic order");
    interrupted.advance_ticks(5);
    require(interrupted.combat_units().at(formation).position == stopped &&
            interrupted.stockpiles().at(stores).quantity[fuel] == 19,
            "Cancelling a journey changed its position or refunded fuel");

    CampaignState empty(scenario, 42);
    plan(empty);
    empty.advance_ticks(1);
    ResourceAmounts drained{};
    drained[fuel] = 19;
    require(empty.consume(stores, drained), "Could not drain the movement fixture stock");
    empty.advance_ticks(3);
    require(empty.combat_units().at(formation).position == Point{800'000, 200'000, 60'000} &&
            empty.faction_view(west).missions.back().status == MissionStatus::failed,
            "An already-paid leg could not finish, or the next leg moved without fuel");
    CampaignState underfunded(scenario, 42);
    drained[fuel] = 18;
    require(underfunded.consume(stores, drained) && !underfunded.issue_order(west, MissionKind::defend, target).mission,
            "Mission planning accepted an unaffordable geographic journey");

    auto mixed_equipment = scenario;
    auto road_vehicle = type;
    road_vehicle.id = EquipmentTypeId{10};
    road_vehicle.can_occupy = false;
    road_vehicle.mobility->terrain = {};
    mixed_equipment.equipment_types.push_back(road_vehicle);
    mixed_equipment.equipment.push_back({EquipmentId{10}, road_vehicle.id, formation});
    CampaignState restricted(mixed_equipment, 42);
    require(!restricted.issue_order(west, MissionKind::defend, target).mission,
            "A mixed formation ignored a ready vehicle's off-road restriction");

    auto fast_clock = scenario;
    fast_clock.tick_seconds = 300;
    fast_clock.initial_sectors[3].state.controller = FactionId::neutral;
    fast_clock.public_sectors.push_back(target);
    CampaignState occupation(fast_clock, 42);
    require(occupation.issue_order(west, MissionKind::attack, target).mission.has_value(),
            "Could not order occupation of the geographic objective");
    occupation.advance_ticks(1);
    std::vector<std::pair<SectorId, std::int64_t>> crossings;
    for (const auto& event : occupation.observer_battle_log()) {
        if (event.unit == formation && event.movement_offset_us) {
            require(event.tick == 1, "A single tick lost a sector crossing");
            crossings.emplace_back(event.sector, *event.movement_offset_us);
        }
    }
    require(crossings == std::vector<std::pair<SectorId, std::int64_t>>{
                {SectorId{1}, 100'000'000}, {SectorId{2}, 180'000'000}, {target, 230'000'000}} &&
            occupation.combat_units().at(formation).position == destination &&
            occupation.sector_state(target).controller == FactionId::neutral &&
            occupation.sector_state(target).occupier == west,
            "Multiple crossings or occupation timing did not follow the actual positions");
    occupation.advance_ticks(1);
    require(occupation.sector_state(target).controller == west,
            "Existing occupation rules did not capture a geographic objective");

    auto threatened = scenario;
    threatened.units[1].sector = SectorId{1};
    threatened.combat_units[1].position = Point{750'000, 400'000, 0};
    auto& enemy_stores = *std::ranges::find(threatened.stockpiles, StockpileId{11}, &Stockpile::id);
    enemy_stores.sector = SectorId{1};
    enemy_stores.quantity[static_cast<std::size_t>(Resource::ammunition)] = 0;
    threatened.initial_sectors.push_back({SectorId{6}, {east}});
    CampaignState fighting(threatened, 42);
    plan(fighting);
    fighting.advance_ticks(2);
    require(fighting.combat_units().at(formation).position == Point{500'000, 200'000, 30'000} &&
            fighting.combat_units().at(UnitId{2}).status == GroundStatus::withdrawing,
            "A hidden opposing formation did not stop movement on actual sector entry");
    fighting.advance_ticks(1);
    require(fighting.combat_units().at(UnitId{2}).position == Point{750'000, 750'000, 0} &&
            fighting.units().at(UnitId{2}).sector == SectorId{6} &&
            fighting.stockpiles().at(StockpileId{11}).sector == SectorId{6},
            "Withdrawal did not use the geographic journey to the adjacent friendly sector");

    auto unknown_middle = scenario;
    unknown_middle.initial_sectors[2].state.controller = east;
    CampaignState denied(unknown_middle, 42);
    require(!denied.issue_order(west, MissionKind::defend, target).mission,
            "A geographic road bypassed the restriction on unconfirmed intermediate sectors");

    auto coarse_window = scenario;
    coarse_window.geographic_world->tile_mm = 1000;
    coarse_window.geographic_world->tiles.clear();
    CampaignState limited(coarse_window, 42);
    const auto refused = limited.issue_order(west, MissionKind::defend, target);
    require(!refused.mission && refused.reason == "ground route search limit exceeded",
            "Spatial search exhaustion was reported as a physical obstruction");

    scenario.operations.installation_half_life_seconds = 60;
    CampaignState patrol(scenario, 42), blocked_patrol(scenario, 42);
    blocked_patrol.set_road_closed(1, true);
    const std::array bridge_approach{map.nodes.front().position};
    for (auto* state : {&patrol, &blocked_patrol}) {
        require(state->issue_ground_order(west, MissionKind::ground_patrol, destination, formation,
                                          bridge_approach).mission.has_value(),
                "Patrol could not route from an off-road starting point through a bridge approach");
    }
    require(patrol.faction_view(west).missions == blocked_patrol.faction_view(west).missions,
            "Patrol planning exposed a hidden bridge closure");
    blocked_patrol.advance_ticks(1);
    const auto stopped_patrol = blocked_patrol.faction_view(west);
    require(stopped_patrol.missions.back().status == MissionStatus::failed &&
            stopped_patrol.missions.back().waypoints_reached == 1 &&
            stopped_patrol.roads.at(1).closed && blocked_patrol.faction_view(east).roads.empty() &&
            blocked_patrol.combat_units().at(formation).position == bridge_approach.front(),
            "Patrol failed to retain its geographic waypoint when a bridge stopped travel");

    CampaignState unobstructed(scenario, 42), obstructed(scenario, 42);
    obstructed.set_road_closed(1, true);
    plan(unobstructed);
    plan(obstructed);
    require(unobstructed.faction_view(west).missions == obstructed.faction_view(west).missions &&
            unobstructed.ground_movement(formation) == obstructed.ground_movement(formation) &&
            obstructed.faction_view(west).roads.empty(),
            "A hidden bridge closure leaked through the mission, route preview or ETA");
    obstructed.advance_ticks(1);
    const auto observed = obstructed.faction_view(west);
    require(observed.missions.back().status == MissionStatus::failed &&
            observed.roads.at(1) == RoadReport{true, 1} &&
            std::ranges::any_of(observed.events, [](const auto& event) {
                return event.reason == "road #1 reported closed";
            }) &&
            obstructed.faction_view(east).roads.empty() && obstructed.faction_view(east).events.empty(),
            "Execution did not report the actual obstruction solely to the observing faction");
    require(obstructed.combat_units().at(formation).position == map.nodes.front().position &&
            obstructed.stockpiles().at(stores).quantity[fuel] == 19 &&
            obstructed.combat_units().at(formation).movement_fuel_credit_ml == 600 &&
            obstructed.equipment().at(EquipmentId{1}).status == EquipmentStatus::available,
            "Bridge entry spent fuel, moved the unit or left a reservation after it was blocked");

    const auto blocked_save = files.directory / "observed-closure.ows";
    obstructed.save(blocked_save);
    auto resumed_closure = CampaignState::load(blocked_save);
    require(resumed_closure.closed_roads().contains(1) && resumed_closure.faction_view(west).roads == observed.roads &&
            resumed_closure.faction_view(east).roads.empty(), "Checkpoint lost either truth or faction road knowledge");
    const auto denied_closed = obstructed.issue_order(west, MissionKind::defend, target);
    obstructed.set_road_closed(1, false);
    const auto denied_reopened = obstructed.issue_order(west, MissionKind::defend, target);
    require(!denied_closed.mission && !denied_reopened.mission && denied_reopened.reason == denied_closed.reason,
            "The refusal exposed a reopening that had not been observed");

    for (auto* campaign : {&obstructed, &resumed_closure}) {
        campaign->set_road_closed(1, false);
        campaign->advance_ticks(1);
        const auto stale = campaign->faction_view(west).roads.at(1);
        require(stale.closed && stale.confidence == 500 &&
                campaign->issue_order(west, MissionKind::defend, target).mission.has_value(),
                "Stale reports were erased or treated as certain road conditions");
        campaign->advance_ticks(5);
        require(campaign->combat_units().at(formation).position == destination &&
                !campaign->faction_view(west).roads.at(1).closed,
                "A renewed movement did not update its road report from direct passage");
    }
    require(obstructed.combat_units().at(formation).movement_fuel_credit_ml ==
                resumed_closure.combat_units().at(formation).movement_fuel_credit_ml &&
            obstructed.stockpiles().at(stores).quantity == resumed_closure.stockpiles().at(stores).quantity &&
            obstructed.faction_view(west).missions == resumed_closure.faction_view(west).missions &&
            obstructed.faction_view(west).roads == resumed_closure.faction_view(west).roads &&
            std::ranges::equal(obstructed.observer_operations_log(), resumed_closure.observer_operations_log()),
            "Retried ground movement diverged after resuming a checkpoint with observations");
}

ScenarioDefinition geographic_transport_scenario()
{
    using namespace geography;
    constexpr auto west = FactionId::west;
    constexpr auto supplies = static_cast<std::size_t>(Resource::supplies);
    ResourceAmounts capacity{};
    capacity[supplies] = 100;
    auto stock = capacity;
    stock[supplies] = 40;
    ScenarioDefinition scenario{WorldDefinition(3, 2, 500, std::vector<SectorProperties>(6)),
        {{SectorId{0}, {west}}, {SectorId{1}, {west}}, {SectorId{2}, {west}}}};
    scenario.tick_seconds = 10;
    scenario.stockpiles = {{StockpileId{1}, SectorId{0}, west, capacity, stock},
                           {StockpileId{2}, SectorId{2}, west, capacity, stock}};
    scenario.stockpiles[0].access_point = Point{200'000, 350'000, 0};
    scenario.stockpiles[1].access_point = Point{1'200'000, 350'000, 40'000};
    scenario.carriers = {{CarrierId{1}, west, SectorId{0}, 50, 10}};
    scenario.carriers[0].position = Point{100'000, 350'000, 0};
    Profile profile{};
    profile.terrain = {Rate{5000, 2000}, Rate{2500, 4000}, {}};
    profile.roads[0] = {10'000, 1000, 5000};
    scenario.carriers[0].mobility = profile;
    Map map{3, 2, 500'000, 50'000, {}, {}, {}, {}, {}};
    for (std::int64_t y = 0; y < 20; ++y) {
        for (std::int64_t x = 12; x < 30; ++x) {
            map.tiles[{x, y}] = {Cover::plain, 40'000};
        }
    }
    map.barriers = {{450'000, 0, 550'000, 1'000'000}};
    map.nodes = {{1, {200'000, 200'000, 0}}, {2, {1'200'000, 200'000, 40'000}}};
    map.roads = {{1, 1, 2, {{600'000, 200'000, 40'000}}, Surface::paved,
        Direction::both, Crossing::bridge, 10'000, false}};
    scenario.geographic_world = std::move(map);
    return scenario;
}

void test_geographic_transport()
{
    using namespace geography;
    constexpr auto west = FactionId::west;
    constexpr ShipmentId outgoing{1}, incoming{2};
    constexpr CarrierId vehicle{1}, other{2};
    constexpr StockpileId depot{1}, base{2};
    constexpr auto supplies = static_cast<std::size_t>(Resource::supplies);
    const SupplyRequest request{outgoing, depot, base, Resource::supplies, 10};
    auto scenario = geographic_transport_scenario();
    scenario.loading_ticks = scenario.unloading_ticks = 2;
    CampaignState mixed(scenario, 42), batched(scenario, 42);
    mixed.request_supply(request);
    batched.request_supply(request);
    mixed.advance_ticks(2);
    require(mixed.carriers().at(vehicle).position == Point{150'000, 350'000, 0} &&
            mixed.carriers().at(vehicle).sector == SectorId{0} &&
            mixed.shipments().at(outgoing).cargo == 0 && mixed.stockpiles().at(depot).quantity[supplies] == 40,
            "A carrier on the depot sector loaded away from its access point");
    auto cancelled_empty = mixed;
    require(cancelled_empty.cancel_shipment(outgoing) &&
            cancelled_empty.stockpiles().at(depot).reserved[supplies] == 0,
            "Cancelling on the way to pickup did not release stock");
    cancelled_empty.advance_ticks(3);
    require(!cancelled_empty.carriers().at(vehicle).shipment &&
            cancelled_empty.stockpiles().at(depot).quantity[supplies] == 40 &&
            cancelled_empty.carriers().at(vehicle).spent_ml == 100,
            "Empty cancellation skipped paid movement or changed cargo stock");
    mixed.advance_ticks(3);
    require(mixed.shipments().at(outgoing).cargo == 10 &&
            mixed.carriers().at(vehicle).position == mixed.stockpile_position(depot),
            "Pickup did not occur at the access point after the loading time");
    mixed.advance_ticks(12);
    require(mixed.shipments().at(outgoing).road == 1 &&
            mixed.shipments().at(outgoing).journey->progress.entry_paid,
            "The mixed journey did not hold bridge capacity during transit");

    const CheckpointFiles files;
    const auto midway = files.directory / "geographic-logistics.ows";
    mixed.save(midway);
    auto restored = CampaignState::load(midway);
    auto cancelled_loaded = restored;
    const auto stopped = cancelled_loaded.carriers().at(vehicle).position;
    require(cancelled_loaded.cancel_shipment(outgoing), "Could not cancel a loaded geographic shipment");
    cancelled_loaded.advance_ticks(5);
    require(cancelled_loaded.carriers().at(vehicle).position == stopped &&
            cancelled_loaded.shipments().at(outgoing).cargo == 10 &&
            cancelled_loaded.stockpiles().at(base).quantity[supplies] == 40,
            "A loaded cancellation moved the vehicle or credited the recipient");

    mixed.advance_ticks(7);
    require(mixed.carriers().at(vehicle).sector == SectorId{2} &&
            mixed.carriers().at(vehicle).position != mixed.stockpile_position(base) &&
            mixed.stockpiles().at(base).quantity[supplies] == 40,
            "The carrier unloaded on sector entry");
    mixed.advance_ticks(3);
    for (int i = 0; i < 10; ++i) {
        restored.advance_ticks(1);
    }
    batched.advance_ticks(27);
    const auto save_bytes = [&](const CampaignState& state) {
        state.save(midway);
        std::ifstream stream(midway, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    const auto expected = save_bytes(mixed);
    require(save_bytes(restored) == expected && save_bytes(batched) == expected,
            "Road commitments or movement progress changed across batching or save/load");
    require(mixed.shipments().at(outgoing).status == ShipmentStatus::delivered &&
            mixed.carriers().at(vehicle).position == mixed.stockpile_position(base) &&
            mixed.stockpiles().at(depot).quantity[supplies] == 30 &&
            mixed.stockpiles().at(base).quantity[supplies] == 50 &&
            mixed.carriers().at(vehicle).fuel == 9 && mixed.carriers().at(vehicle).spent_ml == 800,
            "The completed mixed journey violated stock or fuel conservation");

    auto roads = geographic_transport_scenario();
    auto& map = *roads.geographic_world;
    map.tile_mm = 100'000;
    map.tiles.clear();
    roads.stockpiles[0].access_point = map.nodes.front().position;
    roads.stockpiles[1].access_point = map.nodes.back().position;
    roads.carriers[0].position = roads.stockpiles[0].access_point;
    roads.carriers[0].mobility->terrain = {};
    auto reverse = roads.carriers.front();
    reverse.id = other;
    reverse.position = roads.stockpiles[1].access_point;
    reverse.sector = SectorId{2};
    roads.carriers.push_back(reverse);
    CampaignState hidden_bridge(roads, 42);
    hidden_bridge.set_road_closed(1, true);
    hidden_bridge.request_supply(request);
    hidden_bridge.advance_ticks(2);
    require(hidden_bridge.shipments().at(outgoing).cargo == 10 &&
            hidden_bridge.faction_view(west).roads.empty(), "Dispatch or fuel planning leaked a hidden closure");
    hidden_bridge.advance_ticks(1);
    require(hidden_bridge.shipments().at(outgoing).delay == LogisticsDelay::closed &&
            hidden_bridge.shipments().at(outgoing).cargo == 10 && !hidden_bridge.shipments().at(outgoing).road &&
            hidden_bridge.carriers().at(vehicle).spent_ml == 0 && hidden_bridge.carriers().at(vehicle).fuel == 10 &&
            hidden_bridge.faction_view(west).roads.at(1) == RoadReport{true, 3} &&
            hidden_bridge.faction_view(FactionId::east).roads.empty(),
            "A blocked carrier lost cargo, fuel or a road slot, or shared knowledge with its adversary");
    hidden_bridge.request_supply({incoming, base, depot, Resource::supplies, 10});
    hidden_bridge.advance_ticks(1);
    require(hidden_bridge.shipments().at(incoming).status == ShipmentStatus::pending &&
            !hidden_bridge.shipments().at(incoming).carrier,
            "A later dispatch ignored its faction's observed bridge closure");

    auto detour_map = roads;
    auto alternate = detour_map.geographic_world->roads.front();
    alternate.id = 2;
    alternate.surface = Surface::gravel;
    alternate.waypoints = {{600'000, 300'000, 40'000}};
    detour_map.geographic_world->roads.push_back(alternate);
    detour_map.carriers.front().mobility->roads[1] = {5000, 1000, 5000};
    CampaignState detour(detour_map, 42);
    detour.set_road_closed(1, true);
    detour.request_supply(request);
    detour.advance_ticks(3);
    const auto& redirected = detour.shipments().at(outgoing);
    require(redirected.road == 2 && redirected.cargo == 10 && detour.faction_view(west).roads.at(1).closed,
            "A convoy did not choose an alternate after discovering an obstruction");
    const auto detour_fuel = redirected.journey->route.fuel_ml;
    detour.advance_ticks(40);
    const auto& truck = detour.carriers().at(vehicle);
    require(detour.shipments().at(outgoing).status == ShipmentStatus::delivered &&
            detour.stockpiles().at(base).quantity[supplies] == 50 &&
            10'000 - static_cast<std::int64_t>(truck.fuel * 1000) + truck.spent_ml == detour_fuel,
            "Replanning debited a blocked leg or delivered the cargo more than once");
    CampaignState shared(roads, 42);
    shared.request_supply(request);
    shared.request_supply({incoming, base, depot, Resource::supplies, 10});
    shared.advance_ticks(8);
    require(shared.carriers().at(vehicle).position == Point{500'000, 200'000, 30'000} &&
            shared.shipments().at(incoming).delay == LogisticsDelay::capacity &&
            shared.carriers().at(other).position == shared.stockpile_position(base),
            "Opposite-direction carriers did not share a physical road slot");
    shared.set_road_closed(1, true);
    shared.advance_ticks(3);
    require(shared.carriers().at(vehicle).position == Point{500'000, 200'000, 30'000} &&
            shared.shipments().at(outgoing).delay == LogisticsDelay::closed &&
            shared.shipments().at(outgoing).cargo == 10 && shared.carriers().at(vehicle).spent_ml == 400,
            "Closing the bridge moved a carrier or debited fuel during the pause");
    shared.save(midway);
    auto restored_closure = CampaignState::load(midway);
    auto repeated_closure = shared;
    shared.advance_ticks(4);
    for (int i = 0; i < 4; ++i) {
        repeated_closure.advance_ticks(1);
    }
    require(shared.faction_view(west).roads == repeated_closure.faction_view(west).roads,
            "Waiting convoy reports changed between batch and individual ticks");
    shared = CampaignState::load(midway);
    restored_closure.advance_ticks(1);
    require(restored_closure.shipments().at(outgoing).delay == LogisticsDelay::closed &&
            restored_closure.carriers().at(vehicle).position == shared.carriers().at(vehicle).position,
            "The bridge closure or capacity commitment did not survive save/load");
    shared.set_road_closed(1, false);
    shared.advance_ticks(19);
    require(shared.shipments().at(outgoing).status == ShipmentStatus::delivered &&
            shared.shipments().at(incoming).status == ShipmentStatus::delivered &&
            shared.carriers().at(vehicle).fuel == 9 && shared.carriers().at(other).fuel == 9,
            "Bridge reopening did not resume both shipments with one fuel debit per leg");
    std::vector<std::uint64_t> arrivals;
    for (const auto& event : shared.logistics_log()) {
        if (event.kind == LogisticsEventKind::arrived) {
            arrivals.push_back(event.tick);
        }
    }
    require(arrivals == std::vector<std::uint64_t>{19, 29},
            "Directional road grade or slot release timing changed");

    roads.carriers.resize(1);
    roads.carriers[0].fuel = 0;
    CampaignState no_fuel(roads, 42);
    no_fuel.request_supply(request);
    no_fuel.advance_ticks(10);
    require(no_fuel.shipments().at(outgoing).delay == LogisticsDelay::fuel &&
            no_fuel.stockpiles().at(depot).reserved[supplies] == 0,
            "An unaffordable shipment reserved supplies or departed");

    roads.carriers[0].fuel = 10;
    roads.geographic_world->roads.front().direction = Direction::reverse;
    CampaignState one_way(roads, 42);
    one_way.request_supply(request);
    one_way.advance_ticks(1);
    require(one_way.shipments().at(outgoing).delay == LogisticsDelay::unreachable,
            "A road-only carrier drove against the road direction");
    roads.geographic_world->roads.front().direction = Direction::both;
    roads.geographic_world->tile_mm = 500;
    CampaignState fine_raster(roads, 42);
    fine_raster.request_supply(request);
    fine_raster.advance_ticks(1);
    require(fine_raster.shipments().at(outgoing).status == ShipmentStatus::loading,
            "Fine terrain resolution prevented a road-only dispatch");

    scenario.tick_seconds = 300;
    scenario.loading_ticks = scenario.unloading_ticks = 1;
    scenario.carriers.front().position = scenario.stockpiles.front().access_point;
    CampaignState fast(scenario, 42);
    fast.request_supply(request);
    fast.advance_ticks(3);
    require(fast.shipments().at(outgoing).status == ShipmentStatus::unloading &&
            fast.carriers().at(vehicle).position == fast.stockpile_position(base) &&
            std::ranges::count(fast.logistics_log(), LogisticsEventKind::crossed, &LogisticsEvent::kind) == 2,
            "A long tick could not advance through multiple route legs and sector crossings");
}

void test_geographic_mobile_delivery()
{
    using namespace geography;
    constexpr auto west = FactionId::west;
    constexpr UnitId formation{1};
    constexpr CarrierId carrier{1};
    constexpr ShipmentId delivery{1};
    constexpr StockpileId depot{1}, deployed{10};
    constexpr auto supplies = static_cast<std::size_t>(Resource::supplies);
    constexpr auto ammunition = static_cast<std::size_t>(Resource::ammunition);
    constexpr Point pickup{100'000, 100'000, 0};
    constexpr Point start{350'000, 100'000, 0};
    constexpr Point objective{350'000, 850'000, 0};
    const SupplyRequest request{delivery, depot, deployed, Resource::supplies, 5};
    auto scenario = combat_example_scenario(west);
    scenario.tick_seconds = 10;
    scenario.combat_units.front().position = start;
    scenario.equipment_types.front().ground_speed_kph = 5;
    scenario.installations.clear();
    scenario.objectives.clear();
    scenario.victory_objectives.clear();
    scenario.command_objectives.clear();
    scenario.operations.planner_interval_ticks = 0;
    scenario.geographic_world = Map{3, 2, 500'000, 100'000, {}, {}, {}, {}, {}};
    auto& stock = *std::ranges::find(scenario.stockpiles, depot, &Stockpile::id);
    stock.capacity[supplies] = stock.quantity[supplies] = 10;
    stock.access_point = pickup;
    auto& recipient = *std::ranges::find(scenario.stockpiles, deployed, &Stockpile::id);
    recipient.capacity[supplies] = 30;
    scenario.carriers = {{carrier, west, SectorId{0}, 10, 10}};
    scenario.carriers.front().position = pickup;

    CampaignState initial(scenario, 42);
    initial.request_supply(request);
    initial.advance_ticks(2);
    require(initial.shipments().at(delivery).cargo == 5 &&
            initial.stockpile_position(deployed) == start &&
            !initial.stockpiles().at(deployed).access_point,
            "A deployed stockpile did not resolve through its formation");
    auto pursuing = initial;
    pursuing.advance_ticks(1);
    require(pursuing.shipments().at(delivery).journey->destination == start &&
            pursuing.issue_ground_order(west, MissionKind::defend, objective).mission.has_value(),
            "Could not redirect the recipient after dispatch");
    pursuing.advance_ticks(2);
    require(pursuing.stockpile_position(deployed) != start &&
            pursuing.stockpiles().at(deployed).quantity[supplies] == 20 &&
            pursuing.shipments().at(delivery).cargo == 5,
            "Carrier unloaded at the recipient's stale same-sector destination");
    pursuing.advance_ticks(100);
    require(pursuing.shipments().at(delivery).status == ShipmentStatus::delivered &&
            pursuing.carriers().at(carrier).position == objective &&
            pursuing.stockpile_position(deployed) == objective &&
            pursuing.stockpiles().at(deployed).quantity[supplies] == 25 &&
            std::ranges::count(pursuing.logistics_log(), LogisticsEventKind::unloaded, &LogisticsEvent::kind) == 1,
            "Replanning to a moving formation lost or duplicated a delivery");

    auto short_range = scenario;
    short_range.stockpiles.front().access_point = Point{250'000, 100'000, 0};
    short_range.carriers.front().position = short_range.stockpiles.front().access_point;
    short_range.carriers.front().fuel = 1;
    Profile expensive{};
    expensive.terrain[0] = {10'000, 7000};
    short_range.carriers.front().mobility = expensive;
    CampaignState exhausted(short_range, 42);
    exhausted.request_supply(request);
    exhausted.advance_ticks(2);
    require(exhausted.shipments().at(delivery).cargo == 5 &&
            exhausted.issue_ground_order(west, MissionKind::defend, objective).mission.has_value(),
            "Fuel-exhaustion fixture could not start the loaded run");
    exhausted.advance_ticks(100);
    require(exhausted.shipments().at(delivery).delay == LogisticsDelay::fuel &&
            exhausted.shipments().at(delivery).cargo == 5 &&
            exhausted.stockpiles().at(deployed).quantity[supplies] == 20 &&
            exhausted.carriers().at(carrier).position != objective,
            "A carrier without enough fuel followed a new destination or unloaded cargo");

    auto destroyed = initial;
    require(destroyed.lose_equipment(EquipmentId{1}), "Could not destroy the recipient fixture");
    destroyed.advance_ticks(2);
    require(destroyed.combat_units().at(formation).status == GroundStatus::destroyed &&
            destroyed.shipments().at(delivery).status == ShipmentStatus::cancelled &&
            destroyed.shipments().at(delivery).cargo == 5 &&
            destroyed.stockpiles().at(deployed).quantity[supplies] == 20,
            "Destruction discarded loaded cargo or unloaded it to the destroyed recipient");

    scenario.equipment_types.front().ground_speed_kph = 30;
    scenario.equipment.push_back({EquipmentId{10}, EquipmentTypeId{1}, formation});
    scenario.units[1].sector = SectorId{0};
    scenario.units[1].facility.reset();
    scenario.combat_units[1].position = Point{350'000, 400'000, 0};
    std::ranges::find(scenario.stockpiles, StockpileId{11}, &Stockpile::id)->sector = SectorId{0};
    recipient.quantity[ammunition] = 0;
    CampaignState withdrawing(scenario, 42);
    withdrawing.request_supply(request);
    withdrawing.advance_ticks(2);
    require(withdrawing.combat_units().at(formation).status == GroundStatus::withdrawing &&
            withdrawing.shipments().at(delivery).delay == LogisticsDelay::recipient_unavailable &&
            withdrawing.stockpiles().at(depot).reserved[supplies] == 5 &&
            withdrawing.shipments().at(delivery).cargo == 0,
            "Recipient withdrawal did not suspend service and preserve pickup reservations");
    withdrawing.advance_ticks(40);
    require(withdrawing.shipments().at(delivery).status == ShipmentStatus::delivered &&
            withdrawing.carriers().at(carrier).position == withdrawing.stockpile_position(deployed) &&
            withdrawing.stockpiles().at(depot).reserved[supplies] == 0,
            "Shipment did not resume at the formation's withdrawal position");
}

void test_geographic_transport_control_change()
{
    using namespace geography;
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr ShipmentId delivery{1};
    constexpr CarrierId carrier{1};
    constexpr StockpileId depot{1}, recipient{20};
    constexpr SectorId middle{1}, destination{2};
    constexpr Point pickup{200'000, 200'000, 0};
    constexpr Point dropoff{1'200'000, 200'000, 0};

    auto scenario = combat_example_scenario(east);
    scenario.tick_seconds = 10;
    scenario.combat.critical_occupation_seconds = 40;
    scenario.operations.planner_interval_ticks = 0;
    scenario.objectives.clear();
    scenario.installations.clear();
    scenario.victory_objectives.clear();
    scenario.command_objectives.clear();
    std::ranges::find(scenario.initial_sectors, destination, &InitialSectorState::sector)->state.controller = west;
    scenario.units[1].sector = middle;
    scenario.units[1].facility.reset();
    std::ranges::find(scenario.stockpiles, StockpileId{11}, &Stockpile::id)->sector = middle;
    scenario.stockpiles.front().access_point = pickup;
    scenario.stockpiles.push_back({recipient, destination, west, {0, 0, 20}, {}, {}, dropoff});
    scenario.carriers = {{carrier, west, SectorId{0}, 10, 10}};
    scenario.carriers.front().position = pickup;
    Profile road_only{};
    road_only.roads[0] = {5000, 1000};
    scenario.carriers.front().mobility = road_only;
    Map map{3, 2, 500'000, 100'000, {}, {}, {}, {}, {}};
    map.nodes = {{1, pickup}, {2, dropoff}};
    map.roads = {{1, 1, 2, {}}};
    scenario.geographic_world = std::move(map);

    CampaignState campaign(scenario, 42);
    require(campaign.issue_order(east, MissionKind::attack, middle).mission.has_value(),
            "Could not prepare the contested transport corridor");
    campaign.request_supply({delivery, depot, recipient, Resource::fuel, 5});
    campaign.advance_ticks(5);
    const auto stopped = campaign.carriers().at(carrier).position;
    require(campaign.sector_state(middle).controller == east &&
            stopped == Point{350'000, 200'000, 0} &&
            campaign.shipments().at(delivery).cargo == 5,
            "The corridor did not change control during the loaded journey");
    campaign.advance_ticks(2);
    const auto safe_waypoint = campaign.carriers().at(carrier).position;
    require(safe_waypoint == Point{400'000, 200'000, 0} &&
            campaign.carriers().at(carrier).sector == SectorId{0} &&
            campaign.shipments().at(delivery).delay == LogisticsDelay::unsafe &&
            campaign.shipments().at(delivery).cargo == 5,
            "A carrier continued into a newly hostile intermediate sector");
    campaign.advance_ticks(2);
    require(campaign.carriers().at(carrier).position == safe_waypoint,
            "A carrier did not stay at the safe waypoint while the route was blocked");
}

void test_long_geographic_dispatch()
{
    using namespace geography;
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr StockpileId depot{1}, destination{20}, mobile{10};
    constexpr SectorId first{0}, last{127}, eastern_base{16'383};
    constexpr CarrierId carrier{1};
    constexpr UnitId formation{1};
    constexpr ShipmentId shipment{1};
    constexpr Point departure{1'000'000, 1'000'000, 0};
    constexpr Point arrival{255'000'000, 1'000'000, 0};
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);

    auto scenario = combat_example_scenario(west);
    scenario.world = WorldDefinition(128, 128, 2000, std::vector<SectorProperties>(128 * 128));
    scenario.initial_sectors.clear();
    for (std::uint32_t id = 0; id < 128; ++id) {
        scenario.initial_sectors.push_back({SectorId{id}, {west}});
    }
    scenario.initial_sectors.push_back({eastern_base, {east}});
    scenario.tick_seconds = 3600;
    scenario.combat.time_limit_seconds.reset();
    scenario.operations.planner_interval_ticks = 0;
    scenario.objectives.clear();
    scenario.installations.clear();
    scenario.victory_objectives.clear();
    scenario.command_objectives.clear();
    scenario.units[1].sector = eastern_base;
    scenario.units[1].facility.reset();
    std::ranges::find(scenario.stockpiles, StockpileId{11}, &Stockpile::id)->sector = eastern_base;
    scenario.stockpiles.push_back({destination, last, west, {0, 0, 40}});
    auto& deployed = *std::ranges::find(scenario.stockpiles, mobile, &Stockpile::id);
    deployed.quantity[fuel] = deployed.capacity[fuel] = 300;

    Profile road_only{};
    road_only.roads[0] = {10'000, 500};
    scenario.equipment_types.front().ground_speed_kph = 36;
    scenario.equipment_types.front().mobility = road_only;
    scenario.carriers = {{carrier, west, first, 10, 300}};
    scenario.carriers.front().mobility = road_only;
    Map map{128, 128, 2'000'000, 100'000, {}, {}, {}, {}, {}};
    map.nodes = {{1, departure}, {2, arrival}};
    map.roads = {{1, 1, 2, {}}};
    scenario.geographic_world = std::move(map);

    CampaignState campaign(scenario, 42);
    require(campaign.issue_order(west, MissionKind::defend, last).mission.has_value(),
            "A distant friendly objective could not schedule a ground journey");
    campaign.request_supply({shipment, depot, destination, Resource::fuel, 5});
    campaign.advance_ticks(4);
    require(campaign.shipments().at(shipment).journey && campaign.shipments().at(shipment).cargo == 5 &&
            campaign.carriers().at(carrier).position != departure && campaign.carriers().at(carrier).position != arrival,
            "A distant delivery route could not dispatch and make partial progress");
    const CheckpointFiles files;
    const auto checkpoint = files.directory / "regional.ows";
    campaign.save(checkpoint);
    auto restored = CampaignState::load(checkpoint);
    const auto verify = [&](CampaignState& state) {
        state.advance_ticks(10);
        require(state.shipments().at(shipment).status == ShipmentStatus::delivered &&
                state.stockpiles().at(destination).quantity[fuel] == 5 &&
                state.carriers().at(carrier).position == arrival &&
                state.carriers().at(carrier).fuel == 173 &&
                state.combat_units().at(formation).position == arrival &&
                state.stockpiles().at(mobile).quantity[fuel] == 173,
                "Regional journeys did not reach the exact destination with their planned fuel costs");
    };
    verify(campaign);
    verify(restored);
    require(std::ranges::equal(campaign.logistics_log(), restored.logistics_log()) &&
            std::ranges::equal(campaign.observer_battle_log(), restored.observer_battle_log()),
            "Resuming regional routes changed their ordered events");
}

void test_independent_geographic_scenarios()
{
    const auto* fixtures = std::getenv("VALLEY_FIXTURES");
    const auto directory = std::filesystem::path(fixtures ? fixtures : "examples/connecticut-valley");
    const auto world = read_geographic_world(directory / "valley-world.owgeo");
    require(world.grid.width() == 16 && world.grid.height() == 16 && world.map.tiles.size() == 25600 &&
            world.map.roads.size() == 9342 && world.map.waterways.size() == 3948 &&
            world.map.site_features.size() == 1761 && world.map.provenance.find("west_base") == std::string::npos,
            "Independent world omitted physical data or retained a military deployment");

    const auto same_geography = [](const geography::Map& a, const geography::Map& b) {
        const auto node = [](const auto& n) { return std::tie(n.id, n.position, n.osm_node_id); };
        const auto road = [](const auto& r) {
            return std::tie(r.id, r.from, r.to, r.waypoints, r.surface, r.direction, r.crossing, r.speed_limit_mm_s,
                            r.interior_access, r.interior_access_points, r.slots, r.osm_way_id);
        };
        const auto tile = [](const auto& t) { return std::tie(t.first, t.second.cover, t.second.elevation); };
        const auto river = [](const auto& w) { return std::tie(w.from, w.to); };
        const auto site = [](const auto& f) {
            return std::tie(f.kind, f.element, f.source_id, f.position, f.length_mm, f.width_mm, f.area_m2,
                            f.name, f.surface, f.geometry_wkt);
        };
        return std::tie(a.columns, a.rows, a.sector_mm, a.tile_mm, a.default_tile.cover, a.default_tile.elevation,
                        a.provenance) ==
               std::tie(b.columns, b.rows, b.sector_mm, b.tile_mm, b.default_tile.cover, b.default_tile.elevation,
                        b.provenance) &&
               std::ranges::equal(a.nodes, b.nodes, {}, node, node) &&
               std::ranges::equal(a.roads, b.roads, {}, road, road) &&
               std::ranges::equal(a.tiles, b.tiles, {}, tile, tile) &&
               std::ranges::equal(a.waterways, b.waterways, {}, river, river) &&
               std::ranges::equal(a.site_features, b.site_features, {}, site, site);
    };
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    CampaignState demo(read_geographic_scenario(directory / "valley-demo.owscenario"), 42);
    CampaignState autonomous(read_geographic_scenario(directory / "valley-autonomous.owscenario"), 42);
    require(same_geography(world.map, *demo.geographic_world()) &&
            same_geography(world.map, *autonomous.geographic_world()), "Scenarios resolved different physical maps");
    require(demo.units().at(UnitId{1}).faction == west && autonomous.units().at(UnitId{1}).faction == east &&
            demo.sector_state(SectorId{120}).controller == west &&
            autonomous.sector_state(SectorId{120}).controller == east &&
            demo.faction_view(west).objectives.front().sector == SectorId{122} &&
            autonomous.faction_view(west).objectives.front().sector == SectorId{175},
            "Scenario control, forces or objectives were coupled to the map");
    for (const auto* campaign : {&demo, &autonomous}) {
        for (const auto& [id, unit] : campaign->combat_units()) {
            const auto sector = *world.map.sector(*unit.position);
            require(campaign->units().at(id).sector == static_cast<SectorId>(sector) &&
                    campaign->stockpiles().at(unit.stores).sector == static_cast<SectorId>(sector),
                    "Initial unit sector and mobile stock were not derived from their deployment point");
        }
        for (const auto& [id, carrier] : campaign->carriers())
            require(world.map.sector(*carrier.position) == static_cast<std::uint32_t>(carrier.sector),
                    "Initial carrier sector was not derived from its position");
        for (const auto& [id, stock] : campaign->stockpiles())
            require(world.map.sector(campaign->stockpile_position(id)) == static_cast<std::uint32_t>(stock.sector),
                    "Initial stock sector was not derived from its access point");
    }
    autonomous.advance_ticks(40);
    for (const auto faction : {west, east}) {
        require(std::ranges::any_of(autonomous.faction_view(faction).missions, [](const Mission& mission) {
            return !mission.manual && mission.status == MissionStatus::completed;
        }), "An autonomous faction failed to complete a mission on imported geography");
    }
    const auto road = world.map.roads.front().id;
    autonomous.set_road_closed(road, true);
    require(autonomous.closed_roads().contains(road) && demo.closed_roads().empty() &&
            same_geography(world.map, *autonomous.geographic_world()) &&
            same_geography(world.map, *demo.geographic_world()) &&
            same_geography(world.map, read_geographic_world(directory / "valley-world.owgeo").map),
            "Campaign changes reached another scenario or its geographic source");
}

void test_legacy_road_checkpoint()
{
    // Written by bfa05eb from a 2x1 geographic campaign with one bridge closed at tick zero.
    const auto* configured = std::getenv("LEGACY_ROAD_CHECKPOINT");
    const auto path = configured ? configured : "tests/fixtures/closed-bridge-schema4.ows";
    auto migrated = CampaignState::load(path);
    require(migrated.clock().tick() == 3 && migrated.geographic_world()->roads.front().id == 17 &&
            migrated.closed_roads().contains(17) && migrated.faction_view(FactionId::west).roads.empty() &&
            migrated.faction_view(FactionId::east).roads.empty(),
            "Schema 4 bridge flag did not migrate to campaign state without inventing observations");
    const CheckpointFiles files;
    const auto rewritten = files.directory / "rewritten.ows";
    migrated.save(rewritten);
    require(CampaignState::load(rewritten).closed_roads() == migrated.closed_roads(),
            "Migrated closure did not survive a new-format checkpoint");
}

void test_legacy_defend_checkpoint()
{
    // Written by f263b1a with a manual Defend to (300000,250000,0) in the combat demo.
    const auto* configured = std::getenv("LEGACY_DEFEND_CHECKPOINT");
    auto campaign = CampaignState::load(configured ? configured : "tests/fixtures/defend-schema7.ows");
    constexpr auto west = FactionId::west;
    constexpr geography::Point destination{300'000, 250'000, 0};
    const auto scheduled = campaign.faction_view(west).missions.front();
    require(scheduled.kind == MissionKind::defend && scheduled.status == MissionStatus::scheduled &&
            scheduled.ground_destination == destination && scheduled.ground_unit == UnitId{1},
            "Schema 7 Defend lost its assignment, geographic point or original order kind");
    const CheckpointFiles files;
    const auto rewritten = files.directory / "defend.ows";
    campaign.save(rewritten);
    auto resumed = CampaignState::load(rewritten);
    for (auto* state : {&campaign, &resumed}) {
        state->advance_ticks(1);
        require(state->faction_view(west).missions.front().status == MissionStatus::completed &&
                state->combat_units().at(UnitId{1}).position == destination &&
                state->equipment().at(EquipmentId{1}).status == EquipmentStatus::available,
                "Migrated Defend did not complete or was converted into a standing Hold");
    }
    require(campaign.faction_view(west).missions == resumed.faction_view(west).missions,
            "Defend diverged after rewriting a schema 7 checkpoint");
}

void test_checkpoints()
{
    const CheckpointFiles files;
    const auto checkpoint = files.directory / "campaign.ows";
    const auto comparison = files.directory / "comparison.ows";
    const auto bytes = [](const auto& path) {
        std::ifstream input(path, std::ios::binary);
        require(static_cast<bool>(input), "Expected a saved checkpoint");
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto equal_saves = [&](const CampaignState& left, const CampaignState& right) {
        left.save(checkpoint);
        right.save(comparison);
        require(bytes(checkpoint) == bytes(comparison), "Checkpoint state or journal changed after resuming");
    };
    const auto round_trip = [&](const CampaignState& state) {
        state.save(checkpoint);
        auto restored = CampaignState::load(checkpoint);
        equal_saves(state, restored);
        return restored;
    };

    auto battle = CampaignState(combat_example_scenario(), 42);
    battle.advance_ticks(5);
    require(battle.sector_state(SectorId{1}).occupier == FactionId::west &&
            !battle.observer_battle_log().empty(), "Checkpoint fixture did not reach occupation after combat");
    auto restored_battle = round_trip(battle);
    auto uninterrupted = CampaignState(combat_example_scenario(), 42);
    uninterrupted.advance_ticks(30);
    restored_battle.advance_ticks(25);
    require(restored_battle.war_ended(), "Restored occupation did not finish the campaign");
    equal_saves(uninterrupted, restored_battle);
    auto ended = round_trip(restored_battle);
    ended.advance_ticks(100);
    equal_saves(ended, restored_battle);

    auto service = maintenance_example_campaign(42);
    service.advance_ticks(7);
    require(service.equipment().at(EquipmentId{1}).job->work_seconds == 60 &&
            service.equipment().at(EquipmentId{11}).reinforcement_eta == 8,
            "Checkpoint fixture did not reach active repairs and inbound reinforcements");
    auto restored_service = round_trip(service);
    auto continuous_service = maintenance_example_campaign(42);
    continuous_service.advance_ticks(20);
    restored_service.advance_ticks(13);
    equal_saves(continuous_service, restored_service);

    auto reconnaissance = operations_example_campaign(42, FactionId::west);
    require(reconnaissance.issue_order(FactionId::west, MissionKind::recon, SectorId{2}).mission.has_value(),
            "Checkpoint fixture did not reserve a mission");
    auto restored_recon = round_trip(reconnaissance);
    reconnaissance.advance_ticks(25);
    restored_recon.advance_ticks(25);
    equal_saves(reconnaissance, restored_recon);
    require(restored_recon.player_faction() == FactionId::west &&
            !restored_recon.faction_view(FactionId::west).contacts.empty(),
            "Checkpoint lost player authority or reconnaissance reports");

    auto random = CampaignState(example_scenario(), 5489u);
    std::mt19937 reference(5489u);
    for (int i = 0; i < 701; ++i) {
        require(random.next_random() == reference(), "Simulation RNG diverged from MT19937");
    }
    auto restored_random = round_trip(random);
    for (int i = 0; i < 2500; ++i) {
        require(restored_random.next_random() == reference(), "Restored RNG diverged from MT19937");
    }

    const auto blocked = files.directory / "existing-directory";
    std::filesystem::create_directory(blocked);
    require_throws<std::filesystem::filesystem_error>([&] { random.save(blocked); },
                                                     "Save replaced an existing directory");
    require(std::filesystem::is_directory(blocked) && !std::filesystem::exists(blocked.string() + ".tmp.0"),
            "Failed save damaged the destination or left staging files");
}

void test_depleted_withdrawal()
{
    auto scenario = combat_example_scenario();
    scenario.operations.planner_interval_ticks = 0;
    for (std::size_t i = 0; i < scenario.units.size(); ++i) {
        scenario.units[i].sector = SectorId{1};
        scenario.units[i].facility.reset();
        const auto stores = scenario.combat_units[i].stores;
        auto stock = std::ranges::find(scenario.stockpiles, stores, &Stockpile::id);
        stock->sector = SectorId{1};
        stock->quantity[static_cast<std::size_t>(Resource::ammunition)] = 0;
    }
    CampaignState campaign(scenario, 42);
    campaign.advance_ticks(2);
    require(campaign.units().at(UnitId{1}).sector == SectorId{0} &&
            campaign.units().at(UnitId{2}).sector == SectorId{2} &&
            std::ranges::none_of(campaign.observer_battle_log(), [](const auto& event) {
                return event.reason == "ammunition expended";
            }), "Out-of-ammunition units remained pinned despite open withdrawal routes");

    scenario.world = WorldDefinition(3, 2, 2000, std::vector<SectorProperties>(6));
    CampaignState withdrawal(scenario, 42);
    withdrawal.advance_ticks(2);
    require(withdrawal.units().at(UnitId{1}).sector == SectorId{1} &&
            withdrawal.ground_movement(UnitId{1}) == GroundMovement{SectorId{0}, 25, 180},
            "Withdrawal did not use accumulated ground movement");
    withdrawal.advance_ticks(3);
    require(withdrawal.units().at(UnitId{1}).sector == SectorId{0} &&
            !withdrawal.ground_movement(UnitId{1}), "Withdrawal did not complete after four travel ticks");
}

}  // namespace

int main()
{
    test_clock();
    test_topology();
    test_initialization();
    test_reproducibility();
    test_economy();
    test_logistics();
    test_recurring_supply();
    test_realtime_pacing();
    test_forces_and_maintenance();
    test_operations_and_intelligence();
    test_command_session();
    test_explicit_formation_assignment();
    test_recon_revisit();
    test_mission_preparation_reservation();
    test_destroyed_reinforcement();
    test_capture_cancels_preparation();
    test_combat_and_territory();
    test_war_endings();
    test_long_campaign();
    test_strategic_command();
    test_procedural_campaign();
    test_large_procedural_campaigns();
    test_ground_movement();
    test_ground_position_orders();
    test_ground_patrol_waypoints();
    test_ground_reconnaissance();
    test_geographic_ground_operations();
    test_geographic_transport();
    test_geographic_mobile_delivery();
    test_geographic_transport_control_change();
    test_depleted_withdrawal();
    test_checkpoints();
    test_legacy_road_checkpoint();
    test_legacy_defend_checkpoint();
    test_independent_geographic_scenarios();
    test_long_geographic_dispatch();
}
