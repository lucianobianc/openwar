#include "command.hpp"
#include "viewer_model.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <tuple>

namespace {

using namespace openwar;
using namespace openwar::viewer;

void require(bool condition, std::string_view description)
{
    if (!condition) {
        std::cerr << description << '\n';
        std::exit(1);
    }
}

bool has_marker(const Snapshot& snapshot, MarkerKind kind, std::string_view name)
{
    return std::ranges::any_of(snapshot.markers, [&](const Marker& marker) {
        return marker.kind == kind && marker.label == name;
    });
}

const Marker& marker_at(const Snapshot& snapshot, MarkerKey key)
{
    const auto marker = std::ranges::find(snapshot.markers, key, &Marker::key);
    require(marker != snapshot.markers.end(), "Expected geographic marker is missing");
    return *marker;
}

bool has_inspection(const Marker& marker, std::string_view text)
{
    return std::ranges::any_of(marker.inspection, [&](const std::string& line) {
        return line.find(text) != std::string::npos;
    });
}

void require_stored_route(const std::optional<RouteTrace>& trace, const geography::Route& route,
                          const geography::Progress& progress, geography::Point destination)
{
    require(trace && trace->vertices.size() == route.legs.size() + 1 &&
            trace->vertices.front() == route.origin && trace->position == progress.position &&
            trace->leg == progress.leg && trace->elapsed_us == progress.elapsed_us &&
            trace->duration_us == route.duration_us && trace->destination == destination,
            "Viewer route did not reflect the stored journey and its exact progress");
    for (std::size_t i = 0; i < route.legs.size(); ++i) {
        require(trace->vertices[i] == route.legs[i].from && trace->vertices[i + 1] == route.legs[i].to,
                "Viewer route invented or omitted a leg");
    }
}

std::string read_bytes(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    require(static_cast<bool>(input), "Missing checkpoint for byte comparison");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_geographic_snapshot()
{
    const auto path = std::getenv("OWGEO_FIXTURE");
    require(path != nullptr, "Geographic viewer test requires OWGEO_FIXTURE");
    auto scenario = read_geographic_scenario(path);
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr MarkerKey formation{MarkerKind::unit, west, 1};
    constexpr MarkerKey carrier{MarkerKind::carrier, west, 1};
    constexpr MarkerKey installation{MarkerKind::installation, west, 1};
    const auto target = std::ranges::find(scenario.objectives, west, &Objective::faction)->sector;
    scenario.operations.player = west;
    CampaignState campaign(scenario, 0);
    const auto road = campaign.geographic_world()->roads.front().id;
    campaign.set_road_closed(road, true);
    require(read_snapshot(campaign, {}).roads.at(road).closed && read_snapshot(campaign, west).roads.empty() &&
            read_snapshot(campaign, east).roads.empty(), "Viewer displayed a hidden closure in a faction perspective");
    campaign.set_road_closed(road, false);
    const auto initial = read_snapshot(campaign, west);
    const auto start = marker_at(initial, formation).position;
    const auto depot = marker_at(initial, installation);
    require(start == campaign.combat_units().at(UnitId{1}).position &&
            marker_at(initial, carrier).position == campaign.carriers().at(CarrierId{1}).position &&
            depot.position == campaign.stockpile_position(StockpileId{3}) && !depot.estimated &&
            start != initial.sector_center(marker_at(initial, formation).sector),
            "Geographic markers were placed at sector centres instead of authoritative positions");
    const auto destination = scenario.ground_destinations.at(target);
    CommandSession commander(campaign, west);
    CommandSession observer(campaign);
    CommandSession wrong_faction(campaign, east);
    require(!observer.preview(MissionKind::defend, destination).formation &&
            !wrong_faction.preview(MissionKind::defend, destination).formation,
            "An unauthorized session obtained an order preview");

    const auto before = std::filesystem::path("viewer-before-draft.ows");
    const auto after = std::filesystem::path("viewer-after-draft.ows");
    campaign.save(before);
    const auto preview = commander.preview(MissionKind::defend, destination);
    {
        const auto zero = commander.preview(MissionKind::defend, start);
        require(zero.formation && zero.route && zero.route->duration_us == 0 &&
                zero.route->fuel_ml == 0 && !zero.abstract_duration_seconds,
                "A valid zero-time ground route was treated as unavailable");
        const auto denied = commander.preview(MissionKind::defend, geography::Point{-1, 0, 0});
        require(!denied.formation && !denied.route && denied.reason == "ground destination is out of bounds",
                "Preview accepted an invalid geographic position");
    } // A discarded local draft leaves the campaign untouched.
    campaign.save(after);
    require(read_bytes(before) == read_bytes(after), "Previewing and discarding an order changed campaign state");
    std::filesystem::remove(before);
    std::filesystem::remove(after);
    require(preview.tick == 0 && preview.formation == UnitId{1} && preview.route &&
            preview.route->destination == destination && preview.route->duration_us > 0 &&
            preview.launch_reservation == ResourceAmounts{},
            "Ground preview omitted its candidate, route estimate or fuel semantics");

    const auto engine_plan = campaign.preview_order(west, MissionKind::defend, destination);
    const auto& legs = engine_plan.candidate->ground_journey->route.legs;
    const auto used_road = std::ranges::find_if(legs, [](const auto& leg) { return leg.road.has_value(); });
    require(used_road != legs.end() && used_road == legs.begin(), "Preview fixture did not start on an authored road");
    CampaignState hidden(scenario, 0);
    hidden.set_road_closed(*used_road->road, true);
    CommandSession blocked(hidden, west);
    const auto obscured = blocked.preview(MissionKind::defend, destination);
    require(obscured.formation == preview.formation && obscured.reason == preview.reason &&
            obscured.route == preview.route && blocked.snapshot().roads.empty(),
            "Hidden road closure changed the estimated route, ETA or refusal");
    const auto attempted = blocked.submit(MissionKind::defend, destination);
    require(attempted.mission.has_value(), "The committed order leaked the hidden road condition");
    hidden.advance_ticks(1);
    const auto interrupted = blocked.snapshot();
    const auto& failed = marker_at(interrupted, {MarkerKind::mission, west, static_cast<std::uint32_t>(*attempted.mission)});
    require(failed.mission_status == MissionStatus::failed && has_inspection(failed, "route obstructed") &&
            failed.journey && failed.journey->elapsed_us < failed.journey->duration_us &&
            interrupted.roads.at(*used_road->road).closed && !read_snapshot(hidden, east).roads.contains(*used_road->road),
            "Mission history omitted an observed obstruction or disclosed it to the other faction");

    CampaignState changed(scenario, 0);
    CommandSession stale(changed, west);
    require(stale.preview(MissionKind::defend, destination).formation && changed.lose_equipment(EquipmentId{1}),
            "Could not invalidate a feasible draft");
    const auto revalidated = stale.submit(MissionKind::defend, destination);
    require(!revalidated.mission && stale.snapshot().mission_markers.empty(),
            "Submission reused a stale preview after equipment was lost");

    const auto order = commander.submit(MissionKind::defend, destination);
    require(order.mission.has_value(), "Could not start geographic viewer fixture movement");
    const MarkerKey mission_key{MarkerKind::mission, west, static_cast<std::uint32_t>(*order.mission)};
    const auto scheduled = commander.snapshot();
    require(has_inspection(marker_at(scheduled, formation), "defend #") &&
            has_inspection(marker_at(scheduled, formation), "scheduled"),
            "Formation inspection omitted the engine-assigned mission");
    const auto& planned = marker_at(scheduled, mission_key);
    require(scheduled.mission_markers.size() == 1 && planned.mission_status == MissionStatus::scheduled &&
            planned.position == destination && planned.journey == preview.route &&
            has_inspection(planned, "Committed equipment") && has_inspection(planned, "Travel ETA "),
            "Scheduled mission omitted the committed route or reservations");
    campaign.advance_ticks(5);
    const auto moving = read_snapshot(campaign, {});
    const auto& moving_unit = marker_at(moving, formation);
    const auto& moving_carrier = marker_at(moving, carrier);
    require(moving_unit.position != start && moving_unit.position == campaign.combat_units().at(UnitId{1}).position &&
            moving_carrier.position != start && moving_carrier.position == campaign.carriers().at(CarrierId{1}).position &&
            !moving_unit.movement.empty() && !moving_carrier.movement.empty() &&
            has_inspection(moving_carrier, "Ground transport") && has_inspection(moving_carrier, "Shipment #") &&
            has_inspection(moving_carrier, "Cargo") && has_inspection(moving_carrier, "stockpile #") &&
            has_inspection(moving_carrier, "L fuel") && has_inspection(depot, "Production installation"),
            "Viewer missed in-transit coordinates or movement status");
    const auto& active = marker_at(moving, mission_key);
    const auto view = campaign.faction_view(west);
    const auto& journey = *view.missions.front().ground_journey;
    require(active.mission_status == MissionStatus::in_progress && active.journey && active.journey->elapsed_us > 0 &&
            active.journey->position == moving_unit.position && has_inspection(active, "ground operation started"),
            "In-progress mission did not show its authoritative progress or events");
    require_stored_route(active.journey, journey.route, journey.progress, destination);
    const auto& shipment = campaign.shipments().at(*campaign.carriers().at(CarrierId{1}).shipment);
    require(shipment.journey.has_value(), "Carrier fixture did not retain an active journey");
    require_stored_route(moving_carrier.journey, shipment.journey->route, shipment.journey->progress,
                         shipment.journey->destination);

    for (const auto perspective : {west, east}) {
        const auto restricted = read_snapshot(campaign, perspective);
        for (const auto& marker : restricted.markers) {
            require(!marker.faction || marker.faction == perspective,
                    "Faction viewer exposed opposing carrier, installation or formation details");
            if (marker.kind == MarkerKind::contact || (marker.kind == MarkerKind::installation && !marker.faction)) {
                require(marker.estimated && marker.position == restricted.sector_center(marker.sector),
                        "An intelligence report exposed an authoritative geographic position");
            }
        }
    }

    const auto checkpoint = std::filesystem::path("viewer-active-mission.ows");
    campaign.save(checkpoint);
    auto resumed = CampaignState::load(checkpoint);
    require(marker_at(read_snapshot(resumed, west), mission_key).journey == marker_at(commander.snapshot(), mission_key).journey,
            "Checkpoint changed the displayed route or progress");
    std::filesystem::remove(checkpoint);
    resumed.advance_ticks(45);
    campaign.advance_ticks(45);
    const auto terminal = commander.snapshot();
    const auto& completed = marker_at(terminal, mission_key);
    require(completed.mission_status == MissionStatus::completed && has_inspection(completed, "ground objective secured") &&
            read_snapshot(resumed, west).mission_markers == terminal.mission_markers &&
            marker_at(read_snapshot(resumed, west), mission_key).journey == completed.journey &&
            std::ranges::equal(resumed.observer_operations_log(), campaign.observer_operations_log()),
            "Completed mission was unavailable or replay after checkpoint diverged");


}

void test_abstract_mission_inspection()
{
    constexpr auto west = FactionId::west;
    constexpr SectorId objective{2};
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    const auto kind = MissionKind::recon;
    auto campaign = operations_example_campaign(42, west);
    CommandSession session(campaign, west);
    const auto planned = session.preview(kind, objective);
    require(planned.formation && !planned.route && planned.abstract_duration_seconds &&
            planned.launch_reservation[fuel] == 10 && campaign.stockpiles().at(StockpileId{1}).reserved[fuel] == 0,
            "Abstract preview fabricated travel geometry or mutated its launch reservation");
    const auto order = session.submit(kind, objective);
    require(order.mission.has_value(), "Could not commit an abstract mission");
    const MarkerKey key{MarkerKind::mission, west, static_cast<std::uint32_t>(*order.mission)};
    const auto scheduled = session.snapshot();
    const auto& entry = marker_at(scheduled, key);
    require(entry.estimated && !entry.journey && entry.position == scheduled.sector_center(objective) &&
            has_inspection(entry, "Fuel reserved 5 L") && has_inspection(entry, "Flight route and travel ETA unavailable") &&
            campaign.stockpiles().at(StockpileId{1}).reserved[fuel] == planned.launch_reservation[fuel],
            "Abstract mission inspection lost the distinction between launch fuel, preparation and travel time");
    require(session.cancel(west, *order.mission).cancelled, "Could not cancel the scheduled abstract mission");
    const auto history = session.snapshot();
    const auto& cancelled = marker_at(history, key);
    require(cancelled.mission_status == MissionStatus::cancelled && has_inspection(cancelled, "player order cancelled") &&
            !has_inspection(cancelled, "Fuel reserved") && campaign.stockpiles().at(StockpileId{1}).reserved[fuel] == 0,
            "Cancelled mission lost its history or retained a reservation");
}

void test_military_identity()
{
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    constexpr auto neutral = FactionId::neutral;
    require(relative_affiliation(west, west) == Affiliation::friendly &&
            relative_affiliation(east, east) == Affiliation::friendly &&
            relative_affiliation(west, east) == Affiliation::hostile &&
            relative_affiliation(east, west) == Affiliation::hostile &&
            relative_affiliation(neutral, west) == Affiliation::neutral &&
            relative_affiliation({}, west) == Affiliation::unknown &&
            relative_affiliation(west, {}) == Affiliation::observer,
            "Symbol affiliation did not follow the selected perspective");

    auto scenario = combat_example_scenario(west);
    constexpr MilitaryIdentity battalion{MilitaryDomain::land, MilitaryFunction::mechanized_infantry,
                                        MilitaryEchelon::battalion};
    constexpr MilitaryIdentity platoon{MilitaryDomain::land, MilitaryFunction::reconnaissance,
                                       MilitaryEchelon::platoon};
    scenario.units.front().identity = battalion;
    scenario.units.push_back({UnitId{3}, "Scout platoon", west, SectorId{0}, UnitId{1}, FacilityId{1}, platoon});
    auto support = scenario.equipment_types.front();
    support.id = EquipmentTypeId{3};
    support.name = "Support trucks";
    support.category = EquipmentCategory::transport;
    support.combat_power = 0;
    support.can_occupy = false;
    scenario.equipment_types.push_back(support);
    scenario.equipment.push_back({EquipmentId{4}, support.id, UnitId{1}});
    scenario.equipment.push_back({EquipmentId{5}, support.id, UnitId{1}, "Reserve"});
    CampaignState campaign(scenario, 42);
    require(campaign.damage_equipment(EquipmentId{4}, 60) && campaign.lose_equipment(EquipmentId{1}),
            "Could not exercise damage and loss in the mixed formation");
    constexpr MarkerKey mixed{MarkerKind::unit, west, 1};
    const auto frame = read_snapshot(campaign, west);
    const auto& marker = marker_at(frame, mixed);
    require(marker.symbol->identity == battalion && marker.symbol->affiliation == Affiliation::friendly &&
            has_inspection(marker, "mechanized infantry") && has_inspection(marker, "Echelon battalion") &&
            has_inspection(marker, "Subordinate #3 / Scout platoon") &&
            has_inspection(marker, "Heavy game vehicle") && has_inspection(marker, "0 damaged / 1 lost") &&
            has_inspection(marker, "Support trucks") && has_inspection(marker, "2 unavailable") &&
            has_inspection(marker, "1 damaged / 0 lost") && has_inspection(marker, "supplies 20"),
            "Mixed formation lost its declared identity, hierarchy, equipment counts or supply information");
    require(std::ranges::count(frame.markers, mixed, &Marker::key) == 1 &&
            marker_at(frame, {MarkerKind::unit, west, 3}).symbol->identity == platoon &&
            has_inspection(marker_at(frame, {MarkerKind::unit, west, 3}), "Parent #1 / West field group"),
            "A mixed formation emitted equipment symbols or lost its parent relationship");
    const auto restricted = read_snapshot(campaign, east);
    require(std::ranges::none_of(restricted.markers, [west](const Marker& object) {
                return object.faction == west || has_inspection(object, "Support trucks");
            }), "Enemy view exposed military identity or inventory");

    const auto legacy_path = std::getenv("LEGACY_IDENTITY_CHECKPOINT");
    require(legacy_path != nullptr, "Military identity migration test requires a schema 5 checkpoint");
    auto legacy = CampaignState::load(legacy_path);
    const auto migrated = read_snapshot(legacy, west);
    require(marker_at(migrated, mixed).symbol->identity == MilitaryIdentity{} &&
            has_inspection(marker_at(migrated, mixed), "Echelon unspecified"),
            "A legacy checkpoint should leave military identity unspecified");
    CampaignState fresh(combat_example_scenario(), 42);
    legacy.advance_ticks(10);
    fresh.advance_ticks(10);
    require(legacy.war_result() == fresh.war_result() &&
            std::ranges::equal(legacy.observer_operations_log(), fresh.observer_operations_log()) &&
            std::ranges::equal(legacy.observer_battle_log(), fresh.observer_battle_log()),
            "A schema 5 checkpoint resumed with different simulation behavior");

    const auto valley_path = std::filesystem::path(std::getenv("VALLEY_FIXTURES")) / "valley-demo.owscenario";
    const auto valley = read_geographic_scenario(valley_path, west);
    constexpr MilitaryIdentity company{MilitaryDomain::land, MilitaryFunction::armor, MilitaryEchelon::company};
    require(valley.units.front().identity == company, "OWSCENARIO 1 unit identity was not read");
    {
        const auto path = std::filesystem::path("viewer-identity-parent.owscenario");
        std::ifstream source(valley_path);
        std::string line;
        std::getline(source, line);
        std::getline(source, line);
        std::ofstream authored(path);
        authored << "OWSCENARIO 1\nworld "
                 << std::quoted(std::filesystem::absolute(valley_path.parent_path() / "valley-world.owgeo").string())
                 << '\n' << source.rdbuf()
                 << "\nunit 3 WEST west_base 0 1 80 \"Scout platoon\"\n"
                 << "identity 3 land reconnaissance platoon\nparent 3 1\n";
        authored.close();
        const auto hierarchy = read_geographic_scenario(path, west);
        std::filesystem::remove(path);
        require(hierarchy.units.back().parent == UnitId{1} && hierarchy.units.back().identity == platoon,
                "OWSCENARIO 1 failed to preserve the declared command relationship");
    }
    auto plain_valley = valley;
    for (auto& unit : plain_valley.units) {
        unit.identity = {};
    }
    CampaignState declared(valley, 42);
    CampaignState unspecified(plain_valley, 42);
    const auto objective = valley.objectives.front().sector;
    require(declared.issue_order(west, MissionKind::defend, objective).mission ==
            unspecified.issue_order(west, MissionKind::defend, objective).mission,
            "Declared identity changed the order assignment");
    declared.advance_ticks(2);
    unspecified.advance_ticks(2);
    const auto checkpoint = std::filesystem::path("viewer-identity-roundtrip.ows");
    declared.save(checkpoint);
    auto resumed = CampaignState::load(checkpoint);
    std::filesystem::remove(checkpoint);
    require(resumed.units().at(UnitId{1}).identity == company, "Checkpoint lost declared military identity");
    resumed.advance_ticks(40);
    declared.advance_ticks(40);
    unspecified.advance_ticks(40);
    for (const auto* other : {&resumed, &unspecified}) {
        require(other->war_result() == declared.war_result() && other->front_line() == declared.front_line() &&
                std::ranges::equal(other->observer_operations_log(), declared.observer_operations_log()) &&
                std::ranges::equal(other->observer_battle_log(), declared.observer_battle_log()) &&
                std::ranges::equal(other->logistics_log(), declared.logistics_log()) &&
                std::ranges::equal(other->forces_log(), declared.forces_log()),
                "Declared military identity or a resumed checkpoint changed campaign behavior");
    }
}

}  // namespace

int main()
{
    test_geographic_snapshot();
    test_abstract_mission_inspection();
    test_military_identity();
    CampaignState economy(example_scenario(), 42);
    economy.advance_ticks(5);
    const auto economy_observer = read_snapshot(economy, {});
    const auto economy_west = read_snapshot(economy, FactionId::west);
    require(economy_observer.tick == 5 && has_marker(economy_observer, MarkerKind::installation, "Refinery") &&
            economy_observer.tiles[0].controller == FactionId::west &&
            !economy_west.tiles[0].controller && economy_west.events.empty(),
            "Economy-only campaign should render without an intelligence store");

    CampaignState campaign(combat_example_scenario(), 42);
    const auto observer = read_snapshot(campaign, {});
    const auto west = read_snapshot(campaign, FactionId::west);
    const auto east = read_snapshot(campaign, FactionId::east);
    require(observer.width == campaign.world().width() && observer.height == campaign.world().height() &&
            observer.tiles.size() == campaign.world().sectors().size() &&
            observer.sector_size_m == campaign.world().sector_size_m() && observer.tick == 0,
            "Viewer geometry did not come from the world");
    require(observer.tiles[2].controller == FactionId::east && !west.tiles[2].controller &&
            observer.tiles[0].controller == FactionId::west && !east.tiles[0].controller,
            "A faction snapshot exposed an unreported owner");
    require(has_marker(observer, MarkerKind::unit, "East field group") &&
            has_marker(west, MarkerKind::unit, "West field group") &&
            !has_marker(west, MarkerKind::unit, "East field group") &&
            !has_marker(east, MarkerKind::unit, "West field group"),
            "Faction snapshots exposed opposing unit identities");
    require(has_marker(observer, MarkerKind::installation, "Central workshop") &&
            !has_marker(west, MarkerKind::installation, "Central workshop") &&
            std::ranges::none_of(west.markers, [](const Marker& marker) {
                return marker.kind == MarkerKind::installation && marker.sector == SectorId{1};
            }), "Unobserved installation appeared in the faction map");

    campaign.advance_ticks(5);
    const auto contested = read_snapshot(campaign, {});
    const auto west_battle = read_snapshot(campaign, FactionId::west);
    const auto east_battle = read_snapshot(campaign, FactionId::east);
    require(contested.tiles[1].contested && contested.tick == 5 &&
            contested.elapsed_seconds == campaign.clock().elapsed_seconds() &&
            west_battle.tiles[1].contested && east_battle.tiles[1].contested,
            "Participants did not report the contested sector");
    for (const auto* snapshot : {&contested, &east_battle}) {
        require(std::ranges::any_of(snapshot->markers, [](const Marker& marker) {
                    return marker.kind == MarkerKind::unit && marker.label == "East field group" &&
                           marker.sector == SectorId{1} && marker.detail.find("1 lost") != std::string::npos;
                }), "A recorded equipment loss was missing from the unit details");
    }
    require(has_marker(contested, MarkerKind::mission, "attack #1") &&
            std::ranges::any_of(west_battle.markers, [&](const Marker& marker) {
                return marker.kind == MarkerKind::contact && marker.sector == SectorId{1} && marker.estimated &&
                       marker.position == west_battle.sector_center(marker.sector) &&
                       marker.symbol && marker.symbol->affiliation == Affiliation::unknown &&
                       marker.symbol->status == SymbolStatus::reported &&
                       marker.symbol->identity.function == MilitaryFunction::unspecified &&
                       marker.symbol->identity.echelon == MilitaryEchelon::unspecified &&
                       marker.inspection.empty();
            }) && !has_marker(west_battle, MarkerKind::unit, "East field group"),
            "Viewer lost a mission or rendered a reported contact as a true unit");
    require(!west_battle.events.empty() && std::ranges::all_of(west_battle.events, [](const Event& event) {
                return event.description.find(" / WEST / Sector ") != std::string::npos;
            }), "Faction event log contained an unauthorized event");

    campaign.advance_ticks(3);
    const auto captured = read_snapshot(campaign, {});
    const auto west_after_capture = read_snapshot(campaign, FactionId::west);
    const auto east_after_capture = read_snapshot(campaign, FactionId::east);
    require(captured.tiles[1].controller == FactionId::west &&
            !captured.tiles[1].contested && !west_after_capture.tiles[1].contested &&
            east_after_capture.tiles[1].controller == FactionId::neutral &&
            east_after_capture.tiles[1].contested && east_after_capture.tiles[1].observed_tick == 5 &&
            campaign.clock().tick() == 8,
            "Perspective refresh revealed a capture or advanced the campaign");
    require(std::ranges::any_of(captured.markers, [](const Marker& marker) {
                return marker.kind == MarkerKind::installation && marker.label == "Central workshop" &&
                       marker.faction == FactionId::west && marker.detail == "WEST / 40% integrity / power off";
            }), "Viewer did not reflect the captured installation and its condition");
    require(std::ranges::is_sorted(captured.events, {}, [](const Event& event) {
                return std::tie(event.tick, event.sequence);
            }), "Recent events were not sorted by simulation tick and sequence");
    const auto started = std::ranges::find_if(captured.events, [](const Event& event) {
        return event.description.starts_with("ground operation started / WEST");
    });
    const auto moved = std::ranges::find(captured.events, "Sector 1 / unit moved", &Event::description);
    const auto lost = std::ranges::find(captured.events, "Equipment 2 / permanently lost", &Event::description);
    const auto engaged = std::ranges::find(captured.events, "Sector 1 / engagement", &Event::description);
    require(moved != captured.events.end() && lost != captured.events.end() &&
            started < moved && moved < engaged && engaged < lost,
            "Same-tick events were grouped by subsystem instead of emission order");

    auto displayed = long_example_campaign(42);
    auto headless = long_example_campaign(42);
    for (int tick = 0; tick < 600; ++tick) {
        const auto frame = read_snapshot(displayed, {});
        require(frame.width == 9 && frame.height == 7 && frame.tick == displayed.clock().tick(),
                "Long campaign viewer used different geometry or time");
        const auto faction_frame = read_snapshot(displayed, tick % 2 == 0 ? FactionId::west : FactionId::east);
        if (tick == 12) {
            require(std::ranges::any_of(faction_frame.markers, [](const Marker& marker) {
                return marker.kind == MarkerKind::unit && marker.faction == FactionId::west &&
                       marker.movement.find("50% / ETA 120s") != std::string::npos;
            }) && std::ranges::none_of(faction_frame.markers, [](const Marker& marker) {
                return marker.faction == FactionId::east && !marker.movement.empty();
            }), "The viewer omitted friendly movement or exposed enemy progress");
        }
        displayed.advance_ticks(1);
    }
    headless.advance_ticks(600);
    require(displayed.war_result() == headless.war_result() && displayed.front_line() == headless.front_line() &&
            std::ranges::equal(displayed.observer_battle_log(), headless.observer_battle_log()) &&
            std::ranges::equal(displayed.observer_operations_log(), headless.observer_operations_log()) &&
            std::ranges::equal(displayed.logistics_log(), headless.logistics_log()) &&
            std::ranges::equal(displayed.forces_log(), headless.forces_log()) &&
            std::ranges::equal(displayed.economy_ledger(), headless.economy_ledger()),
            "Viewer snapshots changed the long campaign");

    ProceduralSettings settings;
    settings.seed = 2026;
    settings.width = 13;
    settings.height = 11;
    auto procedural = procedural_campaign(settings);
    const auto preview = read_snapshot(procedural, {});
    require(preview.width == 13 && preview.height == 11 && preview.tiles.size() == 143 &&
            std::ranges::any_of(preview.tiles, [](const Tile& tile) { return tile.terrain == Terrain::water; }) &&
            std::ranges::any_of(preview.markers, [](const Marker& marker) {
                return marker.kind == MarkerKind::installation;
            }), "The viewer did not use the generated world and infrastructure");
    settings.width = settings.height = 128;
    const auto large = procedural_campaign(settings);
    const auto large_preview = read_snapshot(large, {});
    require(large_preview.width == 128 && large_preview.height == 128 &&
            large_preview.tiles.size() == 16384 && large_preview.sector_size_m == 2000,
            "The viewer truncated a large procedural world or changed its physical scale");
}
