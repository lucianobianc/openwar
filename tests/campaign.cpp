#include "campaign.hpp"
#include "realtime.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
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
            first.economy_ledger().back() == EconomyEntry{30, west, std::nullopt, demand, {}},
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
                5, order, LogisticsEventKind::arrived, 40, scenario.stockpiles.back().sector},
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

}  // namespace

int main()
{
    test_clock();
    test_topology();
    test_initialization();
    test_reproducibility();
    test_economy();
    test_logistics();
    test_realtime_pacing();
    test_forces_and_maintenance();
}
