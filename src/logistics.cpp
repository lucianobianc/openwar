#include "campaign.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>

namespace openwar {
namespace {

bool terminal(ShipmentStatus status)
{
    return status == ShipmentStatus::delivered || status == ShipmentStatus::cancelled ||
           status == ShipmentStatus::lost;
}

bool unavailable(GroundStatus status)
{
    return status == GroundStatus::destroyed || status == GroundStatus::surrendered;
}

std::size_t cargo_index(Resource resource)
{
    const auto index = static_cast<std::size_t>(resource);
    if (resource == Resource::oil_reserve || index >= ResourceAmounts{}.size()) {
        throw std::invalid_argument("Invalid transport resource");
    }
    return index;
}

constexpr auto fuel_index = static_cast<std::size_t>(Resource::fuel);
constexpr std::int64_t litre_ml = 1000;
constexpr std::int64_t second_us = 1'000'000;

std::int64_t bounded_cost(__int128_t value)
{
    return static_cast<std::int64_t>(std::min<__int128_t>(value, std::numeric_limits<std::int64_t>::max()));
}

std::int64_t fuel_available(const Carrier& carrier, std::uint64_t refuel = 0)
{
    return bounded_cost((__int128_t{carrier.fuel} + refuel) * litre_ml - carrier.spent_ml);
}

std::uint64_t litres_needed(std::int64_t amount)
{
    return amount <= 0 ? 0 : static_cast<std::uint64_t>(amount / litre_ml + (amount % litre_ml != 0));
}

void spend_fuel(Carrier& carrier, std::int64_t amount)
{
    const auto debit = __int128_t{amount} + carrier.spent_ml;
    carrier.fuel -= static_cast<std::uint64_t>(debit / litre_ml);
    carrier.spent_ml = static_cast<std::uint16_t>(debit % litre_ml);
}

geography::Profile default_transport_profile()
{
    geography::Profile profile{};
    profile.terrain = {geography::Rate{12'500, 500}, geography::Rate{6250, 1000}, {}};
    profile.roads = {geography::Rate{12'500, 500}, geography::Rate{10'000, 750}};
    return profile;
}

geography::Route single_leg(geography::Point from, geography::Point to, std::int64_t time, std::int64_t fuel)
{
    geography::Route route{geography::RouteStatus::found, from, {}, time, fuel};
    if (from != to) {
        route.legs.push_back({from, to, geography::horizontal_distance(from, to), time, fuel, {}});
    }
    return route;
}

}  // namespace

std::string_view shipment_status_name(ShipmentStatus status)
{
    switch (status) {
    case ShipmentStatus::pending: return "pending";
    case ShipmentStatus::loading: return "loading";
    case ShipmentStatus::en_route: return "en route";
    case ShipmentStatus::delayed: return "delayed";
    case ShipmentStatus::unloading: return "unloading";
    case ShipmentStatus::delivered: return "delivered";
    case ShipmentStatus::cancelled: return "cancelled";
    case ShipmentStatus::lost: return "lost";
    }
    throw std::invalid_argument("Unknown shipment status");
}

std::string_view logistics_event_name(LogisticsEventKind kind)
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
    case LogisticsEventKind::collecting: return "travelling empty to pickup";
    case LogisticsEventKind::refuelled: return "carrier refuelled";
    case LogisticsEventKind::crossed: return "carrier crossed sector boundary";
    }
    throw std::logic_error("Unknown logistics event");
}

void CampaignState::initialize_logistics(const ScenarioDefinition& scenario)
{
    if (loading_ticks_ == 0 || unloading_ticks_ == 0) {
        throw std::invalid_argument("Loading and unloading times must be positive");
    }
    for (const auto& route : scenario.routes) {
        world_.sector(route.from);
        world_.sector(route.to);
        if (route.from == route.to || route.travel_ticks == 0 || route.slots == 0 ||
            !routes_.emplace(route.id, route).second) {
            throw std::invalid_argument("Invalid or duplicate transport route");
        }
    }
    index_routes();
    for (auto carrier : scenario.carriers) {
        world_.sector(carrier.sector);
        faction_name(carrier.faction);
        if (carrier.tank_capacity == 0) {
            carrier.tank_capacity = carrier.fuel;
        }
        if (carrier.faction == FactionId::neutral || carrier.capacity == 0 ||
            carrier.fuel > carrier.tank_capacity ||
            carrier.shipment || carrier.lost || carrier.spent_ml || !carriers_.emplace(carrier.id, carrier).second) {
            throw std::invalid_argument("Invalid or duplicate carrier");
        }
    }
    std::set<std::pair<StockpileId, Resource>> destinations;
    for (const auto& policy : scenario.resupply) {
        const auto& origin = stockpiles_.at(policy.origin);
        const auto& destination = stockpiles_.at(policy.destination);
        const auto index = cargo_index(policy.resource);
        if (origin.id == destination.id || origin.owner == FactionId::neutral ||
            origin.owner != destination.owner || origin.capacity[index] == 0 ||
            policy.threshold == 0 || policy.threshold > policy.target ||
            policy.target > destination.capacity[index] ||
            !destinations.emplace(destination.id, policy.resource).second) {
            throw std::invalid_argument("Invalid or duplicate resupply policy");
        }
    }
    resupply_ = scenario.resupply;
}

void CampaignState::index_routes()
{
    incoming_routes_.clear();
    transport_footprints_.clear();
    for (const auto& [id, route] : routes_) {
        incoming_routes_[route.to].push_back(id);
        const auto& neighbors = world_.sector(route.from).neighbors;
        if (std::ranges::find(neighbors, route.to) == neighbors.end()) {
            const auto geometry = legacy_transport_leg(route);
            auto& footprint = transport_footprints_[id];
            (void)geography::allows_leg(movement_map_, geometry.legs.front(), [&](std::int64_t sector) {
                const auto cell = static_cast<SectorId>(sector);
                if (cell != route.from && cell != route.to) {
                    footprint.push_back(cell);
                }
                return true;
            });
        }
    }
}

void CampaignState::request_supply(SupplyRequest request)
{
    queue_supply(request, clock_.tick());
}

SupplyOrderPreview CampaignState::preview_supply(FactionId actor, SupplyTarget target, Resource resource,
                                                  std::uint64_t quantity) const
{
    if (!can_issue_orders(actor)) {
        return {"player has no authority to request supplies"};
    }
    const auto index = static_cast<std::size_t>(resource);
    if (resource == Resource::oil_reserve || index >= ResourceAmounts{}.size()) {
        return {"resource cannot be transported"};
    }
    if (quantity == 0) {
        return {"quantity must be positive"};
    }
    std::optional<StockpileId> destination;
    if (const auto* stock = std::get_if<StockpileId>(&target)) {
        destination = *stock;
    } else {
        const auto unit = units_.find(std::get<UnitId>(target));
        if (unit != units_.end() && unit->second.faction == actor) {
            if (const auto formation = combat_units_.find(unit->first); formation != combat_units_.end()) {
                destination = formation->second.stores;
            } else if (unit->second.facility) {
                destination = facilities_.at(*unit->second.facility).stockpile;
            }
        }
    }
    if (!destination || !stockpiles_.contains(*destination) || stockpiles_.at(*destination).owner != actor) {
        return {"destination is not an eligible friendly stockpile"};
    }
    const auto& stock = stockpiles_.at(*destination);
    if (unavailable(recipient_status(*destination)) || recipient_status(*destination) == GroundStatus::withdrawing) {
        return {"recipient is unavailable"};
    }

    SupplyOrderPreview preview{"shipment can be queued", destination};
    preview.room = stock.capacity[index] - stock.quantity[index];
    for (const auto& [id, shipment] : shipments_) {
        if (shipment.request.destination == *destination && shipment.request.resource == resource &&
            !terminal(shipment.status) && stockpiles_.at(shipment.request.origin).owner == actor) {
            const auto remaining = shipment.cargo ? shipment.cargo : shipment.request.quantity;
            preview.room -= std::min(remaining, preview.room);
        }
    }
    std::uint64_t fleet_capacity = 0;
    for (const auto& [id, carrier] : carriers_) {
        if (carrier.faction == actor && !carrier.lost) {
            fleet_capacity = std::max(fleet_capacity, carrier.capacity);
            if (!carrier.shipment) {
                preview.idle_capacity = std::max(preview.idle_capacity, carrier.capacity);
            }
        }
    }
    const auto point = stockpile_position(*destination);
    std::optional<std::tuple<bool, std::int64_t, StockpileId>> best;
    std::optional<StockpileId> source;
    for (const auto& [id, candidate] : stockpiles_) {
        if (id == *destination || candidate.owner != actor || recipient_status(id) != GroundStatus::holding) {
            continue;
        }
        const auto available = candidate.quantity[index] - candidate.reserved[index];
        preview.source_available = std::max(preview.source_available, available);
        if (available >= quantity) {
            const auto rank = std::tuple{mobile_stocks_.contains(id),
                geography::horizontal_distance(stockpile_position(id), point), id};
            if (!best || rank < *best) {
                best = rank;
                source = id;
            }
        }
    }
    if (quantity > preview.room) {
        preview.reason = "destination lacks uncommitted storage space";
    } else if (!source) {
        preview.reason = "no friendly source has enough unreserved stock";
    } else if (quantity > fleet_capacity) {
        preview.reason = "no friendly carrier can hold that quantity";
    } else if (!shipments_.empty() && shipments_.rbegin()->first == ShipmentId{UINT32_MAX}) {
        preview.reason = "shipment ID limit reached";
    } else {
        preview.source = source;
        preview.source_available = stockpiles_.at(*source).quantity[index] - stockpiles_.at(*source).reserved[index];
    }
    return preview;
}

SupplyOrderResult CampaignState::submit_supply(FactionId actor, SupplyTarget target, Resource resource,
                                                std::uint64_t quantity)
{
    const auto preview = preview_supply(actor, target, resource, quantity);
    if (!preview.source) {
        return {{}, preview.reason};
    }
    const auto last = shipments_.empty() ? 0 : static_cast<std::uint32_t>(shipments_.rbegin()->first);
    const ShipmentId id{last + 1};
    request_supply({id, *preview.source, *preview.destination, resource, quantity});
    shipments_.at(id).requester = actor;
    return {id, "supply request queued"};
}

bool CampaignState::can_cancel_supply(FactionId actor, ShipmentId id) const
{
    const auto shipment = shipments_.find(id);
    return can_issue_orders(actor) && shipment != shipments_.end() &&
        shipment->second.requester == actor && !terminal(shipment->second.status);
}

bool CampaignState::cancel_supply(FactionId actor, ShipmentId id)
{
    return can_cancel_supply(actor, id) && cancel_shipment(id);
}

std::vector<SupplyOrderSummary> CampaignState::supply_orders(std::optional<FactionId> perspective) const
{
    std::vector<SupplyOrderSummary> orders;
    for (const auto& [id, shipment] : shipments_) {
        if (shipment.requester && (!perspective || shipment.requester == perspective)) {
            orders.push_back({*shipment.requester, shipment.request, shipment.status, shipment.carrier, shipment.cargo});
        }
    }
    return orders;
}

void CampaignState::queue_supply(SupplyRequest request, std::uint64_t tick)
{
    const auto& origin = stockpiles_.at(request.origin);
    const auto& destination = stockpiles_.at(request.destination);
    const auto index = cargo_index(request.resource);
    if (origin.id == destination.id || origin.owner == FactionId::neutral ||
        origin.owner != destination.owner || request.quantity == 0 ||
        request.quantity > origin.capacity[index] || destination.capacity[index] == 0) {
        throw std::invalid_argument("Invalid supply request");
    }
    const auto [position, inserted] = shipments_.emplace(request.id, Shipment{request});
    if (!inserted) {
        throw std::invalid_argument("Duplicate shipment ID");
    }
    try {
        record_event(logistics_log_, {tick, request.id, LogisticsEventKind::requested, request.quantity});
    } catch (...) {
        shipments_.erase(position);
        throw;
    }
}

void CampaignState::request_replenishment(std::uint64_t tick)
{
    std::map<std::pair<StockpileId, Resource>, std::uint64_t> incoming;
    for (const auto& [id, shipment] : shipments_) {
        if (terminal(shipment.status)) {
            continue;
        }
        const auto key = std::pair{shipment.request.destination, shipment.request.resource};
        const auto& stock = stockpiles_.at(key.first);
        const auto index = cargo_index(key.second);
        auto& total = incoming[key];
        const auto remaining = shipment.cargo ? shipment.cargo : shipment.request.quantity;
        total += std::min(remaining, stock.capacity[index] - total);
    }
    for (const auto& policy : resupply_) {
        const auto& origin = stockpiles_.at(policy.origin);
        const auto& destination = stockpiles_.at(policy.destination);
        if (origin.owner != destination.owner || unavailable(recipient_status(origin.id)) ||
            unavailable(recipient_status(destination.id))) {
            continue;
        }
        const auto index = cargo_index(policy.resource);
        const auto promised = incoming[{destination.id, policy.resource}];
        const auto free = destination.quantity[index] - destination.reserved[index];
        const auto space = destination.capacity[index] - destination.quantity[index];
        if (free >= policy.threshold || promised >= policy.threshold - free || promised >= space) {
            continue;
        }
        std::uint64_t carrier_capacity = 0;
        for (const auto& [id, carrier] : carriers_) {
            if (!carrier.lost && carrier.faction == origin.owner) {
                carrier_capacity = std::max(carrier_capacity, carrier.capacity);
            }
        }
        const auto quantity = std::min({policy.target - free - promised, space - promised,
                                        origin.capacity[index], carrier_capacity});
        if (!quantity) {
            continue;
        }
        const auto previous = shipments_.empty() ? 0 : static_cast<std::uint32_t>(shipments_.rbegin()->first);
        if (previous == std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("Shipment ID limit reached");
        }
        queue_supply({ShipmentId{previous + 1}, origin.id, destination.id,
                      policy.resource, quantity, policy.priority}, tick);
    }
}

void CampaignState::release_pickup_stock(Shipment& shipment)
{
    auto& reserved = stockpiles_.at(shipment.request.origin).reserved;
    reserved[cargo_index(shipment.request.resource)] -= shipment.request.quantity;
    reserved[fuel_index] -= shipment.pickup_fuel;
    shipment.pickup_fuel = 0;
}

void CampaignState::set_route_passable(RouteId id, bool passable)
{
    routes_.at(id).passable = passable;
}

void CampaignState::set_road_closed(std::uint32_t id, bool closed)
{
    const auto road = std::ranges::find(movement_map_.roads, id, &geography::RoadSegment::id);
    if (road == movement_map_.roads.end()) {
        throw std::out_of_range("Unknown geographic road");
    }
    if (closed) {
        closed_roads_.insert(id);
    } else {
        closed_roads_.erase(id);
    }
}

bool CampaignState::cancel_shipment(ShipmentId id)
{
    return cancel_shipment_at(shipments_.at(id), clock_.tick());
}

bool CampaignState::cancel_shipment_at(Shipment& shipment, std::uint64_t tick)
{
    if (terminal(shipment.status)) {
        return false;
    }
    record_event(logistics_log_, {tick, shipment.request.id, LogisticsEventKind::cancelled, shipment.cargo});
    if (shipment.carrier && !shipment.cargo) {
        release_pickup_stock(shipment);
        if (!shipment.journey || !shipment.journey->progress.entry_paid) {
            carriers_.at(*shipment.carrier).shipment.reset();
            shipment.carrier.reset();
            shipment.journey.reset();
            shipment.edge.reset();
            shipment.road.reset();
            shipment.remaining_ticks = 0;
        }
    }
    // A loaded, cancelled shipment stays with its carrier at its current location.
    shipment.status = ShipmentStatus::cancelled;
    shipment.delay = LogisticsDelay::none;
    return true;
}

bool CampaignState::lose_carrier(CarrierId id)
{
    auto& carrier = carriers_.at(id);
    if (carrier.lost) {
        return false;
    }
    if (carrier.shipment) {
        auto& shipment = shipments_.at(*carrier.shipment);
        record_event(logistics_log_, {clock_.tick(), shipment.request.id, LogisticsEventKind::lost, shipment.cargo});
        if (!terminal(shipment.status) && !shipment.cargo) {
            release_pickup_stock(shipment);
        }
        shipment.status = ShipmentStatus::lost;
        shipment.cargo = 0;
        shipment.edge.reset();
        shipment.road.reset();
        shipment.journey.reset();
        shipment.remaining_ticks = 0;
        carrier.shipment.reset();
    }
    carrier.lost = true;
    return true;
}

bool CampaignState::transport_allowed(SectorId sector, FactionId faction) const
{
    const auto controller = sector_state(sector).controller;
    return controller == FactionId::neutral || controller == faction;
}

bool CampaignState::route_usable(const TransportRoute& route, FactionId faction) const
{
    if (!route.passable || !transport_allowed(route.from, faction) || !transport_allowed(route.to, faction)) {
        return false;
    }
    const auto footprint = transport_footprints_.find(route.id);
    return footprint == transport_footprints_.end() ||
        std::ranges::all_of(footprint->second, [&](auto sector) { return transport_allowed(sector, faction); });
}

std::optional<CampaignState::TransportPath> CampaignState::transport_path(
    SectorId from, SectorId to, FactionId faction, bool cheapest, bool* limited) const
{
    // Search backwards and use the first edge ID as the final tie-breaker.
    using Cost = std::pair<std::uint64_t, std::uint64_t>;
    using Visit = std::pair<Cost, SectorId>;
    const auto edge_cost = [cheapest](const TransportRoute& route) -> Cost {
        return cheapest ? Cost{route.fuel_cost, route.travel_ticks} : Cost{route.travel_ticks, route.fuel_cost};
    };
    std::priority_queue<Visit, std::vector<Visit>, std::greater<>> pending;
    std::map<SectorId, Cost> distance{{to, {}}};
    std::map<SectorId, RouteId> first;
    pending.emplace(Cost{}, to);
    while (!pending.empty()) {
        const auto [cost, sector] = pending.top();
        pending.pop();
        if (cost != distance.at(sector)) {
            continue;
        }
        if (sector == from) {
            return TransportPath{
                from == to ? std::nullopt : std::optional{first.at(from)},
                cheapest ? cost.second : cost.first, cheapest ? cost.first : cost.second
            };
        }
        const auto incoming = incoming_routes_.find(sector);
        if (incoming == incoming_routes_.end()) {
            continue;
        }
        for (const auto id : incoming->second) {
            const auto& route = routes_.at(id);
            if (!route_usable(route, faction)) {
                continue;
            }
            const auto leg = edge_cost(route);
            const Cost candidate{cost.first + leg.first, cost.second + leg.second};
            auto [best, inserted] = distance.try_emplace(route.from, candidate);
            if (distance.size() > 20'000) {
                if (limited) {
                    *limited = true;
                }
                return {};
            }
            if (inserted || candidate < best->second) {
                best->second = candidate;
                first.insert_or_assign(route.from, id);
                pending.emplace(candidate, route.from);
            } else if (candidate == best->second && id < first.at(route.from)) {
                first.at(route.from) = id;
            }
        }
    }
    return std::nullopt;
}

geography::Point CampaignState::logistics_anchor(SectorId sector) const
{
    const auto& cell = world_.sector(sector);
    const auto size = movement_map_.sector_mm;
    geography::Point point{cell.x * size + size / 2, cell.y * size + size / 2, 0};
    const auto tile = movement_map_.tiles.find({point.east / movement_map_.tile_mm,
                                               point.south / movement_map_.tile_mm});
    point.elevation = tile == movement_map_.tiles.end() ? movement_map_.default_tile.elevation
                                                       : tile->second.elevation;
    return point;
}

void CampaignState::initialize_logistics_positions()
{
    for (const auto& [id, formation] : combat_units_) {
        mobile_stocks_.emplace(formation.stores, id);
    }
    for (auto& [id, stock] : stockpiles_) {
        if (mobile_stocks_.contains(id)) {
            if (stock.access_point) {
                throw std::invalid_argument("Mobile stockpile cannot have a fixed access point");
            }
            continue;
        }
        if (!stock.access_point) {
            stock.access_point = logistics_anchor(stock.sector);
        }
        if (!movement_map_.valid_position(*stock.access_point) ||
            movement_map_.sector(*stock.access_point) != static_cast<std::uint32_t>(stock.sector)) {
            throw std::invalid_argument("Stockpile access point must be in its sector");
        }
    }
    for (auto& [id, carrier] : carriers_) {
        if (!carrier.position) {
            carrier.position = logistics_anchor(carrier.sector);
        }
        if (!movement_map_.valid_position(*carrier.position) ||
            movement_map_.sector(*carrier.position) != static_cast<std::uint32_t>(carrier.sector)) {
            throw std::invalid_argument("Carrier position must be in its sector");
        }
    }
    if (std::ranges::any_of(movement_map_.roads, [](const auto& road) { return road.slots == 0; })) {
        throw std::invalid_argument("A road must provide at least one transport slot");
    }
}

geography::Point CampaignState::stockpile_position(StockpileId id) const
{
    const auto mobile = mobile_stocks_.find(id);
    if (mobile != mobile_stocks_.end()) {
        return combat_units_.at(mobile->second).position.value();
    }
    return stockpiles_.at(id).access_point.value();
}

GroundStatus CampaignState::recipient_status(StockpileId stock) const
{
    const auto found = mobile_stocks_.find(stock);
    return found == mobile_stocks_.end() ? GroundStatus::holding : combat_units_.at(found->second).status;
}

geography::Route CampaignState::legacy_transport_leg(const TransportRoute& route) const
{
    const auto from = logistics_anchor(route.from);
    const auto to = logistics_anchor(route.to);
    const auto time = bounded_cost(__int128_t{route.travel_ticks} * clock_.tick_seconds() * second_us);
    return single_leg(from, to, time, std::int64_t{route.fuel_cost} * litre_ml);
}

geography::Route CampaignState::local_transport_leg(const Carrier& carrier, geography::Point from,
                                                     geography::Point to) const
{
    if (from == to) {
        return single_leg(from, to, 0, 0);
    }
    const auto sector = static_cast<SectorId>(*movement_map_.sector(from));
    const auto& properties = world_.sector(sector).properties;
    const auto terrain = properties.terrain;
    const auto profile = carrier.mobility.value_or(default_transport_profile());
    const auto cover = terrain == Terrain::water ? geography::Cover::water :
        terrain == Terrain::forest ? geography::Cover::forest : geography::Cover::plain;
    const auto& rate = profile.terrain[static_cast<std::size_t>(cover)];
    const auto distance = geography::horizontal_distance(from, to);
    if ((!properties.traversable && terrain != Terrain::water) ||
        rate.speed_mm_s <= 0 || rate.fuel_ml_km < 0 || !distance || from.elevation != to.elevation) {
        return {};
    }
    return single_leg(from, to,
        bounded_cost((__int128_t{distance} * second_us + rate.speed_mm_s - 1) / rate.speed_mm_s),
        bounded_cost((__int128_t{distance} * rate.fuel_ml_km + 999'999) / 1'000'000));
}

CampaignState::TransportPlan CampaignState::transport_plan(const Carrier& carrier, geography::Point from,
    geography::Point to, std::int64_t budget, bool cheapest) const
{
    const auto start = static_cast<SectorId>(*movement_map_.sector(from));
    const auto end = static_cast<SectorId>(*movement_map_.sector(to));
    if (!transport_allowed(start, carrier.faction) || !transport_allowed(end, carrier.faction)) {
        return {LogisticsDelay::unsafe};
    }
    if (authored_geography_) {
        const auto allowed = [&](std::int64_t sector) {
            return transport_allowed(static_cast<SectorId>(sector), carrier.faction);
        };
        const auto profile = carrier.mobility.value_or(default_transport_profile());
        const geography::RoadAccess road_allowed = [&](std::uint32_t road) {
            return !road_known_closed(carrier.faction, road);
        };
        auto route = geography::find_route(movement_map_, profile, from, to, {},
            cheapest ? geography::RoutePriority::fuel : geography::RoutePriority::time, allowed, road_allowed);
        if (!cheapest && route.status == geography::RouteStatus::found && route.fuel_ml > budget) {
            return transport_plan(carrier, from, to, budget, true);
        }
        if (route.status == geography::RouteStatus::limit_exceeded) {
            return {LogisticsDelay::search_limit};
        }
        if (route.status != geography::RouteStatus::found) {
            return {LogisticsDelay::unreachable};
        }
        if (route.fuel_ml > budget) {
            return {LogisticsDelay::fuel};
        }
        return {LogisticsDelay::none, route.duration_us, route.fuel_ml, {}, std::move(route)};
    }

    if (start == end) {
        auto route = local_transport_leg(carrier, from, to);
        if (route.status != geography::RouteStatus::found) {
            return {LogisticsDelay::unreachable};
        }
        if (route.fuel_ml > budget) {
            return {LogisticsDelay::fuel};
        }
        return {LogisticsDelay::none, route.duration_us, route.fuel_ml, {}, std::move(route)};
    }
    if (carrier.mobility && carrier.mobility->roads[static_cast<std::size_t>(geography::Surface::paved)].speed_mm_s <= 0) {
        return {LogisticsDelay::unreachable};
    }
    const auto at_start = logistics_anchor(start);
    const auto at_end = logistics_anchor(end);
    auto departure = local_transport_leg(carrier, from, at_start);
    const auto approach = local_transport_leg(carrier, at_end, to);
    if (departure.status != geography::RouteStatus::found || approach.status != geography::RouteStatus::found) {
        return {LogisticsDelay::unreachable};
    }
    bool limited = false;
    auto path = transport_path(start, end, carrier.faction, cheapest, &limited);
    if (!path) {
        return {limited ? LogisticsDelay::search_limit : LogisticsDelay::unreachable};
    }
    const auto cost = bounded_cost(__int128_t{path->fuel} * litre_ml + departure.fuel_ml + approach.fuel_ml);
    if (cost > budget) {
        return cheapest ? TransportPlan{LogisticsDelay::fuel} : transport_plan(carrier, from, to, budget, true);
    }
    const auto duration = bounded_cost(__int128_t{path->ticks} * clock_.tick_seconds() * second_us +
                                       departure.duration_us + approach.duration_us);
    if (!departure.legs.empty()) {
        return {LogisticsDelay::none, duration, cost, {}, std::move(departure)};
    }
    return {LogisticsDelay::none, duration, cost, path->first, legacy_transport_leg(routes_.at(*path->first))};
}

CampaignState::TransportPlan CampaignState::delivery_plan(const Carrier& carrier, geography::Point from,
    geography::Point to, geography::Point depot, std::int64_t budget) const
{
    const auto back = transport_plan(carrier, to, depot, budget, true);
    if (back.failure == LogisticsDelay::none) {
        auto outbound = transport_plan(carrier, from, to, budget - back.fuel_ml);
        if (outbound.failure == LogisticsDelay::none) {
            outbound.fuel_ml += back.fuel_ml;
            return outbound;
        }
    }
    return transport_plan(carrier, from, to, budget);
}

bool CampaignState::delay_shipment(Shipment& shipment, std::uint64_t tick, LogisticsDelay reason)
{
    const bool changed = shipment.delay != reason || shipment.status != ShipmentStatus::delayed;
    shipment.delay = reason;
    if (shipment.status == ShipmentStatus::cancelled) {
        return false;
    }
    if (changed) {
        record_event(logistics_log_, {tick, shipment.request.id, LogisticsEventKind::delayed, shipment.cargo});
        shipment.status = ShipmentStatus::delayed;
    }
    return changed;
}

bool CampaignState::advance_transport(Shipment& shipment, Carrier& carrier, geography::Point target,
    std::uint64_t tick, std::map<RouteId, std::uint32_t>& available_slots,
    std::map<std::uint32_t, std::uint32_t>& road_slots)
{
    const bool cancelled = shipment.status == ShipmentStatus::cancelled;
    const bool collecting = shipment.cargo == 0;
    const auto id = shipment.request.id;
    const auto allowed = [&](std::int64_t sector) {
        return transport_allowed(static_cast<SectorId>(sector), carrier.faction);
    };
    auto time = std::int64_t{clock_.tick_seconds()} * second_us;
    auto tick_road = shipment.road;
    bool changed = false;
    const auto finish_journey = [&] {
        shipment.remaining_ticks = 0;
        shipment.edge.reset();
        shipment.road.reset();
        shipment.journey.reset();
    };
    while (time > 0 && (shipment.journey || *carrier.position != target)) {
        if (!shipment.journey) {
            const auto budget = fuel_available(carrier);
            auto plan = collecting ? transport_plan(carrier, *carrier.position, target, budget)
                : delivery_plan(carrier, *carrier.position, target,
                                stockpile_position(shipment.request.origin), budget);
            if (plan.failure != LogisticsDelay::none) {
                return delay_shipment(shipment, tick, plan.failure) || changed;
            }
            if (plan.edge) {
                auto& slots = available_slots.at(*plan.edge);
                if (!slots) {
                    return delay_shipment(shipment, tick, LogisticsDelay::capacity) || changed;
                }
                --slots;
                shipment.edge = plan.edge;
            }
            auto progress = geography::begin(plan.journey, 0);
            shipment.journey = TransportJourney{std::move(plan.journey), std::move(progress), target};
            shipment.remaining_ticks = 0;
        }

        if (!cancelled && !shipment.edge && shipment.journey->destination != target &&
            !shipment.journey->progress.entry_paid) {
            finish_journey();
            continue;
        }
        auto& journey = *shipment.journey;
        auto& progress = journey.progress;
        const auto& leg = journey.route.legs.at(progress.leg);
        if (shipment.edge && !route_usable(routes_.at(*shipment.edge), carrier.faction)) {
            const auto reason = routes_.at(*shipment.edge).passable ? LogisticsDelay::unsafe : LogisticsDelay::closed;
            return delay_shipment(shipment, tick, reason) || changed;
        }
        if (authored_geography_) {
            const bool closed = leg.road && !observe_road(carrier.faction, *leg.road, carrier.sector, tick);
            if (closed || !geography::allows_leg(movement_map_, leg, allowed, progress.leg_elapsed_us)) {
                const auto reason = closed ? LogisticsDelay::closed : LogisticsDelay::unsafe;
                if (!cancelled) {
                    auto alternate = transport_plan(carrier, *carrier.position, target, fuel_available(carrier));
                    if (alternate.failure == LogisticsDelay::none && !alternate.journey.legs.empty()) {
                        finish_journey();
                        auto start = geography::begin(alternate.journey, 0);
                        shipment.journey = TransportJourney{std::move(alternate.journey), std::move(start), target};
                        continue;
                    }
                    if (alternate.failure == LogisticsDelay::search_limit) {
                        return delay_shipment(shipment, tick, LogisticsDelay::search_limit) || changed;
                    }
                }
                return delay_shipment(shipment, tick, reason) || changed;
            }
        }

        const auto balance = fuel_available(carrier);
        if (!progress.entry_paid && leg.fuel_ml > balance) {
            return delay_shipment(shipment, tick, LogisticsDelay::fuel) || changed;
        }
        if (leg.road && shipment.road != leg.road) {
            if (tick_road != leg.road) {
                auto& slots = road_slots.at(*leg.road);
                if (!slots) {
                    return delay_shipment(shipment, tick, LogisticsDelay::capacity) || changed;
                }
                --slots;
                tick_road = leg.road;
            }
            shipment.road = leg.road;
        }

        const auto leg_index = progress.leg;
        const auto slice = std::min(time, leg.duration_us - progress.leg_elapsed_us);
        progress.fuel_ml = balance;
        const auto remainder = geography::advance(movement_map_, journey.route, progress, slice);
        spend_fuel(carrier, balance - progress.fuel_ml);
        progress.fuel_ml = 0;
        carrier.position = progress.position;
        carrier.sector = static_cast<SectorId>(*movement_map_.sector(*carrier.position));
        for (const auto& crossing : progress.crossings) {
            record_event(logistics_log_, {tick, id, LogisticsEventKind::crossed, shipment.cargo,
                static_cast<SectorId>(crossing.to)});
        }
        progress.crossings.clear();
        const auto elapsed = slice - remainder;
        time -= elapsed;
        changed |= elapsed != 0;
        if (!cancelled) {
            shipment.status = ShipmentStatus::en_route;
            shipment.delay = LogisticsDelay::none;
        }

        if (progress.leg == leg_index) {
            if (shipment.edge) {
                const auto remaining = leg.duration_us - progress.leg_elapsed_us;
                const auto tick_us = std::int64_t{clock_.tick_seconds()} * second_us;
                shipment.remaining_ticks = static_cast<std::uint32_t>(
                    remaining / tick_us + (remaining % tick_us != 0));
            }
            return changed;
        }

        const bool arrived = *carrier.position == target;
        if (shipment.edge || (arrived && !cancelled)) {
            record_event(logistics_log_, {tick, id, LogisticsEventKind::arrived, shipment.cargo, carrier.sector});
        }
        if (cancelled || arrived || progress.leg == journey.route.legs.size() || journey.destination != target) {
            finish_journey();
        } else if (journey.route.legs[progress.leg].road != shipment.road) {
            shipment.road.reset();
        }
        if (cancelled) {
            carrier.shipment.reset();
            shipment.carrier.reset();
            shipment.delay = LogisticsDelay::none;
            return true;
        }
    }
    if (*carrier.position == target && !shipment.journey) {
        shipment.status = collecting ? ShipmentStatus::loading : ShipmentStatus::unloading;
        shipment.remaining_ticks = collecting ? loading_ticks_ : unloading_ticks_;
        shipment.delay = LogisticsDelay::none;
        return true;
    }
    return changed;
}

bool CampaignState::advance_shipment(Shipment& shipment, std::uint64_t tick,
                                     std::map<RouteId, std::uint32_t>& available_slots,
                                     std::map<std::uint32_t, std::uint32_t>& road_slots)
{
    const auto& request = shipment.request;
    auto& carrier = carriers_.at(*shipment.carrier);
    auto& origin = stockpiles_.at(request.origin);
    auto& destination = stockpiles_.at(request.destination);
    const auto index = cargo_index(request.resource);
    const bool cancelled = shipment.status == ShipmentStatus::cancelled;
    const auto owner = carrier.faction;
    const bool collecting = shipment.cargo == 0;
    const auto pickup = stockpile_position(origin.id);
    const auto delivery = stockpile_position(destination.id);
    const auto target = collecting ? pickup : delivery;
    if (cancelled) {
        return advance_transport(shipment, carrier, target, tick, available_slots, road_slots);
    }

    const auto recipient = recipient_status(destination.id);
    const auto supplier = collecting ? recipient_status(origin.id) : GroundStatus::holding;
    if (unavailable(recipient) || unavailable(supplier)) {
        return cancel_shipment_at(shipment, tick);
    }
    if (recipient == GroundStatus::withdrawing || supplier == GroundStatus::withdrawing) {
        if (!shipment.journey) {
            shipment.remaining_ticks = 0;
        }
        return delay_shipment(shipment, tick, LogisticsDelay::recipient_unavailable);
    }
    if (origin.owner != owner || destination.owner != owner ||
        !transport_allowed(carrier.sector, owner) || !transport_allowed(destination.sector, owner) ||
        (collecting && !transport_allowed(origin.sector, owner))) {
        return delay_shipment(shipment, tick, LogisticsDelay::unsafe);
    }

    if (!shipment.journey && *carrier.position == target && shipment.remaining_ticks == 0 &&
        (shipment.status == ShipmentStatus::en_route ||
         shipment.delay == LogisticsDelay::recipient_unavailable)) {
        shipment.status = collecting ? ShipmentStatus::loading : ShipmentStatus::unloading;
        shipment.delay = LogisticsDelay::none;
        shipment.remaining_ticks = collecting ? loading_ticks_ : unloading_ticks_;
        return true;
    }
    if (collecting && *carrier.position == pickup && !shipment.journey) {
        if (shipment.remaining_ticks > 1) {
            --shipment.remaining_ticks;
            return true;
        }
        // Recheck the run from the depot if the destination or routes changed during pickup.
        auto& reserved = origin.reserved[fuel_index];
        const auto available = origin.quantity[fuel_index] - reserved + shipment.pickup_fuel;
        const auto budget = fuel_available(carrier, std::min(carrier.tank_capacity - carrier.fuel, available));
        const auto plan = delivery_plan(carrier, pickup, delivery, pickup, budget);
        if (plan.failure != LogisticsDelay::none) {
            return delay_shipment(shipment, tick, plan.failure);
        }
        const auto fuel = litres_needed(plan.fuel_ml - fuel_available(carrier));
        reserved = reserved - shipment.pickup_fuel + fuel;
        shipment.pickup_fuel = fuel;
        if (fuel) {
            ResourceAmounts cost{};
            cost[fuel_index] = fuel;
            if (!consume_at(origin.id, cost, tick, {}, true)) {
                throw std::logic_error("Missing carrier fuel reservation");
            }
            carrier.fuel += fuel;
            shipment.pickup_fuel = 0;
            record_event(logistics_log_, {tick, request.id, LogisticsEventKind::refuelled, fuel, carrier.sector});
        }
        EconomyEntry debit{tick, origin.id, std::nullopt, {}, {}, request.id};
        debit.consumed[index] = request.quantity;
        record_event(economy_ledger_, debit);
        record_event(logistics_log_, {tick, request.id, LogisticsEventKind::loaded, request.quantity});
        origin.quantity[index] -= request.quantity;
        origin.reserved[index] -= request.quantity;
        shipment.cargo = request.quantity;
        shipment.delay = LogisticsDelay::none;
        if (*carrier.position == delivery) {
            shipment.status = ShipmentStatus::unloading;
            shipment.remaining_ticks = unloading_ticks_;
        } else {
            shipment.status = ShipmentStatus::en_route;
            shipment.remaining_ticks = 0;
        }
        return true;
    }

    if (!collecting && *carrier.position == delivery && !shipment.journey) {
        if (shipment.remaining_ticks > 1) {
            --shipment.remaining_ticks;
            return true;
        }
        const auto amount = std::min(shipment.cargo, destination.capacity[index] - destination.quantity[index]);
        if (amount == 0) {
            shipment.remaining_ticks = 0;
            return delay_shipment(shipment, tick, LogisticsDelay::destination_full);
        }
        EconomyEntry credit{tick, destination.id, std::nullopt, {}, {}, request.id};
        credit.produced[index] = amount;
        record_event(economy_ledger_, credit);
        record_event(logistics_log_, {tick, request.id, LogisticsEventKind::unloaded, amount});
        destination.quantity[index] += amount;
        shipment.cargo -= amount;
        shipment.remaining_ticks = 0;
        if (shipment.cargo == 0) {
            record_event(logistics_log_, {tick, request.id, LogisticsEventKind::delivered, request.quantity});
            shipment.status = ShipmentStatus::delivered;
            shipment.delay = LogisticsDelay::none;
            carrier.shipment.reset();
        } else {
            delay_shipment(shipment, tick, LogisticsDelay::destination_full);
        }
        return true;
    }

    shipment.remaining_ticks = shipment.journey ? shipment.remaining_ticks : 0;
    return advance_transport(shipment, carrier, target, tick, available_slots, road_slots);
}

bool CampaignState::update_logistics(std::uint64_t tick)
{
    // A route slot is committed for the whole tick, even if a carrier exits this tick.
    std::map<RouteId, std::uint32_t> available_slots;
    for (const auto& [id, route] : routes_) {
        available_slots.emplace(id, route.slots);
    }
    std::map<std::uint32_t, std::uint32_t> road_slots;
    for (const auto& road : movement_map_.roads) {
        road_slots.emplace(road.id, road.slots);
    }
    for (const auto& [id, shipment] : shipments_) {
        if (shipment.edge) {
            --available_slots.at(*shipment.edge);
        }
        if (shipment.road) {
            --road_slots.at(*shipment.road);
        }
    }
    bool changed = false;
    for (auto& [id, shipment] : shipments_) {
        const bool finishing_empty_leg = shipment.status == ShipmentStatus::cancelled &&
                                         shipment.journey && !shipment.cargo;
        if (shipment.carrier && (!terminal(shipment.status) || finishing_empty_leg)) {
            changed = advance_shipment(shipment, tick, available_slots, road_slots) || changed;
        }
    }
    if (!resupply_.empty()) {
        request_replenishment(tick);
    }
    std::vector<Shipment*> pending;
    for (auto& [id, shipment] : shipments_) {
        if (shipment.status == ShipmentStatus::pending) {
            pending.push_back(&shipment);
        }
    }
    std::ranges::sort(pending, [](const auto* left, const auto* right) {
        const auto& a = left->request;
        const auto& b = right->request;
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        return std::tuple{!a.deadline, a.deadline, a.id} < std::tuple{!b.deadline, b.deadline, b.id};
    });
    for (auto* shipment : pending) {
        const auto& request = shipment->request;
        auto& origin = stockpiles_.at(request.origin);
        const auto& destination = stockpiles_.at(request.destination);
        const auto index = cargo_index(request.resource);
        const auto recipient = recipient_status(destination.id);
        const auto supplier = recipient_status(origin.id);
        if (unavailable(recipient) || unavailable(supplier)) {
            changed |= cancel_shipment_at(*shipment, tick);
            continue;
        }
        if (recipient == GroundStatus::withdrawing || supplier == GroundStatus::withdrawing) {
            shipment->delay = LogisticsDelay::recipient_unavailable;
            continue;
        }
        if (origin.owner != destination.owner || !transport_allowed(origin.sector, origin.owner) ||
            !transport_allowed(destination.sector, origin.owner)) {
            shipment->delay = LogisticsDelay::unsafe;
            continue;
        }
        if (origin.quantity[index] - origin.reserved[index] < request.quantity) {
            continue;
        }
        const auto pickup_point = stockpile_position(origin.id);
        const auto delivery_point = stockpile_position(destination.id);
        const auto free_fuel = origin.quantity[fuel_index] - origin.reserved[fuel_index] -
                               (index == fuel_index ? request.quantity : 0);
        Carrier* chosen = nullptr;
        std::int64_t pickup_time = 0;
        std::uint64_t pickup_fuel = 0;
        auto failure = LogisticsDelay::none;
        const auto note_failure = [&](LogisticsDelay reason) {
            if (failure == LogisticsDelay::none || reason == LogisticsDelay::search_limit) {
                failure = reason;
            }
        };
        for (auto& [id, carrier] : carriers_) {
            if (carrier.lost || carrier.shipment || carrier.faction != origin.owner ||
                carrier.capacity < request.quantity) {
                continue;
            }
            const auto pickup = transport_plan(carrier, *carrier.position, pickup_point, fuel_available(carrier));
            if (pickup.failure != LogisticsDelay::none) {
                note_failure(pickup.failure);
                continue;
            }
            if (chosen && pickup.duration_us >= pickup_time) {
                continue;
            }
            auto at_depot = carrier;
            spend_fuel(at_depot, pickup.fuel_ml);
            const auto budget = fuel_available(at_depot, std::min(at_depot.tank_capacity - at_depot.fuel, free_fuel));
            const auto delivery = delivery_plan(at_depot, pickup_point, delivery_point, pickup_point, budget);
            if (delivery.failure != LogisticsDelay::none) {
                note_failure(delivery.failure);
                continue;
            }
            chosen = &carrier;
            pickup_time = pickup.duration_us;
            pickup_fuel = litres_needed(delivery.fuel_ml - fuel_available(at_depot));
            if (pickup_time == 0) {
                break;
            }
        }
        shipment->delay = chosen ? LogisticsDelay::none : failure;
        if (chosen) {
            record_event(logistics_log_, {tick, request.id, LogisticsEventKind::assigned, request.quantity});
            origin.reserved[index] += request.quantity;
            origin.reserved[fuel_index] += pickup_fuel;
            shipment->pickup_fuel = pickup_fuel;
            chosen->shipment = request.id;
            shipment->carrier = chosen->id;
            if (pickup_time == 0) {
                shipment->status = ShipmentStatus::loading;
                shipment->remaining_ticks = loading_ticks_;
            } else {
                shipment->status = ShipmentStatus::en_route;
                record_event(logistics_log_, {tick, request.id, LogisticsEventKind::collecting, 0, chosen->sector});
            }
            changed = true;
        }
    }
    // A remembered closure can expire while dispatch is pending. A carrier waiting at an
    // obstruction can also refresh its observation even when the rest of the campaign is idle.
    return changed || (authored_geography_ && std::ranges::any_of(shipments_, [&](const auto& entry) {
        const auto& shipment = entry.second;
        if (shipment.status == ShipmentStatus::cancelled) {
            return shipment.journey && shipment.delay == LogisticsDelay::closed;
        }
        if (terminal(shipment.status)) {
            return false;
        }
        const auto faction = stockpiles_.at(shipment.request.origin).owner;
        const auto store = intelligence_.find(faction);
        return store != intelligence_.end() && std::ranges::any_of(store->second.roads, [&](const auto& report) {
            return report.second.closed && aged_road_report(report.second).confirmed();
        });
    }));
}

}  // namespace openwar
