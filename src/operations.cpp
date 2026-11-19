#include "campaign.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace openwar {
namespace {

constexpr auto fuel_index = static_cast<std::size_t>(Resource::fuel);

bool known_friendly(const FactionView& view, FactionId faction, SectorId target)
{
    const auto found = view.sectors.find(target);
    return found != view.sectors.end() && found->second.confidence >= 750 &&
           found->second.estimated_controller == faction;
}

struct Plan {
    SectorId target;
    MissionKind kind;
    std::uint64_t score;
    std::string_view reason;
};

// This ranking has no access to the campaign or to true enemy positions.
std::vector<Plan> rank_objectives(const FactionView& view, FactionId faction, bool combat)
{
    std::vector<Plan> plans;
    for (const auto& objective : view.objectives) {
        const auto sector = view.sectors.find(objective.sector);
        const auto confidence = sector == view.sectors.end() ? 0 : sector->second.confidence;
        const bool stale_contact = std::ranges::any_of(view.contacts, [&](const auto& contact) {
            return sector != view.sectors.end() && current_contact(contact, sector->second) &&
                   contact.confidence < 750;
        });
        const bool need_hostile_survey = combat && sector != view.sectors.end() &&
            !sector->second.surveyed && sector->second.estimated_controller != faction &&
            sector->second.estimated_controller != FactionId::neutral;
        const auto priority = std::uint64_t{objective.priority} * 1000;
        if (confidence < 750 || stale_contact || need_hostile_survey) {
            plans.push_back({objective.sector, MissionKind::recon, priority + 1000 - confidence,
                             "planner: refresh incomplete intelligence"});
        } else if (known_friendly(view, faction, objective.sector)) {
            const auto kind = objective.priority > 1 ? MissionKind::defend : MissionKind::patrol;
            plans.push_back({objective.sector, kind, priority, "planner: cover a friendly objective"});
        } else if (combat) {
            plans.push_back({objective.sector, MissionKind::attack, priority,
                             "planner: contest a confirmed objective"});
        }
    }
    std::ranges::sort(plans, [](const auto& left, const auto& right) {
        return left.score != right.score ? left.score > right.score : left.target < right.target;
    });
    return plans;
}

}  // namespace

std::string_view mission_name(MissionKind kind)
{
    switch (kind) {
    case MissionKind::recon: return "recon";
    case MissionKind::patrol: return "patrol";
    case MissionKind::defend: return "defend";
    case MissionKind::reposition: return "reposition";
    case MissionKind::attack: return "attack";
    case MissionKind::move: return "move";
    case MissionKind::hold: return "hold";
    case MissionKind::ground_recon: return "ground-recon";
    case MissionKind::ground_patrol: return "ground-patrol";
    }
    throw std::invalid_argument("Unknown mission kind");
}

std::string_view mission_status_name(MissionStatus status)
{
    switch (status) {
    case MissionStatus::scheduled: return "scheduled";
    case MissionStatus::preparing: return "preparing";
    case MissionStatus::in_progress: return "in progress";
    case MissionStatus::completed: return "completed";
    case MissionStatus::failed: return "failed";
    case MissionStatus::cancelled: return "cancelled";
    case MissionStatus::holding: return "holding";
    case MissionStatus::released: return "released";
    case MissionStatus::observing: return "observing";
    }
    throw std::invalid_argument("Unknown mission status");
}

std::string_view ground_recon_result(const Mission& mission)
{
    switch (mission.recon_result) {
    case ReconResult::none: return "no objective observation";
    case ReconResult::partial: return "partial observation, territorial report only";
    case ReconResult::observed: return mission.detections ? "contacts detected" : "no contacts detected";
    }
    throw std::invalid_argument("Unknown ground reconnaissance result");
}

bool CampaignState::can_issue_orders(FactionId actor) const
{
    return operations_.enabled && operations_.player == actor && !war_ended();
}

std::optional<bool> CampaignState::operational_reserve(FactionId actor, UnitId id) const
{
    const auto unit = units_.find(id);
    if (!can_issue_orders(actor) || unit == units_.end() || unit->second.faction != actor) {
        return std::nullopt;
    }
    return operational_reserve_.contains(id);
}

