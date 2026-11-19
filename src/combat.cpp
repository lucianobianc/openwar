#include "campaign.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>

namespace openwar {
namespace {

constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
constexpr auto ammo = static_cast<std::size_t>(Resource::ammunition);
constexpr auto supply = static_cast<std::size_t>(Resource::supplies);

std::uint64_t free_stock(const Stockpile& stock, std::size_t resource)
{
    return stock.quantity[resource] - stock.reserved[resource];
}

bool ready(EquipmentStatus status)
{
    return status == EquipmentStatus::available || status == EquipmentStatus::ready ||
           status == EquipmentStatus::reserved || status == EquipmentStatus::in_operation;
}

bool active_ground(MissionStatus status)
{
    return status == MissionStatus::scheduled || status == MissionStatus::in_progress;
}

bool position_access(MissionKind kind, FactionId own, FactionId controller, bool destination)
{
    return controller == own ||
        (kind == MissionKind::move && destination && controller == FactionId::neutral);
}

void decrease(std::uint32_t& value, std::uint32_t amount)
{
    value -= std::min(value, amount);
}

std::uint32_t ground_weight(const Sector& sector)
{
    if (!sector.properties.traversable || sector.properties.terrain == Terrain::water) {
        return 0;
    }
    return sector.properties.terrain == Terrain::forest ? 2 : 1;
}

std::uint64_t ground_fuel_per_weight(const WorldDefinition& world)
{
    return (std::uint64_t{world.sector_size_m()} + 1999) / 2000;
}

constexpr std::int64_t second_us = 1'000'000;
constexpr std::int64_t litre_ml = 1000;

std::uint64_t ceil_divide(std::uint64_t amount, std::uint64_t divisor)
{
    return amount / divisor + (amount % divisor != 0);
}

std::int64_t fuel_balance(const Stockpile& stores, const CombatUnit& combat)
{
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto available = __int128_t{free_stock(stores, fuel)} * litre_ml + combat.movement_fuel_credit_ml;
    return static_cast<std::int64_t>(std::min<__int128_t>(available, maximum));
}

struct GroundPath {
    std::vector<SectorId> sectors;
    bool limit_exceeded = false;
};

GroundPath find_ground_path(const WorldDefinition& world, const FactionView& view,
                            FactionId faction, SectorId from, SectorId to, std::uint64_t budget)
{
    const auto& goal = world.sector(to);
    const auto lower_bound = [&goal](const Sector& sector) {
        return std::uint64_t{std::max(sector.x, goal.x) - std::min(sector.x, goal.x)} +
               std::max(sector.y, goal.y) - std::min(sector.y, goal.y);
    };
    const auto& origin = world.sector(from);
    if (!ground_weight(origin) || lower_bound(origin) > budget) {
        return {};
    }
    using Visit = std::pair<std::uint64_t, SectorId>;
    std::priority_queue<Visit, std::vector<Visit>, std::greater<>> queue;
    std::map<SectorId, SectorId> previous{{from, from}};
    std::map<SectorId, std::uint64_t> distance{{from, 0}};
    queue.emplace(0, from);
    while (!queue.empty()) {
        const auto [cost, sector] = queue.top();
        queue.pop();
        if (cost != distance.at(sector)) {
            continue;
        }
        if (sector == to) {
            std::vector<SectorId> path{sector};
            while (path.back() != from) {
                path.push_back(previous.at(path.back()));
            }
            std::ranges::reverse(path);
            return {std::move(path), false};
        }
        for (const auto neighbor : world.sector(sector).neighbors) {
            const auto& step = world.sector(neighbor);
            const auto weight = ground_weight(step);
            const auto next = cost + weight;
            if (!weight || next + lower_bound(step) > budget) {
                continue;
            }
            const auto report = view.sectors.find(neighbor);
            if (neighbor != to && (report == view.sectors.end() || report->second.confidence < 750 ||
                                   report->second.estimated_controller != faction)) {
                continue;
            }
            const auto [best, inserted] = distance.try_emplace(neighbor, next);
            if (distance.size() > 20'000) {
                return {{}, true};
            }
            if (inserted || next < best->second) {
                best->second = next;
                previous.insert_or_assign(neighbor, sector);
                queue.emplace(next, neighbor);
            }
        }
    }
    return {};
}

}  // namespace

std::string_view ground_status_name(GroundStatus status)
{
    switch (status) {
    case GroundStatus::holding: return "holding";
    case GroundStatus::withdrawing: return "withdrawing";
    case GroundStatus::broken: return "broken";
    case GroundStatus::surrendered: return "surrendered";
    case GroundStatus::destroyed: return "destroyed";
    }
    throw std::invalid_argument("Unknown combat unit status");
}

void CampaignState::initialize_combat(const ScenarioDefinition& scenario)
{
    combat_ = scenario.combat;
    if (!combat_.enabled) {
        return;
    }
    if (!combat_.interval_ticks || !combat_.occupation_seconds || !combat_.critical_occupation_seconds ||
        !combat_.capitulation_seconds) {
        throw std::invalid_argument("Combat and occupation intervals must be positive");
    }
    for (const auto& [id, type] : equipment_types_) {
        if ((type.can_occupy && (type.category == EquipmentCategory::helicopter || !type.ground_speed_kph)) ||
            type.combat_power > 1000) {
            throw std::invalid_argument("Invalid abstract combat equipment profile");
        }
    }
    ground_destinations_ = scenario.ground_destinations;
    for (const auto& [sector, point] : ground_destinations_) {
        if (!movement_map_.valid_position(point) ||
            movement_map_.sector(point) != static_cast<std::uint32_t>(sector) ||
            (!authored_geography_ && point.elevation != 0)) {
            throw std::invalid_argument("Ground objective point is outside its sector");
        }
    }
    std::vector<StockpileId> deployed_stores;
    for (auto combat : scenario.combat_units) {
        const auto& unit = units_.at(combat.unit);
        const auto& stock = stockpiles_.at(combat.stores);
        combat.position = combat.position.value_or(ground_anchor(unit.sector));
        if (!movement_map_.valid_position(*combat.position) ||
            movement_map_.sector(*combat.position) != static_cast<std::uint32_t>(unit.sector) ||
            (!authored_geography_ &&
             (!ground_weight(world_.sector(unit.sector)) || combat.position->elevation != 0)) ||
            stock.owner != unit.faction || stock.sector != unit.sector ||
            combat.morale > 100 || combat.cohesion > 100 || combat.fatigue > 100 ||
            combat.experience > 100 || combat.suppression > 100 ||
            combat.status != GroundStatus::holding || combat.withdrawal || combat.withdrawal_journey ||
            combat.movement_fuel_credit_ml ||
            std::ranges::find(deployed_stores, combat.stores) != deployed_stores.end() ||
            std::ranges::any_of(installations_, [&](const auto& state) {
                return state.installation.stockpile == combat.stores;
            }) || std::ranges::any_of(facilities_, [&](const auto& entry) {
                return entry.second.stockpile == combat.stores;
            }) || !combat_units_.emplace(combat.unit, combat).second) {
            throw std::invalid_argument("Invalid combat unit or shared deployed stockpile");
        }
        deployed_stores.push_back(combat.stores);
    }
    for (const auto& [id, item] : equipment_) {
        if (item.status != EquipmentStatus::reserve && item.status != EquipmentStatus::in_transit &&
            item.status != EquipmentStatus::lost) {
            ++initial_strength_[units_.at(item.assignment.unit).faction];
        }
    }
    victory_objectives_ = scenario.victory_objectives;
    command_objectives_ = scenario.command_objectives;
    const auto validate = [this](const auto& objective) {
        if ((objective.faction != FactionId::west && objective.faction != FactionId::east) ||
            objective.sectors.empty()) {
            throw std::invalid_argument("Invalid war objective");
        }
        for (const auto sector : objective.sectors) {
            world_.sector(sector);
        }
    };
    for (const auto& objective : victory_objectives_) {
        validate(objective);
        for (const auto id : objective.required_installations) {
            if (std::ranges::none_of(installations_, [id](const auto& state) {
                    return state.installation.id == id;
                })) {
                throw std::invalid_argument("Unknown victory installation");
            }
        }
    }
    for (const auto& objective : command_objectives_) {
        validate(objective);
    }
    victory_since_.resize(victory_objectives_.size());
}

OrderResult CampaignState::issue_ground_order(FactionId actor, MissionKind kind, geography::Point destination,
                                              std::optional<UnitId> formation)
{
    return commit_order(preview_order(actor, kind, destination, formation), true);
}

MissionPlan CampaignState::preview_order(FactionId actor, MissionKind kind, geography::Point destination,
                                         std::optional<UnitId> formation) const
{
    if (const auto refusal = order_authority_refusal(actor, formation); !refusal.empty()) {
        return {{}, refusal};
    }
    if (!combat_.enabled || (kind != MissionKind::attack && kind != MissionKind::defend && !position_order(kind))) {
        return {{}, "ground operations unavailable"};
    }
    const auto target = movement_map_.sector(destination);
    if (!target || !movement_map_.valid_position(destination) ||
        (!authored_geography_ && destination.elevation != 0)) {
        return {{}, "ground destination is out of bounds"};
    }
    return plan_ground_order(actor, kind, static_cast<SectorId>(*target), destination, formation);
}

MissionPlan CampaignState::plan_ground_order(FactionId faction, MissionKind kind, SectorId target,
                                            std::optional<geography::Point> destination,
                                             std::optional<UnitId> formation) const
{
    if (!combat_.enabled || war_ended()) {
        return {{}, "ground operations unavailable"};
    }
    const auto view = faction_view(faction, false);
    if (const auto refusal = ground_target_refusal(view, faction, kind, target); !refusal.empty()) {
        return {{}, refusal};
    }
    if (formation && !combat_units_.contains(*formation)) {
        return {{}, "selected formation has no ground deployment"};
    }
    std::string_view refusal = "no eligible supplied ground unit with a known route";
    auto chosen = plan_ground_mission(faction, kind, target, view, {}, destination, &refusal, formation);
    if (!chosen) {
        return {{}, refusal};
    }
    return {std::move(chosen), "feasible"};
}

std::string_view CampaignState::ground_target_refusal(const FactionView& view, FactionId faction,
                                                     MissionKind kind, SectorId target) const
{
    if (!authored_geography_ && !ground_weight(world_.sector(target))) {
        return "ground target is not traversable";
    }
    const auto report = view.sectors.find(target);
    if (position_order(kind)) {
        if (report == view.sectors.end() || report->second.confidence < 750) {
            return "destination lacks confirmed territorial intelligence";
        }
        if (!position_access(kind, faction, report->second.estimated_controller, true)) {
            return kind == MissionKind::hold ? "hold requires a reported friendly destination" :
                                             "move requires a reported friendly or neutral destination";
        }
        return {};
    }
    if (report == view.sectors.end() || report->second.confidence < 750 ||
        (report->second.estimated_controller == faction) != (kind == MissionKind::defend)) {
        return "target lacks suitable confirmed territorial intelligence";
    }
    if (kind == MissionKind::attack && report->second.estimated_controller != FactionId::neutral &&
        !report->second.surveyed &&
        std::ranges::none_of(view.contacts, [target](const auto& contact) {
            return contact.estimated_sector == target && contact.confidence >= 350;
        })) {
        return "attack requires a reported contact in the estimated sector";
    }
    return {};
}

std::optional<Mission> CampaignState::plan_ground_mission(FactionId faction, MissionKind kind, SectorId target,
                                                       const FactionView& view, std::span<const UnitId> excluded,
                                                       std::optional<geography::Point> destination,
                                                       std::string_view* refusal, std::optional<UnitId> formation) const
{
    const auto objective = destination.value_or(ground_anchor(target));
    std::optional<Mission> chosen;
    std::int64_t best_duration = 0;
    for (const auto& [id, combat] : combat_units_) {
        const auto& unit = units_.at(id);
        if (unit.faction != faction || (formation && id != *formation) ||
            std::ranges::find(excluded, id) != excluded.end() || formation_committed(id)) {
            continue;
        }
        const auto equipment = ground_order_equipment(id, kind, formation ? refusal : nullptr);
        if (!equipment) {
            continue;
        }
        const auto speed = ground_speed(id);
        if (!speed) {
            if (formation && refusal)
                *refusal = "selected formation has no available ground mobility";
            continue;
        }
        const auto& stores = stockpiles_.at(combat.stores);
        std::vector<SectorId> path;
        auto route = ground_route(combat, objective, view, path, position_order(kind));
        if (route.status == geography::RouteStatus::limit_exceeded && refusal) {
            *refusal = "ground route search limit exceeded";
        } else if ((formation || position_order(kind)) && refusal) {
            if (formation && route.status != geography::RouteStatus::found)
                *refusal = "selected formation has no route with current faction reports";
            else if (route.status == geography::RouteStatus::found && route.fuel_ml > fuel_balance(stores, combat))
                *refusal = formation ? "selected formation has insufficient fuel for the route" :
                                       "no eligible formation has sufficient fuel for the route";
        }
        if (route.status != geography::RouteStatus::found ||
            route.fuel_ml > fuel_balance(stores, combat) || (chosen && route.duration_us >= best_duration)) {
            continue;
        }
        chosen = Mission{MissionId{}, faction, kind, target, *equipment, combat.stores, 0, 0};
        chosen->ground_path = std::move(path);
        chosen->ground_unit = id;
        chosen->ground_destination = objective;
        chosen->duration_ticks = std::max<std::uint64_t>(1,
            ceil_divide(route.duration_us, clock_.tick_seconds() * second_us));
        best_duration = route.duration_us;
        auto progress = geography::begin(route, 0);
        chosen->ground_journey = GroundJourney{std::move(route), std::move(progress), speed};
    }
    return chosen;
}

std::optional<EquipmentId> CampaignState::ground_order_equipment(UnitId id, MissionKind kind,
                                                                std::string_view* refusal) const
{
    const auto combat = combat_units_.find(id);
    if (combat == combat_units_.end() || combat->second.status != GroundStatus::holding) {
        if (refusal)
            *refusal = "selected ground formation is unavailable";
        return {};
    }
    const auto& stores = stockpiles_.at(combat->second.stores);
    if (!free_stock(stores, supply) || (kind == MissionKind::attack && !free_stock(stores, ammo))) {
        if (refusal)
            *refusal = "selected formation lacks unreserved supplies or ammunition";
        return {};
    }
    const auto equipment = std::ranges::find_if(equipment_, [&](const auto& entry) {
        const auto& item = entry.second;
        if (item.assignment.unit != id || item.status != EquipmentStatus::available) {
            return false;
        }
        const auto& type = equipment_types_.at(item.assignment.type);
        if (position_order(kind)) {
            return type.category != EquipmentCategory::helicopter && type.ground_speed_kph &&
                (kind == MissionKind::move || type.can_occupy || type.combat_power);
        }
        return type.can_occupy;
    });
    if (equipment == equipment_.end()) {
        if (refusal) {
            *refusal = kind == MissionKind::move ? "selected formation has no available mobile ground equipment" :
                kind == MissionKind::hold ? "selected formation has no available ground defensive equipment" :
                "selected formation has no available equipment that can occupy territory";
        }
        return {};
    }
    return equipment->first;
}

MissionId CampaignState::commit_ground_mission(Mission mission)
{
    const auto mission_id = next_mission_id(mission.faction);
    mission.id = mission_id;
    const auto equipment = mission.equipment;
    const auto faction = mission.faction;
    const auto target = mission.target;
    const auto manual = mission.manual;
    auto& committed = missions_.emplace(std::pair{faction, mission_id}, std::move(mission)).first->second;
    reserve_equipment(equipment);
    auto reason = manual ? "player ground order scheduled" : "autonomous ground order scheduled";
    if (committed.kind == MissionKind::hold && committed.ground_journey->route.legs.empty()) {
        committed.status = MissionStatus::holding;
        committed.launch_tick = clock_.tick();
        set_equipment_status(equipment_.at(equipment), EquipmentStatus::in_operation, clock_.tick());
        reason = "holding current position";
    }
    record_event(operations_log_, {clock_.tick(), faction, target, mission_id, reason});
    return mission_id;
}

std::uint32_t CampaignState::ground_speed(UnitId id) const
{
    std::uint32_t speed = 0;
    for (const auto& [equipment_id, item] : equipment_) {
        if (item.assignment.unit != id || !ready(item.status)) {
            continue;
        }
        const auto& type = equipment_types_.at(item.assignment.type);
        if (type.category != EquipmentCategory::helicopter && type.ground_speed_kph &&
            (!speed || type.ground_speed_kph < speed)) {
            speed = type.ground_speed_kph;
        }
    }
    return speed;
}

bool CampaignState::ground_opposition(SectorId sector, FactionId faction) const
{
    return std::ranges::any_of(combat_units_, [&](const auto& entry) {
        const auto& unit = units_.at(entry.first);
        return unit.sector == sector && unit.faction != faction &&
               entry.second.status == GroundStatus::holding;
    });
}

geography::Map CampaignState::movement_geometry() const
{
    return {world_.width(), world_.height(), std::int64_t{world_.sector_size_m()} * 1000, 100'000,
        {}, {}, {}, {}, {}};
}

geography::Point CampaignState::ground_anchor(SectorId sector) const
{
    if (const auto it = ground_destinations_.find(sector); it != ground_destinations_.end()) {
        return it->second;
    }
    const auto& cell = world_.sector(sector);
    const auto size = movement_map_.sector_mm;
    geography::Point point{cell.x * size + size / 2, cell.y * size + size / 2, 0};
    const auto tile = movement_map_.tiles.find({point.east / movement_map_.tile_mm,
                                               point.south / movement_map_.tile_mm});
    point.elevation = tile == movement_map_.tiles.end() ? movement_map_.default_tile.elevation
                                                       : tile->second.elevation;
    return point;
}

geography::Profile CampaignState::ground_profile(UnitId id) const
{
    geography::Profile profile{};
    bool first = true;
    for (const auto& [item_id, item] : equipment_) {
        const auto& type = equipment_types_.at(item.assignment.type);
        if (item.assignment.unit != id || !ready(item.status) ||
            type.category == EquipmentCategory::helicopter || !type.ground_speed_kph) {
            continue;
        }
        const auto speed = std::int64_t{type.ground_speed_kph} * 2500 / 9;
        geography::Profile mobility;
        mobility.terrain = {geography::Rate{speed, 500}, geography::Rate{speed / 2, 1000}, {}};
        mobility.roads = {geography::Rate{speed, 500}, geography::Rate{speed, 500}};
        if (type.mobility) {
            mobility = *type.mobility;
        }
        const auto combine = [first, speed](auto& formation, const auto& equipment) {
            for (std::size_t i = 0; i < formation.size(); ++i) {
                auto rate = equipment[i];
                rate.speed_mm_s = std::min(rate.speed_mm_s, speed);
                rate.uphill_speed_mm_s = rate.uphill_speed_mm_s > 0
                    ? std::min(rate.uphill_speed_mm_s, rate.speed_mm_s) : rate.speed_mm_s;
                auto& combined = formation[i];
                if (first) {
                    combined = rate;
                    continue;
                }
                combined.speed_mm_s = std::min(combined.speed_mm_s, rate.speed_mm_s);
                combined.fuel_ml_km = std::max(combined.fuel_ml_km, rate.fuel_ml_km);
                combined.uphill_speed_mm_s = std::min(combined.uphill_speed_mm_s, rate.uphill_speed_mm_s);
                combined.max_uphill_permille = std::min(combined.max_uphill_permille, rate.max_uphill_permille);
                combined.max_downhill_permille = std::min(combined.max_downhill_permille, rate.max_downhill_permille);
            }
        };
        combine(profile.terrain, mobility.terrain);
        combine(profile.roads, mobility.roads);
        first = false;
    }
    return profile;
}

geography::Route CampaignState::ground_route(const CombatUnit& combat, geography::Point destination,
                                             const FactionView& view, std::vector<SectorId>& path,
                                             bool diagnose_fuel) const
{
    const auto& unit = units_.at(combat.unit);
    const auto target = static_cast<SectorId>(*movement_map_.sector(destination));
    const auto budget = fuel_balance(stockpiles_.at(combat.stores), combat);
    if (authored_geography_) {
        // Restrict the graph with public geography and the faction's reports before choosing the route.
        const geography::SectorAccess allowed = [&](std::int64_t sector) {
            const auto id = static_cast<SectorId>(sector);
            const auto report = view.sectors.find(id);
            return id == unit.sector || id == target || (report != view.sectors.end() &&
                report->second.confidence >= 750 && report->second.estimated_controller == unit.faction);
        };
        const geography::RoadAccess road_allowed = [&](std::uint32_t road) {
            const auto report = view.roads.find(road);
            return report == view.roads.end() || !report->second.confirmed() || !report->second.closed;
        };
        const auto profile = ground_profile(combat.unit);
        auto route = geography::find_route(movement_map_, profile, *combat.position, destination,
            {}, geography::RoutePriority::time, allowed, road_allowed);
        if (route.status == geography::RouteStatus::found && route.fuel_ml > budget) {
            route = geography::find_route(movement_map_, profile, *combat.position, destination,
                {}, geography::RoutePriority::fuel, allowed, road_allowed);
        }
        if (route.status == geography::RouteStatus::found) {
            auto trace = geography::begin(route, route.fuel_ml);
            geography::advance(movement_map_, route, trace, route.duration_us);
            path.push_back(unit.sector);
            for (const auto& crossing : trace.crossings) {
                path.push_back(static_cast<SectorId>(crossing.to));
            }
        }
        return route;
    }

    const auto cost_per_weight = ground_fuel_per_weight(world_) * litre_ml;
    const auto max_weight = diagnose_fuel ? std::numeric_limits<std::uint64_t>::max() : budget / cost_per_weight;
    auto corridor = find_ground_path(world_, view, unit.faction, unit.sector, target, max_weight);
    if (corridor.sectors.empty()) {
        geography::Route route{};
        if (corridor.limit_exceeded) {
            route.status = geography::RouteStatus::limit_exceeded;
        }
        return route;
    }
    path = std::move(corridor.sectors);
    return legacy_ground_route(*combat.position, path, destination, ground_speed(combat.unit));
}

geography::Route CampaignState::legacy_ground_route(geography::Point origin, std::span<const SectorId> steps,
                                                   geography::Point destination, std::uint32_t speed) const
{
    // Legacy terrain weights and whole-litre charges price straight geographic corridors.
    geography::Route route{};
    route.origin = origin;
    auto position = origin;
    const auto cost_per_weight = ground_fuel_per_weight(world_) * litre_ml;
    auto add_leg = [&](SectorId sector, geography::Point point) {
        if (position == point) {
            return;
        }
        const auto distance = geography::horizontal_distance(position, point);
        const auto weight = ground_weight(world_.sector(sector));
        if (!distance || !weight || point.elevation != position.elevation) {
            throw std::invalid_argument("Invalid legacy ground leg");
        }
        const auto duration = ceil_divide(distance * 3600 * weight, speed);
        const auto fuel_ml = static_cast<std::int64_t>(cost_per_weight * weight);
        if (duration > std::uint64_t{std::numeric_limits<std::int64_t>::max()} - route.duration_us) {
            throw std::overflow_error("Legacy geographic journey duration overflow");
        }
        route.legs.push_back({position, point, distance, static_cast<std::int64_t>(duration), fuel_ml, {}});
        route.duration_us += static_cast<std::int64_t>(duration);
        route.fuel_ml += fuel_ml;
        position = point;
    };
    for (std::size_t i = 1; i < steps.size(); ++i) {
        add_leg(steps[i], i + 1 == steps.size() ? destination : ground_anchor(steps[i]));
    }
    if (steps.size() == 1) {
        add_leg(steps.front(), destination);
    }
    route.status = geography::RouteStatus::found;
    return route;
}

std::optional<GroundMovement> CampaignState::ground_movement(UnitId id, bool faction_limited) const
{
    const auto found = combat_units_.find(id);
    if (war_ended() || found == combat_units_.end()) {
        return {};
    }
    const auto& combat = found->second;
    const GroundJourney* journey = combat.withdrawal_journey ? &*combat.withdrawal_journey : nullptr;
    if (!journey) {
        for (const auto& [key, mission] : missions_) {
            if (mission.ground_unit == id && active_ground(mission.status) && mission.ground_journey) {
                journey = &*mission.ground_journey;
                break;
            }
        }
    }
    if (!journey || journey->progress.leg == journey->route.legs.size()) {
        return {};
    }
    const auto& progress = journey->progress;
    const auto& leg = journey->route.legs[progress.leg];
    GroundMovement report{static_cast<SectorId>(*movement_map_.sector(leg.to)),
        static_cast<std::uint32_t>(__int128_t{100} * progress.leg_elapsed_us / leg.duration_us), {}};
    const auto& unit = units_.at(id);
    auto speed = ground_speed(id);
    if (authored_geography_) {
        speed = std::min(speed, journey->planned_speed_kph);
    }
    // A faction ETA estimates travel at the current speed using its own road reports.
    // Suppressing it on unobserved opposition would disclose a contact.
    if (speed && (faction_limited || combat.withdrawal || !ground_opposition(unit.sector, unit.faction))) {
        std::int64_t unpaid = 0;
        bool closed = false;
        for (auto i = progress.leg + progress.entry_paid; i < journey->route.legs.size(); ++i) {
            const auto& remaining = journey->route.legs[i];
            unpaid += remaining.fuel_ml;
            closed |= remaining.road && road_known_closed(unit.faction, *remaining.road);
        }
        if (!closed && unpaid <= fuel_balance(stockpiles_.at(combat.stores), combat)) {
            const auto remaining = journey->route.duration_us - progress.elapsed_us;
            const auto tick_budget = __int128_t{clock_.tick_seconds()} * second_us * speed;
            const auto needed = __int128_t{remaining} * journey->planned_speed_kph;
            const auto seconds = ((needed + tick_budget - 1) / tick_budget) * clock_.tick_seconds();
            if (seconds <= std::numeric_limits<std::uint64_t>::max()) {
                report.eta_seconds = static_cast<std::uint64_t>(seconds);
            }
        }
    }
    return report;
}

CampaignState::GroundStep CampaignState::advance_ground(CombatUnit& combat, GroundJourney& journey, bool withdrawing,
                                                        const geography::SectorAccess& sector_allowed)
{
    const auto& unit = units_.at(combat.unit);
    auto speed = ground_speed(combat.unit);
    if (authored_geography_) {
        speed = std::min(speed, journey.planned_speed_kph);
    }
    if (!speed) {
        return GroundStep::blocked;
    }
    auto& progress = journey.progress;
    const auto prior_time = progress.elapsed_us;
    const auto prior_leg = progress.leg;
    const auto nominal_tick = __int128_t{clock_.tick_seconds()} * second_us * speed / journey.planned_speed_kph;
    const auto time = static_cast<std::int64_t>(std::min<__int128_t>(nominal_tick,
        std::numeric_limits<std::int64_t>::max()));
    const auto starting_fuel = fuel_balance(stockpiles_.at(combat.stores), combat);
    progress.fuel_ml = starting_fuel;
    bool opposed = false;
    bool access_lost = false;
    const auto remainder = geography::advance(movement_map_, journey.route, progress, time,
        [&](const auto& crossing) {
            const auto sector = static_cast<SectorId>(crossing.to);
            opposed = ground_opposition(sector, unit.faction);
            return !opposed;
        }, [&](std::uint32_t road) {
            const auto sector = static_cast<SectorId>(*movement_map_.sector(progress.position));
            return observe_road(unit.faction, road, sector, clock_.tick());
        }, [&](std::int64_t index) {
            access_lost = sector_allowed && !sector_allowed(index);
            return !access_lost;
        });
    const auto spent = starting_fuel - progress.fuel_ml;
    auto credit = std::int64_t{combat.movement_fuel_credit_ml} - spent;
    if (credit < 0) {
        ResourceAmounts movement{};
        movement[fuel] = ceil_divide(-credit, litre_ml);
        if (!consume(combat.stores, movement)) {
            throw std::logic_error("Movement exceeded unreserved fuel");
        }
        // The remainder is the unused part of the last whole litre drawn.
        credit = (litre_ml + credit % litre_ml) % litre_ml;
    }
    combat.movement_fuel_credit_ml = static_cast<std::uint16_t>(credit);
    // Fuel lives in the deployed stock and its sub-litre credit, never in a second tank.
    progress.fuel_ml = 0;
    for (const auto& crossing : progress.crossings) {
        const auto sector = static_cast<SectorId>(crossing.to);
        move_combat_unit(combat, crossing.position);
        const auto offset = static_cast<std::int64_t>((__int128_t{crossing.elapsed_us - prior_time} *
            journey.planned_speed_kph + speed - 1) / speed);
        record_event(battle_log_, {clock_.tick(), sector, "unit moved", combat.unit, 0, 0, {}, offset});
        if (!withdrawing && operations_.enabled) {
            observe(unit.faction, sector);
        }
    }
    progress.crossings.clear();
    move_combat_unit(combat, progress.position);
    if (!withdrawing && progress.leg != prior_leg) {
        // Authored routes split at access points and raster nodes, which do not represent extra exertion.
        const auto advances = authored_geography_ ? std::size_t{0} : progress.leg - prior_leg;
        combat.fatigue = std::min<std::size_t>(100, combat.fatigue + 3 * advances);
    }
    if (progress.leg == journey.route.legs.size()) {
        return GroundStep::arrived;
    }
    if (access_lost) {
        return GroundStep::territorial_access_lost;
    }
    if (opposed && withdrawing) {
        return GroundStep::blocked;
    }
    if (remainder && !opposed) {
        return journey.route.legs[progress.leg].fuel_ml > starting_fuel - spent
            ? GroundStep::no_fuel : GroundStep::blocked;
    }
    return GroundStep::travelling;
}

void CampaignState::move_combat_unit(CombatUnit& combat, geography::Point position)
{
    const auto sector = static_cast<SectorId>(*movement_map_.sector(position));
    combat.position = position;
    units_.at(combat.unit).sector = sector;
    stockpiles_.at(combat.stores).sector = sector;
}

void CampaignState::release_ground_equipment(EquipmentId id)
{
    auto& item = equipment_.at(id);
    if (item.status == EquipmentStatus::reserved) {
        cancel_equipment_reservation(id);
    } else if (item.status == EquipmentStatus::in_operation) {
        const auto interval = equipment_types_.at(item.assignment.type).service_interval_seconds;
        if (item.damage || item.operating_seconds - item.last_service_seconds >= interval) {
            finish_operation(id);
        } else {
            set_equipment_status(item, EquipmentStatus::available, clock_.tick());
        }
    }
}

void CampaignState::update_combat()
{
    const auto tick = clock_.tick();
    if (!surrenders_.empty()) {
        evaluate_war();
        return;
    }
    for (auto& [id, combat] : combat_units_) {
        if (combat.status == GroundStatus::withdrawing && combat.withdrawal) {
            const auto to = *combat.withdrawal;
            const auto faction = units_.at(id).faction;
            if (combat.withdrawal_journey && sector_state(to).controller == faction &&
                !ground_opposition(to, faction)) {
                const auto step = advance_ground(combat, *combat.withdrawal_journey, true);
                if (step == GroundStep::travelling) {
                    continue;
                }
                if (step == GroundStep::arrived) {
                    record_event(battle_log_, {tick, to, "withdrawal completed", id});
                    if (operations_.enabled) {
                        observe(faction, to);
                    }
                }
            }
            combat.withdrawal.reset();
            combat.withdrawal_journey.reset();
            combat.status = GroundStatus::broken;
        }
    }
    for (auto& [key, mission] : missions_) {
        if (!mission.ground_unit || !mission_active(mission.status)) {
            continue;
        }
        auto& combat = combat_units_.at(*mission.ground_unit);
        auto& item = equipment_.at(mission.equipment);
        if (combat.status != GroundStatus::holding || item.status == EquipmentStatus::lost ||
            (mission.status == MissionStatus::scheduled && item.status != EquipmentStatus::reserved) ||
            (position_order(mission.kind) && mission.status != MissionStatus::scheduled &&
             item.status != EquipmentStatus::in_operation)) {
            release_ground_equipment(mission.equipment);
            mission.status = MissionStatus::failed;
            record_event(operations_log_, {tick, mission.faction, mission.target, mission.id, "ground unit unavailable"});
            continue;
        }
        if (mission.status == MissionStatus::scheduled) {
            if (tick < mission.earliest_launch_tick) {
                continue;
            }
            mission.status = MissionStatus::in_progress;
            mission.launch_tick = tick;
            set_equipment_status(item, EquipmentStatus::in_operation, tick);
            record_event(operations_log_, {tick, mission.faction, mission.target, mission.id, "ground operation started"});
        }
        const auto sector = units_.at(combat.unit).sector;
        const auto allowed = [&](SectorId next) {
            return position_access(mission.kind, mission.faction, sector_state(next).controller, next == mission.target);
        };
        if (position_order(mission.kind) && sector == mission.target && !allowed(sector)) {
            observe(mission.faction, sector);
            release_ground_equipment(mission.equipment);
            mission.status = MissionStatus::failed;
            record_event(operations_log_, {tick, mission.faction, mission.target, mission.id,
                "ground order interrupted: territorial access lost"});
            continue;
        }
        if (mission.status == MissionStatus::holding) {
            continue;
        }
        if (mission.ground_journey && !ground_opposition(units_.at(combat.unit).sector, mission.faction)) {
            geography::SectorAccess access;
            if (position_order(mission.kind)) {
                access = [&](std::int64_t index) {
                    const auto next = static_cast<SectorId>(index);
                    if (allowed(next)) {
                        return true;
                    }
                    observe(mission.faction, next);
                    return false;
                };
            }
            const auto step = advance_ground(combat, *mission.ground_journey, false, access);
            if (step == GroundStep::no_fuel || step == GroundStep::blocked || step == GroundStep::territorial_access_lost) {
                release_ground_equipment(mission.equipment);
                mission.status = MissionStatus::failed;
                record_event(operations_log_, {tick, mission.faction, mission.target, mission.id,
                    step == GroundStep::no_fuel ? "movement stopped: no fuel" :
                    step == GroundStep::territorial_access_lost ? "movement stopped: territorial access lost" :
                    "movement stopped: route obstructed"});
            }
        }
    }
    if (tick % combat_.interval_ticks == 0) {
        resolve_engagements();
    }
    update_territory();
    for (auto& [key, mission] : missions_) {
        if (!mission.ground_unit ||
            (mission.status != MissionStatus::in_progress && mission.status != MissionStatus::holding)) {
            continue;
        }
        const auto& combat = combat_units_.at(*mission.ground_unit);
        const auto& unit = units_.at(combat.unit);
        const auto& sector = sector_state(unit.sector);
        if (position_order(mission.kind)) {
            std::string_view reason;
            if (combat.status != GroundStatus::holding || equipment_.at(mission.equipment).status == EquipmentStatus::lost) {
                release_ground_equipment(mission.equipment);
                mission.status = MissionStatus::failed;
                reason = "ground order interrupted: formation or committed equipment unavailable";
            } else if (mission.status == MissionStatus::in_progress && combat.position == mission.ground_destination &&
                       mission.ground_journey->progress.leg == mission.ground_journey->route.legs.size()) {
                if (mission.kind == MissionKind::hold) {
                    mission.status = MissionStatus::holding;
                    reason = "hold destination reached, holding position";
                } else {
                    release_ground_equipment(mission.equipment);
                    mission.status = MissionStatus::completed;
                    reason = "move destination reached";
                }
            }
            if (!reason.empty()) {
                record_event(operations_log_, {tick, mission.faction, mission.target, mission.id, std::string(reason)});
            }
            continue;
        }
        if (combat.status == GroundStatus::holding &&
            (combat.position != mission.ground_destination || unit.sector != mission.target ||
             sector.controller != mission.faction ||
             sector.control == ControlState::contested)) {
            continue;
        }
        release_ground_equipment(mission.equipment);
        mission.status = combat.status == GroundStatus::holding ? MissionStatus::completed : MissionStatus::failed;
        record_event(operations_log_, {tick, mission.faction, mission.target, mission.id,
            mission.status == MissionStatus::completed ? "ground objective secured" : "ground unit withdrew or broke"});
    }
    evaluate_war();
}

void CampaignState::resolve_engagements()
{
    std::map<SectorId, std::vector<UnitId>> by_sector;
    std::map<UnitId, std::vector<EquipmentId>> inventory;
    std::map<UnitId, std::uint64_t> power;
    std::map<UnitId, std::size_t> live;
    for (const auto& [id, item] : equipment_) {
        if (!combat_units_.contains(item.assignment.unit) || item.status == EquipmentStatus::lost ||
            item.status == EquipmentStatus::reserve || item.status == EquipmentStatus::in_transit) {
            continue;
        }
        ++live[item.assignment.unit];
        if (equipment_types_.at(item.assignment.type).category == EquipmentCategory::helicopter &&
            (item.status == EquipmentStatus::in_operation || item.status == EquipmentStatus::returning)) {
            // Equipment away on an air mission is absent from its base sector.
            continue;
        }
        inventory[item.assignment.unit].push_back(id);
        if (ready(item.status)) {
            power[item.assignment.unit] += std::uint64_t{equipment_types_.at(item.assignment.type).combat_power} *
                                           (100 - item.damage);
        }
    }
    const auto tick = clock_.tick();
    for (auto& [id, state] : combat_units_) {
        if (!live[id] && state.status != GroundStatus::destroyed &&
            state.status != GroundStatus::surrendered) {
            state.status = GroundStatus::destroyed;
            record_event(battle_log_, {tick, units_.at(id).sector, "unit destroyed", id});
        }
        if (!inventory[id].empty() && (state.status == GroundStatus::holding || state.status == GroundStatus::broken)) {
            by_sector[units_.at(id).sector].push_back(id);
        }
    }

    for (const auto& [sector, participants] : by_sector) {
        const auto faction_of = [&](UnitId id) { return units_.at(id).faction; };
        const bool clash = std::ranges::any_of(participants, [&](UnitId id) {
            return faction_of(id) != faction_of(participants.front());
        });
        if (!clash) {
            for (const auto id : participants) {
                auto& state = combat_units_.at(id);
                decrease(state.suppression, 10);
                if (std::ranges::any_of(missions_, [id](const auto& entry) {
                        return entry.second.ground_unit == id &&
                               (entry.second.status == MissionStatus::in_progress ||
                                entry.second.status == MissionStatus::holding);
                    })) {
                    state.fatigue = std::min(100u, state.fatigue + 1);
                    if (!free_stock(stockpiles_.at(state.stores), supply)) {
                        decrease(state.morale, 1);
                    }
                    continue;
                }
                decrease(state.fatigue, 3);
                if (free_stock(stockpiles_.at(state.stores), supply)) {
                    state.morale = std::min(100u, state.morale + 2);
                    state.cohesion = std::min(100u, state.cohesion + 3);
                    if (state.status == GroundStatus::broken && power[id] && state.cohesion >= 45 &&
                        sector_state(sector).controller == faction_of(id)) {
                        state.status = GroundStatus::holding;
                        record_event(battle_log_, {tick, sector, "unit reorganized", id});
                    }
                } else {
                    decrease(state.morale, 1);
                }
            }
            continue;
        }

        record_event(battle_log_, {tick, sector, "engagement"});
        std::map<UnitId, std::uint64_t> hits;
        for (const auto id : participants) {
            auto& state = combat_units_.at(id);
            const auto opponent = std::ranges::find_if(participants, [&](UnitId other) {
                return faction_of(other) != faction_of(id);
            });
            auto& stores = stockpiles_.at(state.stores);
            ResourceAmounts cost{};
            cost[ammo] = 1;
            const bool fed = free_stock(stores, supply) != 0;
            cost[supply] = fed ? 1 : 0;
            state.fatigue = std::min(100u, state.fatigue + 5);
            if (!fed) {
                decrease(state.morale, 3);
            }
            if (state.status == GroundStatus::broken || power[id] == 0 || !consume(stores.id, cost)) {
                continue;
            }
            const auto team = (150 + state.morale + state.cohesion + state.experience -
                               state.fatigue / 2 - state.suppression / 2) / 3;
            const bool covered = world_.sector(sector).properties.terrain == Terrain::forest &&
                                 sector_state(sector).controller == faction_of(*opponent);
            const auto strength = power[id] * team / (covered ? 140 : 100) / (fed ? 100 : 180);
            const auto damage = std::clamp<std::uint64_t>(strength + next_random() % 4, 1, 100);
            hits[*opponent] += damage;
            state.experience = std::min(100u, state.experience + 1);
            record_event(battle_log_, {tick, sector, "ammunition expended", id, 1});
        }

        for (const auto id : participants) {
            auto& state = combat_units_.at(id);
            auto remaining = hits[id];
            const auto faction = faction_of(id);
            for (const auto target : inventory[id]) {
                if (!remaining) {
                    break;
                }
                const auto damage = std::min<std::uint64_t>(remaining, 100 - equipment_.at(target).damage);
                remaining -= damage;
                if (damage >= 100 - equipment_.at(target).damage) {
                    lose_equipment(target);
                    record_event(battle_log_, {tick, sector, "equipment lost", id, static_cast<std::uint32_t>(target)});
                    decrease(state.morale, 18);
                    decrease(state.cohesion, 14);
                } else {
                    damage_equipment(target, static_cast<std::uint32_t>(damage));
                    record_event(battle_log_, {tick, sector, "equipment damaged", id, static_cast<std::uint32_t>(damage)});
                }
            }
            if (live[id] == static_cast<std::size_t>(std::ranges::count_if(inventory[id], [&](EquipmentId item) {
                    return equipment_.at(item).status == EquipmentStatus::lost;
                }))) {
                state.status = GroundStatus::destroyed;
                record_event(battle_log_, {tick, sector, "unit destroyed", id});
                continue;
            }
            if (hits[id]) {
                decrease(state.morale, 3);
                decrease(state.cohesion, 5);
                state.suppression = std::min(100u, state.suppression + 12);
            } else {
                decrease(state.suppression, 5);
            }
            const auto& stores = stockpiles_.at(state.stores);
            const bool depleted = !free_stock(stores, ammo) || !free_stock(stores, supply);
            if (!depleted && (!hits[id] || (state.morale >= 45 && state.cohesion >= 45))) {
                continue;
            }
            const auto speed = ground_speed(id);
            if (speed) {
                const auto view = faction_view(faction, false);
                for (const auto neighbor : world_.sector(sector).neighbors) {
                    if (sector_state(neighbor).controller != faction || ground_opposition(neighbor, faction)) {
                        continue;
                    }
                    std::vector<SectorId> path;
                    auto route = ground_route(state, ground_anchor(neighbor), view, path);
                    if (route.status != geography::RouteStatus::found || route.fuel_ml > fuel_balance(stores, state)) {
                        continue;
                    }
                    auto progress = geography::begin(route, 0);
                    state.withdrawal = neighbor;
                    state.withdrawal_journey = GroundJourney{std::move(route), std::move(progress), speed};
                    state.status = GroundStatus::withdrawing;
                    record_event(battle_log_, {tick, sector, "unit withdrawing", id});
                    break;
                }
            }
            if (!state.withdrawal && depleted && state.morale < 25 && state.cohesion < 35) {
                state.status = GroundStatus::surrendered;
                for (const auto item : inventory[id]) {
                    lose_equipment(item);
                }
                record_event(battle_log_, {tick, sector, "unit surrendered: depleted and cut off", id});
            } else if (!state.withdrawal && state.cohesion < 35) {
                state.status = GroundStatus::broken;
                record_event(battle_log_, {tick, sector, "unit broken", id});
            }
        }
        if (operations_.enabled) {
            for (const auto faction : {FactionId::west, FactionId::east}) {
                observe(faction, sector);
            }
        }
    }
}

}  // namespace openwar
