#include "viewer_model.hpp"

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

}  // namespace

int main()
{
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
            std::ranges::any_of(west_battle.markers, [](const Marker& marker) {
                return marker.kind == MarkerKind::contact && marker.sector == SectorId{1};
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
}