bool CampaignState::set_operational_reserve(FactionId actor, UnitId id, bool reserved)
{
    if (!operational_reserve(actor, id).has_value()) {
        return false;
    }
    if (reserved) {
        operational_reserve_.insert(id);
    } else {
        operational_reserve_.erase(id);
    }
    return true;
}

bool CampaignState::formation_committed(UnitId id) const
{
    const auto faction = units_.at(id).faction;
    const auto first = missions_.lower_bound({faction, MissionId{}});
    const auto last = missions_.upper_bound({faction, MissionId{std::numeric_limits<std::uint32_t>::max()}});
    return std::ranges::any_of(first, last, [&](const auto& entry) {
        const auto& mission = entry.second;
        return mission_active(mission.status) && equipment_.at(mission.equipment).assignment.unit == id;
    });
}

std::string_view CampaignState::order_authority_refusal(FactionId actor, std::optional<UnitId> formation) const
{
    if (!can_issue_orders(actor))
        return "player has no authority for this faction";
    if (formation) {
        const auto unit = units_.find(*formation);
        if (unit == units_.end() || unit->second.faction != actor)
            return "selected formation is not under your command";
        if (formation_committed(*formation))
            return "selected formation already has an active order";
    }
    return {};
}

std::vector<MissionKind> CampaignState::available_orders(FactionId actor, UnitId id) const
{
    if (!order_authority_refusal(actor, id).empty()) {
        return {};
    }
    const auto unit = units_.find(id);

    std::vector<MissionKind> actions;
    if (combat_.enabled && ground_speed(id)) {
        for (const auto kind : {MissionKind::defend, MissionKind::attack, MissionKind::move, MissionKind::hold,
                                MissionKind::ground_recon, MissionKind::ground_patrol}) {
            if (ground_order_equipment(id, kind)) {
                actions.push_back(kind);
            }
        }
    }
    if (!unit->second.facility) {
        return actions;
    }
    const auto& facility = facilities_.at(*unit->second.facility);
    const auto& stock = stockpiles_.at(facility.stockpile);
    if (!facility.operational || stock.owner != actor) {
        return actions;
    }
    const bool available = std::ranges::any_of(equipment_, [&](const auto& entry) {
        const auto& item = entry.second;
        const auto& type = equipment_types_.at(item.assignment.type);
        return item.assignment.unit == id && item.status == EquipmentStatus::available && type.operation_range &&
            (facility.supported_types.empty() || std::ranges::find(facility.supported_types, type.id) !=
                facility.supported_types.end());
    });
    if (available) {
        actions.insert(actions.end(), {MissionKind::recon, MissionKind::patrol});
        if (!combat_.enabled) {
            actions.push_back(MissionKind::defend);
        }
        if (!combat_units_.contains(id) && std::ranges::count_if(equipment_, [&](const auto& entry) {
            return entry.second.assignment.unit == id && entry.second.status != EquipmentStatus::lost;
        }) == 1) {
            actions.push_back(MissionKind::reposition);
        }
    }
    return actions;
}

MissionPlan CampaignState::preview_order(FactionId actor, MissionKind kind, SectorId target,
                                         std::optional<UnitId> formation) const
{
    if (const auto refusal = order_authority_refusal(actor, formation); !refusal.empty()) {
        return {{}, refusal};
    }
    return plan_sector_order(actor, kind, target, formation);
}

OrderResult CampaignState::issue_order(FactionId actor, MissionKind kind, SectorId target,
                                       std::optional<UnitId> formation)
{
    return commit_order(preview_order(actor, kind, target, formation), true);
}

OrderResult CampaignState::schedule_mission(FactionId faction, MissionKind kind, SectorId target, bool manual)
{
    return commit_order(plan_sector_order(faction, kind, target), manual);
}

MissionPlan CampaignState::plan_sector_order(FactionId faction, MissionKind kind, SectorId target,
                                              std::optional<UnitId> formation) const
{
    mission_name(kind);
    if (static_cast<std::uint32_t>(target) >= world_.sectors().size()) {
        return {{}, "target sector is out of bounds"};
    }
    if (position_order(kind)) {
        return {{}, kind == MissionKind::ground_patrol ? "ground patrol requires geographic points" :
                                                        "move and hold require an exact geographic destination"};
    }
    if (kind == MissionKind::attack || kind == MissionKind::ground_recon ||
        (combat_.enabled && kind == MissionKind::defend)) {
        return plan_ground_order(faction, kind, target, {}, formation);
    }
    return plan_air_order(faction, kind, target, faction_view(faction, false), formation);
}

