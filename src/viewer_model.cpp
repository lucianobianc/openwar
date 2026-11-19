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

bool active_mission(const Mission& mission)
{
    return mission_active(mission.status);
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

std::string number_label(std::string_view label, auto id)
{
    return std::string(label) + " #" + std::to_string(static_cast<std::uint32_t>(id));
}

std::string travel_progress(const RouteTrace& journey)
{
    const auto percent = journey.duration_us == 0 ? 100 :
        static_cast<int>(__int128_t{100} * journey.elapsed_us / journey.duration_us);
    return "Stored route " + std::to_string(percent) + "% travelled";
}

void add_missions(Snapshot& snapshot, const CampaignState& campaign, std::span<const Mission> missions,
                  std::span<const OperationsEvent> events, bool faction_limited)
{
    using MissionKey = std::pair<FactionId, MissionId>;
    std::map<MissionKey, std::vector<const OperationsEvent*>> by_mission;
    for (const auto& event : events) {
        if (event.mission) {
            by_mission[{event.faction, *event.mission}].push_back(&event);
        }
    }
    std::vector<std::pair<bool, std::uint64_t>> order;
    order.reserve(missions.size());
    const auto first_index = snapshot.markers.size();
    for (const auto& mission : missions) {
        const auto& item = campaign.equipment().at(mission.equipment);
        const auto& assigned = campaign.units().at(item.assignment.unit);
        Marker marker{
            mission.target, MarkerKind::mission, mission.faction, static_cast<std::uint32_t>(mission.id),
            mission.ground_destination.value_or(snapshot.sector_center(mission.target)), !mission.ground_destination,
            number_label(mission_name(mission.kind), mission.id),
            std::string(mission_status_name(mission.status)),
            "Assigned to " + assigned.name
        };
        marker.mission_status = mission.status;
        auto& rows = marker.inspection;
        rows.push_back(std::string(faction_name(mission.faction)) + (mission.manual ? " / Player order" : " / Autonomous"));
        rows.push_back(number_label("Formation", assigned.id) + " / " + number_label("Equipment", mission.equipment));
        const auto history = by_mission.find({mission.faction, mission.id});
        const auto launched = mission.launch_tick != 0 ||
            mission.status == MissionStatus::holding || mission.status == MissionStatus::released;
        if (history != by_mission.end()) {
            rows.push_back("First recorded tick " + std::to_string(history->second.front()->tick));
        }
        if (launched) {
            rows.push_back("Launch tick " + std::to_string(mission.launch_tick));
        } else if (mission.earliest_launch_tick && active_mission(mission)) {
            rows.push_back("Earliest launch tick " + std::to_string(mission.earliest_launch_tick));
        } else {
            rows.push_back(active_mission(mission) ? "Awaiting launch" : "No launch recorded");
        }
        if (mission.ground_journey && mission.ground_destination) {
            const auto& journey = *mission.ground_journey;
            marker.journey = trace_journey(journey.route, journey.progress, *mission.ground_destination);
            rows.push_back(travel_progress(*marker.journey));
            if (active_mission(mission)) {
                const auto movement = campaign.ground_movement(assigned.id, faction_limited);
                if (journey.progress.leg == journey.route.legs.size()) {
                    rows.push_back("Travel ETA 0 s / destination reached");
                } else {
                    rows.push_back(movement && movement->eta_seconds ?
                        "Travel ETA " + std::to_string(*movement->eta_seconds) + " s" : "Travel ETA unavailable");
                }
            }
            std::int64_t paid = 0;
            for (std::size_t i = 0; i < journey.progress.leg + journey.progress.entry_paid; ++i) {
                paid += journey.route.legs[i].fuel_ml;
            }
            rows.push_back("Route fuel charged " + std::to_string(paid) + " mL");
            rows.push_back("Stored route fuel " + std::to_string(journey.route.fuel_ml) + " mL");
            rows.push_back("Ground fuel drawn during travel");
        } else if (mission.ground_unit) {
            rows.push_back("Stored route and travel ETA unavailable");
        } else {
            const auto tick_seconds = campaign.clock().tick_seconds();
            rows.push_back("Abstract duration " + std::to_string(mission.duration_ticks * tick_seconds) + " s");
            if (mission.status == MissionStatus::in_progress) {
                const auto elapsed_ticks = snapshot.tick - mission.launch_tick;
                const auto left = mission.duration_ticks - std::min(elapsed_ticks, mission.duration_ticks);
                rows.push_back("Time to resolution " + std::to_string(left * tick_seconds) + " s");
            }
            rows.push_back("Flight route and travel ETA unavailable");
            const auto fuel = launched ? "Fuel consumed " : active_mission(mission) ? "Fuel reserved " : "Planned fuel ";
            rows.push_back(fuel + std::to_string(mission.fuel) + " L");
            if (mission.preparation_reserved) {
                const auto& costs = campaign.equipment_types().at(item.assignment.type).preparation.costs;
                for (std::size_t i = 0; i < costs.size(); ++i) {
                    if (costs[i]) {
                        rows.push_back("Prep reserve " + std::string(resource_name(static_cast<Resource>(i))) +
                            " " + std::to_string(costs[i]));
                    }
                }
            }
        }
        if (active_mission(mission)) {
            if (mission.status == MissionStatus::holding) {
                rows.push_back("Holding until released or interrupted");
            }
            rows.push_back(number_label("Committed equipment", mission.equipment));
            rows.push_back(number_label("Mission stockpile", mission.launch_stock));
        }
        if (mission.kind == MissionKind::ground_recon) {
            if (mission.recon_result != ReconResult::none || !active_mission(mission)) {
                auto result = std::string(ground_recon_result(mission));
                if (mission.detections) {
                    result += " / " + std::to_string(mission.detections);
                }
                rows.push_back(std::move(result));
            }
        }
        if (history != by_mission.end()) {
            const auto& entries = history->second;
            rows.push_back("Mission events, newest first");
            for (auto event = entries.rbegin(); event != entries.rend(); ++event) {
                rows.push_back("Tick " + std::to_string((*event)->tick) + " / " + (*event)->reason);
            }
        }
        order.emplace_back(active_mission(mission), history == by_mission.end() ? 0 : history->second.back()->tick);
        snapshot.mission_markers.push_back(snapshot.markers.size());
        snapshot.markers.push_back(std::move(marker));
    }
    std::ranges::stable_sort(snapshot.mission_markers, [&](std::size_t left, std::size_t right) {
        return order[left - first_index] > order[right - first_index];
    });
}

std::string position_label(geography::Point point)
{
    return "E " + std::to_string(point.east) + " / S " + std::to_string(point.south) + " mm";
}

struct InventoryCount {
    std::size_t available = 0;
    std::size_t unavailable = 0;
    std::size_t damaged = 0;
    std::size_t lost = 0;
};


void add_logistics(Snapshot& snapshot, const CampaignState& campaign, std::optional<FactionId> perspective)
{
    for (const auto& [id, carrier] : campaign.carriers()) {
        if (carrier.lost || (perspective && carrier.faction != *perspective)) {
            continue;
        }
        const auto number = static_cast<std::uint32_t>(id);
        std::string detail = "Idle";
        std::string movement;
        const TransportJourney* active_journey = nullptr;
        std::vector<std::string> inspection{std::string(faction_name(carrier.faction)) + " / Ground transport",
            std::to_string(carrier.fuel) + " / " + std::to_string(carrier.tank_capacity) + " L fuel"};
        if (carrier.shipment) {
            const auto& shipment = campaign.shipments().at(*carrier.shipment);
            detail = std::string(shipment_status_name(shipment.status)) + " / Shipment #" +
                std::to_string(static_cast<std::uint32_t>(*carrier.shipment));
            movement = std::to_string(shipment.cargo) + " " + std::string(resource_name(shipment.request.resource));
            const auto pickup = shipment.cargo == 0;
            const auto endpoint = pickup ? shipment.request.origin : shipment.request.destination;
            inspection.push_back(detail);
            inspection.push_back("Cargo " + movement + " / capacity " + std::to_string(carrier.capacity));
            inspection.push_back(number_label(pickup ? "Pickup stockpile" : "Delivery stockpile", endpoint));
            if (shipment.journey) {
                active_journey = &*shipment.journey;
                inspection.push_back(position_label(shipment.journey->destination));
                movement += " / " + std::to_string(shipment.journey->progress.elapsed_us / 1'000'000) +
                    " s in journey";
            }
        } else {
            inspection.push_back("Unassigned / no cargo");
            inspection.push_back("Cargo capacity " + std::to_string(carrier.capacity));
        }
        detail += " / " + std::to_string(carrier.fuel) + " L fuel";
        snapshot.markers.push_back({carrier.sector, MarkerKind::carrier, carrier.faction, number,
            carrier.position.value(), false, "Carrier #" + std::to_string(number), std::move(detail),
            std::move(movement), MilitarySymbol{relative_affiliation(carrier.faction, perspective),
                {MilitaryDomain::land, MilitaryFunction::transport}}, std::move(inspection)});
        if (active_journey) {
            snapshot.markers.back().journey =
                trace_journey(active_journey->route, active_journey->progress, active_journey->destination);
            snapshot.markers.back().inspection.push_back(travel_progress(*snapshot.markers.back().journey));
        }
    }

    std::map<StockpileId, std::vector<std::string>> maintenance;
    for (const auto& [id, facility] : campaign.facilities()) {
        maintenance[facility.stockpile].push_back(number_label("Maintenance facility", id) +
            (facility.operational ? " / operational" : " / offline"));
    }
    const auto add_maintenance = [&](StockpileId stock) {
        const auto found = maintenance.find(stock);
        if (found != maintenance.end()) {
            auto& rows = snapshot.markers.back().inspection;
            rows.insert(rows.end(), found->second.begin(), found->second.end());
        }
    };
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
                (installation.has_power ? "power on" : "power off"), {},
            MilitarySymbol{relative_affiliation(installation.operator_faction, perspective),
                {MilitaryDomain::land}, installation.integrity == 0 ? SymbolStatus::destroyed : SymbolStatus::present},
            {"Production installation", number_label("Supply stockpile", stock.id),
             "Workers " + std::to_string(installation.workers),
             "Reactivation remaining " + std::to_string(state.reactivation_seconds) + " s"}});
        for (std::size_t i = 0; i < installation.recipe.outputs.size(); ++i) {
            if (const auto quantity = installation.recipe.outputs[i]) {
                snapshot.markers.back().inspection.push_back("Produces " + std::to_string(quantity) + " " +
                    std::string(resource_name(static_cast<Resource>(i))) + " per batch");
            }
        }
        add_maintenance(stock.id);
    }
    for (const auto& [id, stock] : campaign.stockpiles()) {
        if (!stock.access_point || installation_stocks.contains(id) || (perspective && stock.owner != *perspective)) {
            continue;
        }
        const auto number = static_cast<std::uint32_t>(id);
        snapshot.markers.push_back({stock.sector, MarkerKind::depot, stock.owner, number, *stock.access_point,
            false, "Stockpile #" + std::to_string(number), std::string(faction_name(stock.owner)), {},
            MilitarySymbol{relative_affiliation(stock.owner, perspective), {MilitaryDomain::land}},
            {"Supply stockpile"}});
        add_maintenance(id);
    }
}

