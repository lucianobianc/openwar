#include "campaign.hpp"

#include <algorithm>
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

std::vector<SectorId> find_ground_path(const WorldDefinition& world, const FactionView& view,
                                       FactionId faction, SectorId from, SectorId to)
{
    std::queue<SectorId> queue;
    std::map<SectorId, SectorId> previous{{from, from}};
    queue.push(from);
    while (!queue.empty()) {
        const auto sector = queue.front();
        queue.pop();
        if (sector == to) {
            std::vector<SectorId> path{sector};
            while (path.back() != from) {
                path.push_back(previous.at(path.back()));
            }
            std::ranges::reverse(path);
            return path;
        }
        for (const auto neighbor : world.sector(sector).neighbors) {
            const auto report = view.sectors.find(neighbor);
            if (previous.contains(neighbor) || !world.sector(neighbor).properties.traversable ||
                (neighbor != to && (report == view.sectors.end() || report->second.confidence < 750 ||
                                    report->second.estimated_controller != faction))) {
                continue;
            }
            previous.emplace(neighbor, sector);
            queue.push(neighbor);
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
        if ((type.can_occupy && type.category == EquipmentCategory::helicopter) || type.combat_power > 1000) {
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
            combat.status != GroundStatus::holding || combat.withdrawal ||
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
    if (!world_.sector(target).properties.traversable) {
        return {{}, "ground target is not traversable"};
    }
    const auto view = faction_view(faction, false);
    const auto report = view.sectors.find(target);
    if (report == view.sectors.end() || report->second.confidence < 750 ||
        (report->second.estimated_controller == faction) != (kind == MissionKind::defend)) {
        return {{}, "target lacks suitable confirmed territorial intelligence"};
    }
    if (kind == MissionKind::attack && report->second.estimated_controller != FactionId::neutral &&
        !report->second.surveyed &&
        std::ranges::none_of(view.contacts, [target](const auto& contact) {
            return contact.estimated_sector == target && contact.confidence >= 350;
        })) {
        return {{}, "attack requires a reported contact in the estimated sector"};
    }

    std::optional<Mission> chosen;
    for (const auto& [id, combat] : combat_units_) {
        const auto& unit = units_.at(id);
        if (unit.faction != faction || combat.status != GroundStatus::holding ||
            std::ranges::any_of(view.missions, [&](const auto& mission) {
                return (mission.status == MissionStatus::preparing || active_ground(mission.status)) &&
                       equipment_.at(mission.equipment).assignment.unit == id;
            })) {
            continue;
        }
        const auto path = find_ground_path(world_, view, faction, unit.sector, target);
        const auto& stores = stockpiles_.at(combat.stores);
        if (path.empty() || (chosen && path.size() >= chosen->ground_path.size()) ||
            free_stock(stores, fuel) < path.size() - 1 ||
            !free_stock(stores, supply) || (kind == MissionKind::attack && !free_stock(stores, ammo))) {
            continue;
        }
        for (const auto& [item_id, item] : equipment_) {
            if (item.assignment.unit != id || item.status != EquipmentStatus::available ||
                !equipment_types_.at(item.assignment.type).can_occupy) {
                continue;
            }
            chosen = Mission{MissionId{}, faction, kind, target, item_id, combat.stores, 0, 0};
            chosen->ground_path = path;
            chosen->ground_unit = id;
            break;
        }
    }
    if (!chosen) {
        return {{}, "no eligible supplied ground unit with a known route"};
    }
    const auto mission_id = next_mission_id(faction);
    chosen->id = mission_id;
    chosen->manual = manual;
    const auto equipment = chosen->equipment;
    missions_.emplace(std::pair{faction, mission_id}, std::move(*chosen));
    reserve_equipment(equipment);
    record_event(operations_log_, {clock_.tick(), faction, target, mission_id,
        manual ? "player ground order scheduled" : "autonomous ground order scheduled"});
    return {mission_id, "scheduled"};
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
    ResourceAmounts movement{};
    movement[fuel] = 1;
    for (auto& [id, combat] : combat_units_) {
        if (combat.status == GroundStatus::withdrawing && combat.withdrawal) {
            if (sector_state(*combat.withdrawal).controller == units_.at(id).faction &&
                consume(combat.stores, movement)) {
                move_combat_unit(combat, *combat.withdrawal);
                record_event(battle_log_, {tick, *combat.withdrawal, "withdrawal completed", id});
            }
            combat.withdrawal.reset();
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
            mission.status = MissionStatus::in_progress;
            mission.launch_tick = tick;
            set_equipment_status(item, EquipmentStatus::in_operation, tick);
            record_event(operations_log_, {tick, mission.faction, mission.target, mission.id, "ground operation started"});
        }
        const auto sector = units_.at(combat.unit).sector;
        const bool opposed = std::ranges::any_of(combat_units_, [&](const auto& entry) {
            const auto& other = units_.at(entry.first);
            return other.faction != mission.faction && other.sector == sector &&
                   entry.second.status == GroundStatus::holding;
        });
        if (!opposed && mission.ground_leg + 1 < mission.ground_path.size()) {
            if (!consume(combat.stores, movement)) {
                release_ground_equipment(mission.equipment);
                mission.status = MissionStatus::failed;
                record_event(operations_log_, {tick, mission.faction, mission.target, mission.id, "movement stopped: no fuel"});
                continue;
            }
            const auto destination = mission.ground_path[++mission.ground_leg];
            move_combat_unit(combat, destination);
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
            for (const auto neighbor : world_.sector(sector).neighbors) {
                if (free_stock(stores, fuel) && sector_state(neighbor).controller == faction &&
                    world_.sector(neighbor).properties.traversable &&
                    std::ranges::none_of(combat_units_, [&](const auto& entry) {
                        const auto& unit = units_.at(entry.first);
                        return unit.sector == neighbor && unit.faction != faction &&
                               entry.second.status == GroundStatus::holding;
                    })) {
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