MissionPlan CampaignState::plan_air_order(FactionId faction, MissionKind kind, SectorId target,
                                         const FactionView& view, std::optional<UnitId> formation) const
{
    if (kind != MissionKind::recon && !known_friendly(view, faction, target)) {
        return {{}, "target lacks confirmed friendly control"};
    }
    std::optional<FacilityId> relocation;
    if (kind == MissionKind::reposition) {
        for (const auto& [id, facility] : facilities_) {
            const auto& stock = stockpiles_.at(facility.stockpile);
            if (stock.owner == faction && stock.sector == target && facility.operational) {
                relocation = id;
                break;
            }
        }
        if (!relocation) {
            return {{}, "target has no usable friendly facility"};
        }
    }

    std::string_view refusal = formation ? "selected formation has no available equipment in range at a launch facility" :
        "no eligible available equipment in range";
    std::optional<Mission> chosen;
    const auto& destination = world_.sector(target);
    for (const auto& [id, item] : equipment_) {
        const auto& unit = units_.at(item.assignment.unit);
        if (unit.faction != faction || (formation ? unit.id != *formation : operational_reserve_.contains(unit.id)) ||
            item.status != EquipmentStatus::available || !unit.facility ||
            formation_committed(unit.id)) {
            continue;
        }
        const auto& type = equipment_types_.at(item.assignment.type);
        const auto& home = world_.sector(unit.sector);
        const auto distance = std::uint64_t{std::max(home.x, destination.x) - std::min(home.x, destination.x)} +
                              std::max(home.y, destination.y) - std::min(home.y, destination.y);
        if (type.operation_range == 0 || distance > type.operation_range) {
            continue;
        }
        const auto supports = [&](const auto& facility) {
            return facility.supported_types.empty() ||
                   std::ranges::find(facility.supported_types, type.id) != facility.supported_types.end();
        };
        if (relocation && (combat_units_.contains(unit.id) || unit.sector == target ||
            !supports(facilities_.at(*relocation)) ||
            std::ranges::count_if(equipment_, [&](const auto& entry) {
                return entry.second.assignment.unit == unit.id &&
                       entry.second.status != EquipmentStatus::lost;
            }) != 1)) {
            if (formation)
                refusal = "selected formation cannot reposition to this facility";
            continue;
        }
        const auto& facility = facilities_.at(*unit.facility);
        const auto& stock = stockpiles_.at(facility.stockpile);
        if (!facility.operational || !supports(facility) || (formation && stock.owner != faction)) {
            refusal = "launch facility unavailable";
            continue;
        }
        const auto fuel = distance * 2 + 1;
        bool supplied = true;
        for (std::size_t i = 0; i < stock.quantity.size(); ++i) {
            const auto free = stock.quantity[i] - stock.reserved[i];
            const auto operation_cost = i == fuel_index ? fuel : 0;
            if (operation_cost > free || type.preparation.costs[i] > free - operation_cost) {
                supplied = false;
                break;
            }
        }
        if (!supplied) {
            refusal = "insufficient unreserved fuel or preparation stock";
            continue;
        }
        if (!chosen || fuel < chosen->fuel) {
            chosen = Mission{MissionId{}, faction, kind, target, id, stock.id, fuel, distance + 2};
        }
    }
    if (!chosen) {
        return {{}, refusal};
    }
    chosen->relocation = relocation;
    return {std::move(chosen), "feasible"};
}