void add_formations(Snapshot& snapshot, const CampaignState& campaign, std::optional<FactionId> perspective,
                    std::span<const Mission> missions)
{
    std::map<UnitId, std::map<EquipmentTypeId, InventoryCount>> inventory;
    std::map<UnitId, UnitReadiness> readiness_by_unit;
    for (const auto& [id, item] : campaign.equipment()) {
        const auto unit = item.assignment.unit;
        if (perspective && campaign.units().at(unit).faction != perspective) {
            continue;
        }
        auto& count = inventory[unit][item.assignment.type];
        auto& readiness = readiness_by_unit[unit];
        if (item.status == EquipmentStatus::lost) {
            ++count.lost;
            ++readiness.lost;
            continue;
        }
        if (item.damage) {
            ++count.damaged;
        }
        if (item.status == EquipmentStatus::available || item.status == EquipmentStatus::ready) {
            ++count.available;
            ++readiness.ready;
        } else {
            ++count.unavailable;
        }
        if (item.status != EquipmentStatus::reserve && item.status != EquipmentStatus::in_transit) {
            ++readiness.active;
        }
    }
    std::map<UnitId, std::vector<UnitId>> children;
    for (const auto& [id, unit] : campaign.units()) {
        if (unit.parent && (!perspective || unit.faction == perspective)) {
            children[*unit.parent].push_back(id);
        }
    }
    std::map<UnitId, std::vector<const Mission*>> assignments;
    for (const auto& mission : missions) {
        if (active_mission(mission)) {
            assignments[campaign.equipment().at(mission.equipment).assignment.unit].push_back(&mission);
        }
    }

    for (const auto& [id, unit] : campaign.units()) {
        if (perspective && unit.faction != perspective) {
            continue;
        }
        const auto readiness = readiness_by_unit[id];
        const auto& identity = unit.identity;
        std::string function(military_functions[static_cast<std::size_t>(identity.function)]);
        std::ranges::replace(function, '-', ' ');
        std::vector<std::string> inspection{std::string(faction_name(unit.faction)) + " / " +
            std::string(military_domains[static_cast<std::size_t>(identity.domain)]),
            "Function " + function,
            "Echelon " + std::string(military_echelons[static_cast<std::size_t>(identity.echelon)])};
        if (unit.parent) {
            inspection.push_back(number_label("Parent", *unit.parent) + " / " + campaign.units().at(*unit.parent).name);
        }
        for (const auto child : children[id]) {
            inspection.push_back(number_label("Subordinate", child) + " / " + campaign.units().at(child).name);
        }
        if (assignments[id].empty()) {
            inspection.push_back("No active mission");
        }
        for (const auto* mission : assignments[id]) {
            inspection.push_back(number_label(mission_name(mission->kind), mission->id) +
                " / " + std::string(mission_status_name(mission->status)));
        }
        auto detail = std::to_string(readiness.ready) + "/" + std::to_string(readiness.active) + " ready";
        if (readiness.lost) {
            detail += " / " + std::to_string(readiness.lost) + " lost";
        }
        const auto combat = campaign.combat_units().find(id);
        std::optional<geography::Point> position;
        auto status = SymbolStatus::present;
        if (combat != campaign.combat_units().end()) {
            detail += " / ";
            detail += ground_status_name(combat->second.status);
            position = combat->second.position;
            if (combat->second.status == GroundStatus::destroyed) {
                status = SymbolStatus::destroyed;
            }
            const auto& stores = campaign.stockpiles().at(combat->second.stores);
            for (const auto resource : {Resource::fuel, Resource::supplies, Resource::ammunition}) {
                inspection.push_back(std::string(resource_name(resource)) + " " +
                    std::to_string(stores.quantity[static_cast<std::size_t>(resource)]));
            }
        } else if (unit.facility) {
            const auto stock = campaign.facilities().at(*unit.facility).stockpile;
            const auto& stores = campaign.stockpiles().at(stock);
            if (stores.sector == unit.sector) {
                position = stores.access_point;
            }
        }
        std::string movement_detail;
        if (const auto movement = campaign.ground_movement(id, perspective.has_value())) {
            movement_detail = "To sector " + std::to_string(static_cast<std::uint32_t>(movement->next_sector)) +
                " / " + std::to_string(movement->leg_percent) + "% / ETA ";
            movement_detail += movement->eta_seconds ? std::to_string(*movement->eta_seconds) + "s" : "unavailable";
        }
        if (inventory[id].empty()) {
            inspection.push_back("No equipment assigned");
        }
        for (const auto& [type_id, count] : inventory[id]) {
            inspection.push_back(number_label("Type", type_id) + " / " + campaign.equipment_types().at(type_id).name);
            inspection.push_back(std::to_string(count.available) + " available / " +
                                 std::to_string(count.unavailable) + " unavailable");
            inspection.push_back(std::to_string(count.damaged) + " damaged / " + std::to_string(count.lost) + " lost");
        }
        snapshot.markers.push_back({
            unit.sector, MarkerKind::unit, unit.faction, static_cast<std::uint32_t>(id),
            position.value_or(snapshot.sector_center(unit.sector)), !position, unit.name,
            std::move(detail), std::move(movement_detail),
            MilitarySymbol{relative_affiliation(unit.faction, perspective), identity, status}, std::move(inspection)
        });
        if (combat != campaign.combat_units().end() && combat->second.withdrawal_journey) {
            const auto& journey = *combat->second.withdrawal_journey;
            const auto destination = journey.route.legs.empty() ? journey.route.origin : journey.route.legs.back().to;
            snapshot.markers.back().journey = trace_journey(journey.route, journey.progress, destination);
        }
    }
}

}  // namespace

