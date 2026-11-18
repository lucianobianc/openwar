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

// At 1 km/h, a unit covers exactly five increments of 1/18 m per second.
constexpr std::uint64_t movement_units_per_metre = 18;

std::uint64_t movement_budget(std::uint32_t speed_kph, std::uint32_t seconds)
{
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return std::min(std::uint64_t{speed_kph} * seconds, maximum / 5) * 5;
}

std::uint64_t movement_distance(const WorldDefinition& world, std::uint64_t weight)
{
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto sector = world.sector_size_m() * movement_units_per_metre;
    return std::min(weight, maximum / sector) * sector;
}

std::uint64_t ceil_divide(std::uint64_t amount, std::uint64_t divisor)
{
    return amount / divisor + (amount % divisor != 0);
}

struct GroundPath {
    std::vector<SectorId> sectors;
    std::uint64_t weight = 0;
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
            return {std::move(path), cost};
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
    std::vector<StockpileId> deployed_stores;
    for (const auto& combat : scenario.combat_units) {
        const auto& unit = units_.at(combat.unit);
        const auto& stock = stockpiles_.at(combat.stores);
        if (stock.owner != unit.faction || stock.sector != unit.sector ||
            combat.morale > 100 || combat.cohesion > 100 || combat.fatigue > 100 ||
            combat.experience > 100 || combat.suppression > 100 ||
            combat.status != GroundStatus::holding || combat.withdrawal || combat.withdrawal_progress ||
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

OrderResult CampaignState::schedule_ground_mission(FactionId faction, MissionKind kind, SectorId target, bool manual)
{
    if (!combat_.enabled || war_ended()) {
        return {{}, "ground operations unavailable"};
    }
    const auto view = faction_view(faction, false);
    if (const auto refusal = ground_target_refusal(view, faction, kind, target); !refusal.empty()) {
        return {{}, refusal};
    }
    auto chosen = plan_ground_mission(faction, kind, target, view, {});
    if (!chosen) {
        return {{}, "no eligible supplied ground unit with a known route"};
    }
    chosen->manual = manual;
    return {commit_ground_mission(std::move(*chosen)), "scheduled"};
}

std::string_view CampaignState::ground_target_refusal(const FactionView& view, FactionId faction,
                                                     MissionKind kind, SectorId target) const
{
    if (!ground_weight(world_.sector(target))) {
        return "ground target is not traversable";
    }
    const auto report = view.sectors.find(target);
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
                                                       const FactionView& view, std::span<const UnitId> excluded) const
{
    std::optional<Mission> chosen;
    std::uint64_t best_seconds = 0;
    for (const auto& [id, combat] : combat_units_) {
        const auto& unit = units_.at(id);
        if (unit.faction != faction || combat.status != GroundStatus::holding ||
            std::ranges::find(excluded, id) != excluded.end() ||
            std::ranges::any_of(view.missions, [&](const auto& mission) {
                return (mission.status == MissionStatus::preparing || active_ground(mission.status)) &&
                       equipment_.at(mission.equipment).assignment.unit == id;
            })) {
            continue;
        }
        const auto speed = ground_speed(id);
        if (!speed) {
            continue;
        }
        const auto& stores = stockpiles_.at(combat.stores);
        if (!free_stock(stores, supply) || (kind == MissionKind::attack && !free_stock(stores, ammo))) {
            continue;
        }
        const auto budget = free_stock(stores, fuel) / ground_fuel_per_weight(world_);
        auto path = find_ground_path(world_, view, faction, unit.sector, target, budget);
        const auto seconds = ceil_divide(movement_distance(world_, path.weight), movement_budget(speed, 1));
        if (path.sectors.empty() || (chosen && seconds >= best_seconds)) {
            continue;
        }
        for (const auto& [item_id, item] : equipment_) {
            if (item.assignment.unit != id || item.status != EquipmentStatus::available ||
                !equipment_types_.at(item.assignment.type).can_occupy) {
                continue;
            }
            chosen = Mission{MissionId{}, faction, kind, target, item_id, combat.stores, 0, 0};
            chosen->ground_path = std::move(path.sectors);
            chosen->ground_unit = id;
            chosen->duration_ticks = std::max<std::uint64_t>(1, ceil_divide(seconds, clock_.tick_seconds()));
            best_seconds = seconds;
            break;
        }
    }
    return chosen;
}

MissionId CampaignState::commit_ground_mission(Mission mission)
{
    const auto mission_id = next_mission_id(mission.faction);
    mission.id = mission_id;
    const auto equipment = mission.equipment;
    const auto faction = mission.faction;
    const auto target = mission.target;
    const auto manual = mission.manual;
    missions_.emplace(std::pair{faction, mission_id}, std::move(mission));
    reserve_equipment(equipment);
    record_event(operations_log_, {clock_.tick(), faction, target, mission_id,
        manual ? "player ground order scheduled" : "autonomous ground order scheduled"});
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

std::optional<GroundMovement> CampaignState::ground_movement(UnitId id) const
{
    const auto combat = combat_units_.find(id);
    if (war_ended() || combat == combat_units_.end()) {
        return std::nullopt;
    }
    std::span<const SectorId> remaining;
    std::uint64_t progress = 0;
    const bool withdrawing = combat->second.status == GroundStatus::withdrawing && combat->second.withdrawal;
    if (withdrawing) {
        remaining = {&*combat->second.withdrawal, 1};
        progress = combat->second.withdrawal_progress;
    } else {
        for (const auto& [key, mission] : missions_) {
            if (mission.ground_unit == id && active_ground(mission.status) &&
                mission.ground_leg + 1 < mission.ground_path.size()) {
                remaining = std::span{mission.ground_path}.subspan(mission.ground_leg + 1);
                progress = mission.ground_progress;
                break;
            }
        }
    }
    if (remaining.empty()) {
        return std::nullopt;
    }
    const auto first_weight = ground_weight(world_.sector(remaining.front()));
    const auto leg_distance = movement_distance(world_, first_weight);
    GroundMovement report{remaining.front(), static_cast<std::uint32_t>(100 * progress / leg_distance), {}};
    const auto& unit = units_.at(id);
    const auto budget = movement_budget(ground_speed(id), clock_.tick_seconds());
    if (budget && (withdrawing || !ground_opposition(unit.sector, unit.faction))) {
        std::uint64_t weight = 0;
        for (const auto sector : remaining) {
            weight += ground_weight(world_.sector(sector));
        }
        const auto unpaid = progress ? weight - first_weight : weight;
        const auto& stock = stockpiles_.at(combat->second.stores);
        if (free_stock(stock, fuel) / ground_fuel_per_weight(world_) >= unpaid) {
            const auto ticks = ceil_divide(movement_distance(world_, weight) - progress, budget);
            if (ticks <= std::numeric_limits<std::uint64_t>::max() / clock_.tick_seconds()) {
                report.eta_seconds = ticks * clock_.tick_seconds();
            }
        }
    }
    return report;
}

CampaignState::GroundStep CampaignState::advance_ground_leg(
    CombatUnit& unit, SectorId to, std::uint64_t& progress, std::uint64_t& budget)
{
    const auto weight = ground_weight(world_.sector(to));
    if (!progress) {
        ResourceAmounts movement{};
        movement[fuel] = ground_fuel_per_weight(world_) * weight;
        if (!consume(unit.stores, movement)) {
            return GroundStep::no_fuel;
        }
    }
    const auto distance = movement_distance(world_, weight);
    const auto step = std::min(budget, distance - progress);
    budget -= step;
    progress += step;
    if (progress < distance) {
        return GroundStep::travelling;
    }
    progress = 0;
    move_combat_unit(unit, to);
    return GroundStep::arrived;
}

void CampaignState::move_combat_unit(CombatUnit& combat, SectorId sector)
{
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
            auto budget = movement_budget(ground_speed(id), clock_.tick_seconds());
            if (budget && sector_state(to).controller == faction && !ground_opposition(to, faction)) {
                const auto step = advance_ground_leg(combat, to, combat.withdrawal_progress, budget);
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
            combat.withdrawal_progress = 0;
            combat.status = GroundStatus::broken;
        }
    }
    for (auto& [key, mission] : missions_) {
        if (!mission.ground_unit || !active_ground(mission.status)) {
            continue;
        }
        auto& combat = combat_units_.at(*mission.ground_unit);
        auto& item = equipment_.at(mission.equipment);
        if (combat.status != GroundStatus::holding || item.status == EquipmentStatus::lost ||
            (mission.status == MissionStatus::scheduled && item.status != EquipmentStatus::reserved)) {
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
        auto budget = movement_budget(ground_speed(combat.unit), clock_.tick_seconds());
        while (budget && mission.ground_leg + 1 < mission.ground_path.size() &&
               !ground_opposition(units_.at(combat.unit).sector, mission.faction)) {
            const auto destination = mission.ground_path[mission.ground_leg + 1];
            const auto step = advance_ground_leg(combat, destination, mission.ground_progress, budget);
            if (step == GroundStep::no_fuel) {
                release_ground_equipment(mission.equipment);
                mission.status = MissionStatus::failed;
                record_event(operations_log_, {tick, mission.faction, mission.target, mission.id, "movement stopped: no fuel"});
                break;
            }
            if (step == GroundStep::travelling) {
                break;
            }
            ++mission.ground_leg;
            combat.fatigue = std::min(100u, combat.fatigue + 3);
            record_event(battle_log_, {tick, destination, "unit moved", combat.unit});
            if (operations_.enabled) {
                observe(mission.faction, destination);
            }
        }
    }
    if (tick % combat_.interval_ticks == 0) {
        resolve_engagements();
    }
    update_territory();
    for (auto& [key, mission] : missions_) {
        if (!mission.ground_unit || mission.status != MissionStatus::in_progress) {
            continue;
        }
        const auto& combat = combat_units_.at(*mission.ground_unit);
        const auto& unit = units_.at(combat.unit);
        const auto& sector = sector_state(unit.sector);
        if (combat.status == GroundStatus::holding &&
            (unit.sector != mission.target || sector.controller != mission.faction ||
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
                               entry.second.status == MissionStatus::in_progress;
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
            for (const auto neighbor : world_.sector(sector).neighbors) {
                const auto weight = ground_weight(world_.sector(neighbor));
                if (speed && weight && free_stock(stores, fuel) / ground_fuel_per_weight(world_) >= weight &&
                    sector_state(neighbor).controller == faction && !ground_opposition(neighbor, faction)) {
                    state.withdrawal = neighbor;
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