OrderResult CampaignState::commit_order(MissionPlan plan, bool manual)
{
    if (!plan.candidate) {
        return {{}, plan.reason};
    }
    auto& chosen = plan.candidate;
    chosen->manual = manual;
    if (chosen->ground_unit) {
        const auto faction = chosen->faction;
        const auto id = commit_ground_mission(std::move(*chosen));
        return {id, mission_status_name(missions_.at({faction, id}).status)};
    }

    const auto faction = chosen->faction;
    const auto target = chosen->target;
    chosen->id = next_mission_id(faction);
    const auto& costs = equipment_types_.at(equipment_.at(chosen->equipment).assignment.type).preparation.costs;
    chosen->preparation_reserved = std::ranges::any_of(costs, [](auto cost) { return cost != 0; });
    missions_.emplace(std::pair{faction, chosen->id}, *chosen);
    reserve_equipment(chosen->equipment);
    auto& reserved = stockpiles_.at(chosen->launch_stock).reserved;
    reserved[fuel_index] += chosen->fuel;
    for (std::size_t i = 0; i < costs.size(); ++i) {
        reserved[i] += costs[i];
    }
    record_event(operations_log_, {clock_.tick(), faction, target, chosen->id,
                               manual ? "player order scheduled" : "autonomous order scheduled"});
    return {chosen->id, "scheduled"};
}

void CampaignState::release_unspent_mission_stock(Mission& mission)
{
    if (mission.ground_unit || mission.status == MissionStatus::in_progress) {
        return;
    }
    auto& reserved = stockpiles_.at(mission.launch_stock).reserved;
    reserved[fuel_index] -= mission.fuel;
    if (mission.preparation_reserved) {
        const auto& costs = equipment_types_.at(equipment_.at(mission.equipment).assignment.type).preparation.costs;
        for (std::size_t i = 0; i < costs.size(); ++i) {
            reserved[i] -= costs[i];
        }
        mission.preparation_reserved = false;
    }
}

void CampaignState::release_air_mission_equipment(const Mission& mission, EquipmentStatus standby)
{
    auto& item = equipment_.at(mission.equipment);
    const bool mission_preparation = mission.status == MissionStatus::preparing &&
        (item.status == EquipmentStatus::ready ||
         (item.job && item.job->kind == ServiceKind::preparation && !item.needs_turnaround));
    if (mission_preparation || item.status == EquipmentStatus::reserved) {
        item.job.reset();
        set_equipment_status(item, standby, clock_.tick());
    } else {
        finish_operation(mission.equipment);
    }
}

MissionId CampaignState::next_mission_id(FactionId faction) const
{
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    const auto next_faction = missions_.upper_bound({faction, MissionId{maximum}});
    std::uint32_t number = 1;
    if (next_faction != missions_.begin()) {
        const auto previous = std::prev(next_faction);
        if (previous->first.first == faction) {
            number = static_cast<std::uint32_t>(previous->second.id);
            if (number == maximum) {
                throw std::length_error("Faction mission ID limit reached");
            }
            ++number;
        }
    }
    return MissionId{number};
}

bool CampaignState::can_cancel_order(FactionId actor, MissionId id) const
{
    const auto found = missions_.find({actor, id});
    return can_issue_orders(actor) && found != missions_.end() &&
        (found->second.status == MissionStatus::scheduled ||
         (found->second.ground_unit && found->second.manual &&
          (found->second.status == MissionStatus::in_progress || found->second.status == MissionStatus::holding ||
           found->second.status == MissionStatus::observing)));
}

bool CampaignState::cancel_order(FactionId actor, MissionId id)
{
    if (!can_cancel_order(actor, id)) {
        return false;
    }
    auto& mission = missions_.at({actor, id});
    if (mission.ground_unit) {
        release_ground_equipment(mission.equipment);
    } else {
        cancel_equipment_reservation(mission.equipment);
        release_unspent_mission_stock(mission);
    }
    const bool holding = mission.status == MissionStatus::holding;
    mission.status = holding ? MissionStatus::released : MissionStatus::cancelled;
    record_event(operations_log_, {clock_.tick(), actor, mission.target, id,
        holding ? "holding position released" : "player order cancelled"});
    return true;
}

