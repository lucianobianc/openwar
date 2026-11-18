#include "viewer_model.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
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
    const auto order = campaign.issue_order(west, MissionKind::defend, target);
    require(order.mission.has_value(), "Could not start geographic viewer fixture movement");
    const auto scheduled = read_snapshot(campaign, west);
    require(marker_at(scheduled, {MarkerKind::mission, west, static_cast<std::uint32_t>(*order.mission)}).position ==
                scenario.ground_destinations.at(target), "Mission marker did not use its geographic objective");
    campaign.advance_ticks(5);
    const auto moving = read_snapshot(campaign, {});
    const auto& moving_unit = marker_at(moving, formation);
    const auto& moving_carrier = marker_at(moving, carrier);
    require(moving_unit.position != start && moving_unit.position == campaign.combat_units().at(UnitId{1}).position &&
            moving_carrier.position != start && moving_carrier.position == campaign.carriers().at(CarrierId{1}).position &&
            !moving_unit.movement.empty() && !moving_carrier.movement.empty(),
            "Viewer missed in-transit coordinates or movement status");
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
}

}  // namespace

int main()
{
    test_geographic_snapshot();
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
                       marker.position == west_battle.sector_center(marker.sector);
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