RouteTrace trace_journey(const geography::Route& route, const geography::Progress& progress,
                         geography::Point destination)
{
    RouteTrace trace{{}, progress.position, destination, progress.leg, progress.elapsed_us,
                     route.duration_us, route.fuel_ml};
    trace.vertices.reserve(route.legs.size() + 1);
    trace.vertices.push_back(route.origin);
    for (const auto& leg : route.legs) {
        trace.vertices.push_back(leg.to);
    }
    return trace;
}

Affiliation relative_affiliation(std::optional<FactionId> faction, std::optional<FactionId> perspective)
{
    if (!faction) {
        return Affiliation::unknown;
    }
    if (*faction == FactionId::neutral) {
        return Affiliation::neutral;
    }
    if (!perspective) {
        return Affiliation::observer;
    }
    return faction == perspective ? Affiliation::friendly : Affiliation::hostile;
}

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
    const auto view = perspective && (campaign.has_operational_view() || campaign.geographic_world()) ?
        campaign.faction_view(*perspective) : FactionView{};
    if (perspective) {
        snapshot.roads = view.roads;
    } else {
        for (const auto road : campaign.closed_roads()) {
            snapshot.roads.emplace(road, RoadReport{true, clock.tick()});
        }
    }
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

    std::vector<Mission> observer_missions;
    if (!perspective && campaign.has_operational_view()) {
        for (const auto faction : {FactionId::west, FactionId::east}) {
            const auto known = campaign.faction_view(faction, false);
            observer_missions.insert(observer_missions.end(), known.missions.begin(), known.missions.end());
        }
    }
    const auto& missions = perspective ? view.missions : observer_missions;
    add_formations(snapshot, campaign, perspective, missions);
    add_logistics(snapshot, campaign, perspective);

    if (perspective) {
        for (const auto& contact : view.contacts) {
            const auto domain = contact.classification == "aircraft" ? MilitaryDomain::air :
                contact.classification == "ground" ? MilitaryDomain::land : MilitaryDomain::unspecified;
            snapshot.markers.push_back({
                contact.estimated_sector, MarkerKind::contact, {}, static_cast<std::uint32_t>(contact.id),
                snapshot.sector_center(contact.estimated_sector), true,
                "Contact #" + std::to_string(static_cast<std::uint32_t>(contact.id)),
                std::string(contact.classification) + " / " + std::string(confidence_name(contact.confidence)) +
                    " / tick " + std::to_string(contact.observed_tick), {},
                MilitarySymbol{Affiliation::unknown, {domain}, SymbolStatus::reported}
            });
        }
        for (const auto& site : view.installations) {
            snapshot.markers.push_back({
                site.sector, MarkerKind::installation, {}, static_cast<std::uint32_t>(site.sector),
                snapshot.sector_center(site.sector), true, "Reported installation",
                std::string(confidence_name(site.confidence)) + " / tick " + std::to_string(site.observed_tick), {},
                MilitarySymbol{Affiliation::unknown, {}, SymbolStatus::reported}
            });
        }
        add_missions(snapshot, campaign, missions, view.events, true);
        for (const auto& event : recent(std::span<const OperationsEvent>{view.events})) {
            snapshot.events.push_back(operation_event(event));
        }
        return snapshot;
    }

    add_missions(snapshot, campaign, missions, campaign.observer_operations_log(), false);
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