bool CampaignState::update_operations()
{
    const auto tick = clock_.tick();
    bool equipment_changed = false;
    for (auto& [key, mission] : missions_) {
        if (mission.ground_unit || !mission_active(mission.status)) {
            continue;
        }
        auto& item = equipment_.at(mission.equipment);
        const auto log = [&](std::string_view reason) {
            record_event(operations_log_, {tick, mission.faction, mission.target, mission.id, std::string(reason)});
        };
        const bool invalid_preparation = mission.status == MissionStatus::preparing &&
            item.status != EquipmentStatus::ready &&
            (!item.job || item.job->kind != ServiceKind::preparation);
        if (item.status == EquipmentStatus::lost || invalid_preparation ||
            (mission.status == MissionStatus::scheduled && item.status != EquipmentStatus::reserved) ||
            (mission.status == MissionStatus::in_progress && item.status != EquipmentStatus::in_operation)) {
            release_unspent_mission_stock(mission);
            mission.status = MissionStatus::failed;
            log("equipment no longer available");
            continue;
        }
        if (mission.status == MissionStatus::scheduled) {
            prepare_equipment(mission.equipment);
            mission.status = MissionStatus::preparing;
            equipment_changed = true;
            log("preparing");
        } else if (mission.status == MissionStatus::preparing && item.status == EquipmentStatus::ready) {
            ResourceAmounts cost{};
            cost[fuel_index] = mission.fuel;
            if (!consume_at(mission.launch_stock, cost, tick, mission.equipment, true)) {
                throw std::logic_error("Missing mission fuel reservation");
            }
            begin_operation(mission.equipment);
            mission.status = MissionStatus::in_progress;
            mission.launch_tick = tick;
            equipment_changed = true;
            log("launched");
        } else if (mission.status == MissionStatus::in_progress && tick - mission.launch_tick >= mission.duration_ticks) {
            if (mission.kind == MissionKind::recon || mission.kind == MissionKind::patrol) {
                mission.detections = observe(mission.faction, mission.target);
                log(mission.detections ? "observation reported: contacts detected" :
                                        "observation reported: no contacts detected");
            }
            bool completed = true;
            if (mission.relocation) {
                const auto& facility = facilities_.at(*mission.relocation);
                auto& unit = units_.at(item.assignment.unit);
                if (facility.operational) {
                    unit.sector = mission.target;
                    unit.facility = mission.relocation;
                } else {
                    completed = false;
                    log("relocation aborted: destination unavailable");
                }
            }
            finish_operation(mission.equipment);
            mission.status = completed ? MissionStatus::completed : MissionStatus::failed;
            equipment_changed = true;
            if (completed) {
                log("completed");
            }
        }
    }
    if (operations_.planner_interval_ticks != 0 && tick % operations_.planner_interval_ticks == 0) {
        for (const auto faction : {FactionId::west, FactionId::east}) {
            if (combat_.enabled && operations_.strategy_review_ticks) {
                equipment_changed = update_strategy(faction) || equipment_changed;
                continue;
            }
            const auto view = faction_view(faction, false);
            auto assigned = static_cast<std::size_t>(std::ranges::count_if(view.missions, [](const auto& mission) {
                return !mission.manual && mission_active(mission.status);
            }));
            if (assigned >= operations_.max_autonomous_missions) {
                continue;
            }
            std::set<SectorId> busy_targets;
            for (const auto& mission : view.missions) {
                if (mission_active(mission.status)) {
                    busy_targets.insert(mission.target);
                }
            }
            for (const auto& plan : rank_objectives(view, faction, combat_.enabled)) {
                if (busy_targets.contains(plan.target)) {
                    continue;
                }
                if (combat_.enabled && plan.kind == MissionKind::defend &&
                    std::ranges::any_of(combat_units_, [&](const auto& entry) {
                        const auto& unit = units_.at(entry.first);
                        return unit.faction == faction && unit.sector == plan.target &&
                               entry.second.status == GroundStatus::holding && unit_readiness(unit.id).ready > 0;
                    })) {
                    continue;
                }
                const auto result = schedule_mission(faction, plan.kind, plan.target, false);
                record_event(operations_log_, {tick, faction, plan.target, result.mission,
                                           std::string(result.mission ? plan.reason : result.reason)});
                if (result.mission) {
                    equipment_changed = true;
                    busy_targets.insert(plan.target);
                    if (++assigned == operations_.max_autonomous_missions) {
                        break;
                    }
                }
            }
        }
    }
    return equipment_changed;
}

}  // namespace openwar
