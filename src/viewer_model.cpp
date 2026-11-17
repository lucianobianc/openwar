#include "viewer_model.hpp"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace openwar::viewer {
namespace {

constexpr std::size_t event_limit = 100;

template<class Entry>
std::span<const Entry> recent(std::span<const Entry> events)
{
    return events.last(std::min(events.size(), event_limit));
}

void add_missions(Snapshot& snapshot, const FactionView& view)
{
    for (const auto& mission : view.missions) {
        if (mission.status != MissionStatus::scheduled && mission.status != MissionStatus::preparing &&
            mission.status != MissionStatus::in_progress) {
            continue;
        }
        snapshot.markers.push_back({
            mission.target, MarkerKind::mission, mission.faction,
            std::string(mission_name(mission.kind)) + " #" + std::to_string(static_cast<std::uint32_t>(mission.id)),
            std::string(mission_status_name(mission.status))
        });
    }
}

Event operation_event(const OperationsEvent& event)
{
    auto label = std::string(faction_name(event.faction)) + " / Sector " +
                 std::to_string(static_cast<std::uint32_t>(event.sector));
    if (event.mission) {
        label += " / Mission " + std::to_string(static_cast<std::uint32_t>(*event.mission));
    }
    return {event.tick, std::string(event.reason) + " / " + label, event.sequence};
}

std::string_view shipment_event_name(LogisticsEventKind kind)
{
    switch (kind) {
    case LogisticsEventKind::requested: return "requested";
    case LogisticsEventKind::assigned: return "assigned";
    case LogisticsEventKind::loaded: return "loaded";
    case LogisticsEventKind::delayed: return "delayed";
    case LogisticsEventKind::unloaded: return "unloaded";
    case LogisticsEventKind::delivered: return "delivered";
    case LogisticsEventKind::cancelled: return "cancelled";
    case LogisticsEventKind::lost: return "lost";
    case LogisticsEventKind::arrived: return "arrived";
    }
    return {};
}

}  // namespace

Snapshot read_snapshot(const CampaignState& campaign, std::optional<FactionId> perspective)
{
    const auto& world = campaign.world();
    const auto& clock = campaign.clock();
    Snapshot snapshot{world.width(), world.height(), world.sector_size_m(), clock.tick(),
                      clock.elapsed_seconds(), campaign.war_result(), {}, {}, {}};
    snapshot.tiles.reserve(world.sectors().size());
    const auto view = perspective ? campaign.faction_view(*perspective) : FactionView{};
    for (const auto& sector : world.sectors()) {
        Tile tile{sector.properties.terrain};
        if (perspective) {
            tile.confidence = 0;
            const auto report = view.sectors.find(sector.id);
            if (report != view.sectors.end()) {
                tile.controller = report->second.estimated_controller;
                tile.confidence = report->second.confidence;
                tile.observed_tick = report->second.observed_tick;
                tile.contested = report->second.contested;
            }
        } else {
            const auto& state = campaign.sector_state(sector.id);
            tile.controller = state.controller;
            tile.contested = state.control == ControlState::contested;
        }
        snapshot.tiles.push_back(tile);
    }

    for (const auto& [id, unit] : campaign.units()) {
        if (perspective && unit.faction != perspective) {
            continue;
        }
        const auto readiness = campaign.unit_readiness(id);
        auto detail = std::to_string(readiness.ready) + "/" + std::to_string(readiness.active) + " ready";
        if (readiness.lost) {
            detail += " / " + std::to_string(readiness.lost) + " lost";
        }
        const auto combat = campaign.combat_units().find(id);
        if (combat != campaign.combat_units().end()) {
            detail += " / ";
            detail += ground_status_name(combat->second.status);
        }
        snapshot.markers.push_back({unit.sector, MarkerKind::unit, unit.faction, unit.name, std::move(detail)});
    }

    if (perspective) {
        for (const auto& contact : view.contacts) {
            snapshot.markers.push_back({
                contact.estimated_sector, MarkerKind::contact, {},
                "Contact #" + std::to_string(static_cast<std::uint32_t>(contact.id)),
                std::string(contact.classification) + " / " + std::string(confidence_name(contact.confidence)) +
                    " / tick " + std::to_string(contact.observed_tick)
            });
        }
        for (const auto& site : view.installations) {
            snapshot.markers.push_back({
                site.sector, MarkerKind::installation, {}, "Reported installation",
                std::string(confidence_name(site.confidence)) + " / tick " + std::to_string(site.observed_tick)
            });
        }
        add_missions(snapshot, view);
        for (const auto& event : recent(std::span<const OperationsEvent>{view.events})) {
            snapshot.events.push_back(operation_event(event));
        }
        return snapshot;
    }

    for (const auto& state : campaign.installations()) {
        const auto& installation = state.installation;
        const auto& stock = campaign.stockpiles().at(installation.stockpile);
        snapshot.markers.push_back({
            stock.sector, MarkerKind::installation, installation.operator_faction, installation.name,
            std::string(faction_name(installation.operator_faction)) + " / " +
                std::to_string(installation.integrity) + "% integrity / " +
                (installation.has_power ? "power on" : "power off")
        });
    }
    for (auto faction : {FactionId::west, FactionId::east}) {
        add_missions(snapshot, campaign.faction_view(faction, false));
    }
    for (const auto& event : recent(campaign.logistics_log())) {
        snapshot.events.push_back({
            event.tick, "Shipment " + std::to_string(static_cast<std::uint32_t>(event.shipment)) +
                " / " + std::string(shipment_event_name(event.kind)), event.sequence
        });
    }
    for (const auto& event : recent(campaign.economy_ledger())) {
        snapshot.events.push_back({
            event.tick, "Stockpile " + std::to_string(static_cast<std::uint32_t>(event.stockpile)) +
                (event.producer ? " / Production batch" : " / Stock transaction"), event.sequence
        });
    }
    for (const auto& event : recent(campaign.forces_log())) {
        snapshot.events.push_back({
            event.tick, "Equipment " + std::to_string(static_cast<std::uint32_t>(event.equipment)) +
                " / " + std::string(equipment_status_name(event.status)), event.sequence
        });
    }
    for (const auto& event : recent(campaign.observer_battle_log())) {
        snapshot.events.push_back({
            event.tick, "Sector " + std::to_string(static_cast<std::uint32_t>(event.sector)) +
                " / " + std::string(event.reason), event.sequence
        });
    }
    for (const auto& event : recent(campaign.observer_operations_log())) {
        snapshot.events.push_back(operation_event(event));
    }
    if (campaign.war_ended()) {
        snapshot.events.push_back({snapshot.war.tick, std::string(war_status_name(snapshot.war.status)) +
            " / " + std::string(snapshot.war.cause), std::numeric_limits<std::uint64_t>::max()});
    }
    std::ranges::sort(snapshot.events, {}, [](const Event& event) {
        return std::tie(event.tick, event.sequence);
    });
    if (snapshot.events.size() > event_limit) {
        snapshot.events.erase(snapshot.events.begin(), snapshot.events.end() - event_limit);
    }
    return snapshot;
}

}  // namespace openwar::viewer
