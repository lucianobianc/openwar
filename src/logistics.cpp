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

std::size_t cargo_index(Resource resource)
{
    const auto index = static_cast<std::size_t>(resource);
    if (resource == Resource::oil_reserve || index >= ResourceAmounts{}.size()) {
        throw std::invalid_argument("Invalid transport resource");
    }
    return index;
}

constexpr auto fuel_index = static_cast<std::size_t>(Resource::fuel);

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
            carrier.shipment || carrier.lost || !carriers_.emplace(carrier.id, carrier).second) {
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
    for (const auto& [id, route] : routes_) {
        incoming_routes_[route.to].push_back(id);
    }
}

void CampaignState::request_supply(SupplyRequest request)
{
    queue_supply(request, clock_.tick());
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
        if (origin.owner != destination.owner ||
            std::ranges::any_of(combat_units_, [&](const auto& entry) {
                const auto& unit = entry.second;
                return unit.stores == destination.id &&
                    (unit.status == GroundStatus::destroyed || unit.status == GroundStatus::surrendered);
            })) {
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

bool CampaignState::cancel_shipment(ShipmentId id)
{
    auto& shipment = shipments_.at(id);
    if (terminal(shipment.status)) {
        return false;
    }
    record_event(logistics_log_, {clock_.tick(), id, LogisticsEventKind::cancelled, shipment.cargo});
    if (shipment.carrier && !shipment.cargo) {
        release_pickup_stock(shipment);
        if (!shipment.edge) {
            carriers_.at(*shipment.carrier).shipment.reset();
            shipment.carrier.reset();
        }
    }
    // A loaded, cancelled shipment stays with its carrier at its current location.
    shipment.status = ShipmentStatus::cancelled;
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
        carrier.shipment.reset();
    }
    carrier.lost = true;
    return true;
}

bool CampaignState::route_usable(const TransportRoute& route, FactionId faction) const
{
    const auto from = sector_state(route.from).controller;
    const auto to = sector_state(route.to).controller;
    return route.passable && (from == FactionId::neutral || from == faction) &&
           (to == FactionId::neutral || to == faction);
}

std::optional<CampaignState::TransportPath> CampaignState::transport_path(
    SectorId from, SectorId to, FactionId faction, bool cheapest) const
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

std::optional<CampaignState::TransportPath> CampaignState::reachable_transport_path(
    SectorId from, SectorId to, FactionId faction, std::uint64_t fuel) const
{
    auto path = transport_path(from, to, faction);
    if (!path || path->fuel <= fuel) {
        return path;
    }
    path = transport_path(from, to, faction, true);
    return path->fuel <= fuel ? path : std::nullopt;
}

std::optional<std::uint64_t> CampaignState::delivery_fuel(
    SectorId origin, SectorId destination, FactionId faction, std::uint64_t budget) const
{
    const auto back = transport_path(destination, origin, faction, true);
    if (back && back->fuel <= budget) {
        const auto outbound = reachable_transport_path(origin, destination, faction, budget - back->fuel);
        if (outbound) {
            return outbound->fuel + back->fuel;
        }
    }
    const auto outbound = reachable_transport_path(origin, destination, faction, budget);
    return outbound ? std::optional{outbound->fuel} : std::nullopt;
}

bool CampaignState::advance_shipment(Shipment& shipment, std::uint64_t tick,
                                     std::map<RouteId, std::uint32_t>& available_slots)
{
    const auto& request = shipment.request;
    auto& carrier = carriers_.at(*shipment.carrier);
    auto& origin = stockpiles_.at(request.origin);
    auto& destination = stockpiles_.at(request.destination);
    const auto index = cargo_index(request.resource);
    const bool cancelled = shipment.status == ShipmentStatus::cancelled;
    const auto owner = carrier.faction;
    const auto delay = [&] {
        if (shipment.status == ShipmentStatus::delayed) {
            return false;
        }
        record_event(logistics_log_, {tick, request.id, LogisticsEventKind::delayed, shipment.cargo});
        shipment.status = ShipmentStatus::delayed;
        return true;
    };

    if (!cancelled && (origin.owner != owner || destination.owner != owner ||
        (sector_state(destination.sector).controller != owner &&
         sector_state(destination.sector).controller != FactionId::neutral))) {
        return delay();
    }

    if (!cancelled && !shipment.cargo && carrier.sector == origin.sector && !shipment.edge) {
        if (shipment.remaining_ticks > 1) {
            --shipment.remaining_ticks;
            return true;
        }
        // Recheck the run from the depot if the destination or routes changed during pickup.
        auto& reserved = origin.reserved[fuel_index];
        const auto available = origin.quantity[fuel_index] - reserved + shipment.pickup_fuel;
        const auto budget = carrier.fuel + std::min(carrier.tank_capacity - carrier.fuel, available);
        const auto required_fuel = delivery_fuel(origin.sector, destination.sector, owner, budget);
        if (!required_fuel) {
            return delay();
        }
        const auto fuel = *required_fuel - std::min(*required_fuel, carrier.fuel);
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
        if (carrier.sector == destination.sector) {
            shipment.status = ShipmentStatus::unloading;
            shipment.remaining_ticks = unloading_ticks_;
        } else {
            shipment.status = ShipmentStatus::en_route;
            shipment.remaining_ticks = 0;
        }
        return true;
    }

    const bool collecting = shipment.cargo == 0;
    const auto target = collecting ? origin.sector : destination.sector;
    if (!collecting && carrier.sector == target && !shipment.edge) {
        if (shipment.remaining_ticks > 1) {
            --shipment.remaining_ticks;
            return true;
        }
        const auto amount = std::min(shipment.cargo, destination.capacity[index] - destination.quantity[index]);
        if (amount == 0) {
            shipment.remaining_ticks = 0;
            return delay();
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
            carrier.shipment.reset();
        } else {
            delay();
        }
        return true;
    }

    if (!shipment.edge) {
        auto path = reachable_transport_path(carrier.sector, target, owner, carrier.fuel);
        if (!collecting) {
            const auto back = transport_path(target, origin.sector, owner, true);
            if (back && carrier.fuel >= back->fuel) {
                const auto round_trip = reachable_transport_path(carrier.sector, target, owner, carrier.fuel - back->fuel);
                if (round_trip) {
                    path = round_trip;
                }
            }
        }
        if (!path || !path->first) {
            return delay();
        }
        const auto& route = routes_.at(*path->first);
        auto& slots = available_slots.at(route.id);
        if (slots == 0) {
            return delay();
        }
        --slots;
        carrier.fuel -= route.fuel_cost;
        shipment.edge = route.id;
        shipment.remaining_ticks = route.travel_ticks;
    }
    const auto& route = routes_.at(*shipment.edge);
    if (!route_usable(route, carrier.faction)) {
        return cancelled ? false : delay();
    }
    if (!cancelled) {
        shipment.status = ShipmentStatus::en_route;
    }
    if (shipment.remaining_ticks == 1) {
        record_event(logistics_log_, {tick, request.id, LogisticsEventKind::arrived, shipment.cargo, route.to});
        shipment.remaining_ticks = 0;
        carrier.sector = route.to;
        shipment.edge.reset();
        if (cancelled) {
            carrier.shipment.reset();
            shipment.carrier.reset();
        } else if (carrier.sector == target) {
            shipment.status = collecting ? ShipmentStatus::loading : ShipmentStatus::unloading;
            shipment.remaining_ticks = collecting ? loading_ticks_ : unloading_ticks_;
        }
    } else {
        --shipment.remaining_ticks;
    }
    return true;
}

bool CampaignState::update_logistics(std::uint64_t tick)
{
    // A route slot is committed for the whole tick, even if a carrier exits this tick.
    std::map<RouteId, std::uint32_t> available_slots;
    for (const auto& [id, route] : routes_) {
        available_slots.emplace(id, route.slots);
    }
    for (auto& [id, shipment] : shipments_) {
        if (shipment.edge) {
            --available_slots.at(*shipment.edge);
        }
    }
    bool changed = false;
    for (auto& [id, shipment] : shipments_) {
        const bool finishing_empty_leg = shipment.status == ShipmentStatus::cancelled &&
                                         shipment.edge && !shipment.cargo;
        if (shipment.carrier && (!terminal(shipment.status) || finishing_empty_leg)) {
            changed = advance_shipment(shipment, tick, available_slots) || changed;
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
        const auto friendly = [&](SectorId sector) {
            const auto controller = sector_state(sector).controller;
            return controller == origin.owner || controller == FactionId::neutral;
        };
        if (origin.owner != destination.owner || !friendly(origin.sector) || !friendly(destination.sector) ||
            origin.quantity[index] - origin.reserved[index] < request.quantity) {
            continue;
        }
        if (!transport_path(origin.sector, destination.sector, origin.owner)) {
            continue;
        }
        const auto free_fuel = origin.quantity[fuel_index] - origin.reserved[fuel_index] -
                               (index == fuel_index ? request.quantity : 0);
        Carrier* chosen = nullptr;
        std::uint64_t pickup_ticks = 0;
        std::uint64_t pickup_fuel = 0;
        for (auto& [id, carrier] : carriers_) {
            if (carrier.lost || carrier.shipment || carrier.faction != origin.owner ||
                carrier.capacity < request.quantity) {
                continue;
            }
            const auto pickup = reachable_transport_path(carrier.sector, origin.sector, carrier.faction, carrier.fuel);
            if (!pickup || (chosen && pickup->ticks >= pickup_ticks)) {
                continue;
            }
            const auto on_arrival = carrier.fuel - pickup->fuel;
            const auto budget = on_arrival + std::min(carrier.tank_capacity - on_arrival, free_fuel);
            const auto required_fuel = delivery_fuel(origin.sector, destination.sector, carrier.faction, budget);
            if (!required_fuel) {
                continue;
            }
            chosen = &carrier;
            pickup_ticks = pickup->ticks;
            pickup_fuel = *required_fuel - std::min(*required_fuel, on_arrival);
            if (pickup_ticks == 0) {
                break;
            }
        }
        if (chosen) {
            record_event(logistics_log_, {tick, request.id, LogisticsEventKind::assigned, request.quantity});
            origin.reserved[index] += request.quantity;
            origin.reserved[fuel_index] += pickup_fuel;
            shipment->pickup_fuel = pickup_fuel;
            chosen->shipment = request.id;
            shipment->carrier = chosen->id;
            if (pickup_ticks == 0) {
                shipment->status = ShipmentStatus::loading;
                shipment->remaining_ticks = loading_ticks_;
            } else {
                shipment->status = ShipmentStatus::en_route;
                record_event(logistics_log_, {tick, request.id, LogisticsEventKind::collecting, 0, chosen->sector});
            }
            changed = true;
        }
    }
    return changed;
}

}  // namespace openwar
