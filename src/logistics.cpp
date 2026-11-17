#include "campaign.hpp"

#include <algorithm>
#include <functional>
#include <queue>
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
    for (const auto& carrier : scenario.carriers) {
        world_.sector(carrier.sector);
        faction_name(carrier.faction);
        if (carrier.faction == FactionId::neutral || carrier.capacity == 0 ||
            carrier.shipment || carrier.lost || !carriers_.emplace(carrier.id, carrier).second) {
            throw std::invalid_argument("Invalid or duplicate carrier");
        }
    }
}

void CampaignState::request_supply(SupplyRequest request)
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
        logistics_log_.push_back({clock_.tick(), request.id, LogisticsEventKind::requested, request.quantity});
    } catch (...) {
        shipments_.erase(position);
        throw;
    }
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
    logistics_log_.push_back({clock_.tick(), id, LogisticsEventKind::cancelled, shipment.cargo});
    if (shipment.status == ShipmentStatus::loading) {
        auto& stock = stockpiles_.at(shipment.request.origin);
        stock.reserved[cargo_index(shipment.request.resource)] -= shipment.request.quantity;
        carriers_.at(*shipment.carrier).shipment.reset();
        shipment.carrier.reset();
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
        logistics_log_.push_back({clock_.tick(), shipment.request.id, LogisticsEventKind::lost, shipment.cargo});
        if (shipment.status == ShipmentStatus::loading) {
            auto& stock = stockpiles_.at(shipment.request.origin);
            stock.reserved[cargo_index(shipment.request.resource)] -= shipment.request.quantity;
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

std::optional<RouteId> CampaignState::next_route(SectorId from, SectorId to, FactionId faction) const
{
    // Search backwards so the first leg can be selected by route ID on equal costs.
    using Visit = std::pair<std::uint64_t, SectorId>;
    std::priority_queue<Visit, std::vector<Visit>, std::greater<>> pending;
    std::map<SectorId, std::uint64_t> distance{{to, 0}};
    pending.emplace(0, to);
    while (!pending.empty()) {
        const auto [cost, sector] = pending.top();
        pending.pop();
        if (cost != distance.at(sector)) {
            continue;
        }
        if (sector == from) {
            for (const auto& [id, route] : routes_) {
                const auto next = distance.find(route.to);
                if (route.from == from && route_usable(route, faction) &&
                    next != distance.end() && next->second < cost &&
                    cost - next->second == route.travel_ticks) {
                    return id;
                }
            }
        }
        for (const auto& [id, route] : routes_) {
            if (route.to != sector || !route_usable(route, faction)) {
                continue;
            }
            const auto candidate = cost + route.travel_ticks;
            auto [best, inserted] = distance.try_emplace(route.from, candidate);
            if (inserted || candidate < best->second) {
                best->second = candidate;
                pending.emplace(candidate, route.from);
            }
        }
    }
    return std::nullopt;
}

bool CampaignState::advance_shipment(Shipment& shipment, std::uint64_t tick,
                                     std::map<RouteId, std::uint32_t>& available_slots)
{
    const auto& request = shipment.request;
    auto& carrier = carriers_.at(*shipment.carrier);
    const auto index = cargo_index(request.resource);
    const auto delay = [&] {
        if (shipment.status == ShipmentStatus::delayed) {
            return false;
        }
        logistics_log_.push_back({tick, request.id, LogisticsEventKind::delayed, shipment.cargo});
        shipment.status = ShipmentStatus::delayed;
        return true;
    };

    if (shipment.status == ShipmentStatus::loading) {
        if (shipment.remaining_ticks > 1) {
            --shipment.remaining_ticks;
            return true;
        }
        auto& origin = stockpiles_.at(request.origin);
        EconomyEntry debit{tick, origin.id, std::nullopt, {}, {}, request.id};
        debit.consumed[index] = request.quantity;
        economy_ledger_.push_back(debit);
        logistics_log_.push_back({tick, request.id, LogisticsEventKind::loaded, request.quantity});
        origin.quantity[index] -= request.quantity;
        origin.reserved[index] -= request.quantity;
        shipment.cargo = request.quantity;
        shipment.status = ShipmentStatus::en_route;
        shipment.remaining_ticks = 0;
        return true;
    }

    auto& destination = stockpiles_.at(request.destination);
    if (carrier.sector == destination.sector && !shipment.edge) {
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
        economy_ledger_.push_back(credit);
        logistics_log_.push_back({tick, request.id, LogisticsEventKind::unloaded, amount});
        destination.quantity[index] += amount;
        shipment.cargo -= amount;
        shipment.remaining_ticks = 0;
        if (shipment.cargo == 0) {
            logistics_log_.push_back({tick, request.id, LogisticsEventKind::delivered, request.quantity});
            shipment.status = ShipmentStatus::delivered;
            carrier.shipment.reset();
        } else {
            delay();
        }
        return true;
    }

    if (!shipment.edge) {
        const auto route_id = next_route(carrier.sector, destination.sector, carrier.faction);
        if (!route_id) {
            return delay();
        }
        const auto& route = routes_.at(*route_id);
        auto& slots = available_slots.at(*route_id);
        if (slots == 0 || carrier.fuel < route.fuel_cost) {
            return delay();
        }
        --slots;
        carrier.fuel -= route.fuel_cost;
        shipment.edge = route_id;
        shipment.remaining_ticks = route.travel_ticks;
    }
    const auto& route = routes_.at(*shipment.edge);
    if (!route_usable(route, carrier.faction)) {
        return delay();
    }
    shipment.status = ShipmentStatus::en_route;
    if (--shipment.remaining_ticks == 0) {
        carrier.sector = route.to;
        shipment.edge.reset();
        if (carrier.sector == destination.sector) {
            shipment.status = ShipmentStatus::unloading;
            shipment.remaining_ticks = unloading_ticks_;
        }
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
    std::vector<Shipment*> pending;
    for (auto& [id, shipment] : shipments_) {
        if (shipment.edge) {
            --available_slots.at(*shipment.edge);
        }
        if (shipment.status == ShipmentStatus::pending) {
            pending.push_back(&shipment);
        }
    }
    bool changed = false;
    for (auto& [id, shipment] : shipments_) {
        if (shipment.carrier && !terminal(shipment.status)) {
            changed = advance_shipment(shipment, tick, available_slots) || changed;
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
        if (origin.quantity[index] - origin.reserved[index] < request.quantity ||
            (origin.sector != destination.sector && !next_route(origin.sector, destination.sector, origin.owner))) {
            continue;
        }
        for (auto& [id, carrier] : carriers_) {
            if (carrier.lost || carrier.shipment || carrier.faction != origin.owner ||
                carrier.sector != origin.sector || carrier.capacity < request.quantity) {
                continue;
            }
            logistics_log_.push_back({tick, request.id, LogisticsEventKind::assigned, request.quantity});
            origin.reserved[index] += request.quantity;
            carrier.shipment = request.id;
            shipment->carrier = id;
            shipment->status = ShipmentStatus::loading;
            shipment->remaining_ticks = loading_ticks_;
            changed = true;
            break;
        }
    }
    return changed;
}

}  // namespace openwar
