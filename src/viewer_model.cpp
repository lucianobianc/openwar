#include "viewer_model.hpp"

#include <algorithm>
#include <limits>
#include <set>
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
            mission.target, MarkerKind::mission, mission.faction, static_cast<std::uint32_t>(mission.id),
            mission.ground_destination.value_or(snapshot.sector_center(mission.target)), !mission.ground_destination,
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

void add_logistics(Snapshot& snapshot, const CampaignState& campaign, std::optional<FactionId> perspective)
{
    for (const auto& [id, carrier] : campaign.carriers()) {
        if (carrier.lost || (perspective && carrier.faction != *perspective)) {
            continue;
        }
        const auto number = static_cast<std::uint32_t>(id);
        std::string detail = "Idle";
        std::string movement;
        if (carrier.shipment) {
            const auto& shipment = campaign.shipments().at(*carrier.shipment);
            detail = std::string(shipment_status_name(shipment.status)) + " / Shipment #" +
                std::to_string(static_cast<std::uint32_t>(*carrier.shipment));
            movement = std::to_string(shipment.cargo) + " " + std::string(resource_name(shipment.request.resource));
            if (shipment.journey) {
                movement += " / " + std::to_string(shipment.journey->progress.elapsed_us / 1'000'000) +
                    " s in journey";
            }
        }
        detail += " / " + std::to_string(carrier.fuel) + " L fuel";
        snapshot.markers.push_back({carrier.sector, MarkerKind::carrier, carrier.faction, number,
            carrier.position.value(), false, "Carrier #" + std::to_string(number), std::move(detail),
            std::move(movement)});
    }

    std::set<StockpileId> installation_stocks;
    for (const auto& state : campaign.installations()) {
        const auto& installation = state.installation;
        const auto& stock = campaign.stockpiles().at(installation.stockpile);
        installation_stocks.insert(stock.id);
        if (perspective && (installation.operator_faction != *perspective || stock.owner != *perspective)) {
            continue;
        }
        snapshot.markers.push_back({stock.sector, MarkerKind::installation, installation.operator_faction,
            static_cast<std::uint32_t>(installation.id), campaign.stockpile_position(stock.id), false,
            installation.name, std::string(faction_name(installation.operator_faction)) + " / " +
                std::to_string(installation.integrity) + "% integrity / " +
                (installation.has_power ? "power on" : "power off")});
    }
    for (const auto& [id, stock] : campaign.stockpiles()) {
        if (!stock.access_point || installation_stocks.contains(id) || (perspective && stock.owner != *perspective)) {
            continue;
        }
        const auto number = static_cast<std::uint32_t>(id);
        snapshot.markers.push_back({stock.sector, MarkerKind::depot, stock.owner, number, *stock.access_point,
            false, "Stockpile #" + std::to_string(number), std::string(faction_name(stock.owner))});
    }
}

}  // namespace

geography::Point Snapshot::sector_center(SectorId sector) const
{
    const auto id = static_cast<std::uint32_t>(sector);
    const auto size = static_cast<std::int64_t>(sector_size_m) * 1000;
    return {(id % width) * size + size / 2, (id / width) * size + size / 2, 0};
}

Snapshot read_snapshot(const CampaignState& campaign, std::optional<FactionId> perspective)
{
    const auto& world = campaign.world();
    const auto& clock = campaign.clock();
    Snapshot snapshot{world.width(), world.height(), world.sector_size_m(), clock.tick(),
                      clock.elapsed_seconds(), campaign.war_result(), {}, {}, {}};
    snapshot.tiles.reserve(world.sectors().size());
    const auto view = perspective && campaign.has_operational_view() ?
        campaign.faction_view(*perspective) : FactionView{};
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
        std::optional<geography::Point> position;
        if (combat != campaign.combat_units().end()) {
            detail += " / ";
            detail += ground_status_name(combat->second.status);
            position = combat->second.position;
        }
        std::string movement_detail;
        if (const auto movement = campaign.ground_movement(id)) {
            movement_detail = "To sector " + std::to_string(static_cast<std::uint32_t>(movement->next_sector)) +
                " / " + std::to_string(movement->leg_percent) + "% / ETA ";
            movement_detail += movement->eta_seconds ? std::to_string(*movement->eta_seconds) + "s" : "unavailable";
        }
        snapshot.markers.push_back({
            unit.sector, MarkerKind::unit, unit.faction, static_cast<std::uint32_t>(id),
            position.value_or(snapshot.sector_center(unit.sector)), !position, unit.name,
            std::move(detail), std::move(movement_detail)
        });
    }
    add_logistics(snapshot, campaign, perspective);

    if (perspective) {
        for (const auto& contact : view.contacts) {
            snapshot.markers.push_back({
                contact.estimated_sector, MarkerKind::contact, {}, static_cast<std::uint32_t>(contact.id),
                snapshot.sector_center(contact.estimated_sector), true,
                "Contact #" + std::to_string(static_cast<std::uint32_t>(contact.id)),
                std::string(contact.classification) + " / " + std::string(confidence_name(contact.confidence)) +
                    " / tick " + std::to_string(contact.observed_tick)
            });
        }
        for (const auto& site : view.installations) {
            snapshot.markers.push_back({
                site.sector, MarkerKind::installation, {}, static_cast<std::uint32_t>(site.sector),
                snapshot.sector_center(site.sector), true, "Reported installation",
                std::string(confidence_name(site.confidence)) + " / tick " + std::to_string(site.observed_tick)
            });
        }
        add_missions(snapshot, view);
        for (const auto& event : recent(std::span<const OperationsEvent>{view.events})) {
            snapshot.events.push_back(operation_event(event));
        }
        return snapshot;
    }

    if (campaign.has_operational_view()) {
        for (auto faction : {FactionId::west, FactionId::east}) {
            add_missions(snapshot, campaign.faction_view(faction, false));
        }
    }
    for (const auto& event : recent(campaign.logistics_log())) {
        snapshot.events.push_back({
            event.tick, "Shipment " + std::to_string(static_cast<std::uint32_t>(event.shipment)) +
                " / " + std::string(logistics_event_name(event.kind)), event.sequence
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
