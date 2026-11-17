#include "campaign.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace openwar {
namespace {

constexpr auto fuel_index = static_cast<std::size_t>(Resource::fuel);

bool active(MissionStatus status)
{
    return status == MissionStatus::scheduled || status == MissionStatus::preparing ||
           status == MissionStatus::in_progress;
}

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
            return contact.estimated_sector == objective.sector && contact.confidence < 750;
        });
        const auto priority = std::uint64_t{objective.priority} * 1000;
        if (confidence < 750 || stale_contact) {
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
    }
    throw std::invalid_argument("Unknown mission status");
}

OrderResult CampaignState::issue_order(FactionId actor, MissionKind kind, SectorId target)
{
    if (war_ended() || !operations_.enabled || operations_.player != actor) {
        return {{}, "player has no authority for this faction"};
    }
    return schedule_mission(actor, kind, target, true);
}

OrderResult CampaignState::schedule_mission(FactionId faction, MissionKind kind, SectorId target, bool manual)
{
    mission_name(kind);
    if (static_cast<std::uint32_t>(target) >= world_.sectors().size()) {
        return {{}, "target sector is out of bounds"};
    }
    if (kind == MissionKind::attack || (combat_.enabled && kind == MissionKind::defend)) {
        return schedule_ground_mission(faction, kind, target, manual);
    }
    const auto view = faction_view(faction, false);
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

    std::string_view refusal = "no eligible available equipment in range";
    std::optional<Mission> chosen;
    const auto& destination = world_.sector(target);
    for (const auto& [id, item] : equipment_) {
        const auto& unit = units_.at(item.assignment.unit);
        if (unit.faction != faction || item.status != EquipmentStatus::available || !unit.facility ||
            std::ranges::any_of(view.missions, [&](const auto& mission) {
                return active(mission.status) &&
                       equipment_.at(mission.equipment).assignment.unit == unit.id;
            })) {
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
        if (relocation && (unit.sector == target || !supports(facilities_.at(*relocation)) ||
            std::ranges::count_if(equipment_, [&](const auto& entry) {
                return entry.second.assignment.unit == unit.id &&
                       entry.second.status != EquipmentStatus::lost;
            }) != 1)) {
            continue;
        }
        const auto& facility = facilities_.at(*unit.facility);
        if (!facility.operational || !supports(facility)) {
            refusal = "launch facility unavailable";
            continue;
        }
        const auto& stock = stockpiles_.at(facility.stockpile);
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
    chosen->id = next_mission_id(faction);
    chosen->manual = manual;
    chosen->relocation = relocation;
    missions_.emplace(std::pair{faction, chosen->id}, *chosen);
    reserve_equipment(chosen->equipment);
    stockpiles_.at(chosen->launch_stock).reserved[fuel_index] += chosen->fuel;
    operations_log_.push_back({clock_.tick(), faction, target, chosen->id,
                               manual ? "player order scheduled" : "autonomous order scheduled"});
    return {chosen->id, "scheduled"};
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

bool CampaignState::cancel_order(FactionId actor, MissionId id)
{
    const auto found = missions_.find({actor, id});
    if (war_ended() || operations_.player != actor || found == missions_.end() ||
        found->second.status != MissionStatus::scheduled) {
        return false;
    }
    auto& mission = found->second;
    cancel_equipment_reservation(mission.equipment);
    stockpiles_.at(mission.launch_stock).reserved[fuel_index] -= mission.fuel;
    mission.status = MissionStatus::cancelled;
    operations_log_.push_back({clock_.tick(), actor, mission.target, id, "player order cancelled"});
    return true;
}

bool CampaignState::update_operations()
{
    const auto tick = clock_.tick();
    bool equipment_changed = false;
    for (auto& [key, mission] : missions_) {
        if (mission.ground_unit || !active(mission.status)) {
            continue;
        }
        auto& item = equipment_.at(mission.equipment);
        const auto log = [&](std::string_view reason) {
            operations_log_.push_back({tick, mission.faction, mission.target, mission.id, reason});
        };
        auto& stock = stockpiles_.at(mission.launch_stock);
        const bool invalid_preparation = mission.status == MissionStatus::preparing &&
            item.status != EquipmentStatus::ready &&
            (!item.job || item.job->kind != ServiceKind::preparation);
        if (item.status == EquipmentStatus::lost || invalid_preparation ||
            (mission.status == MissionStatus::scheduled && item.status != EquipmentStatus::reserved) ||
            (mission.status == MissionStatus::in_progress && item.status != EquipmentStatus::in_operation)) {
            if (mission.status != MissionStatus::in_progress) {
                stock.reserved[fuel_index] -= mission.fuel;
            }
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
            stock.reserved[fuel_index] -= mission.fuel;
            consume_at(stock.id, cost, tick, mission.equipment);
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
            const auto view = faction_view(faction, false);
            if (std::ranges::any_of(view.missions, [](const auto& mission) {
                return !mission.manual && active(mission.status);
            })) {
                continue;
            }
            for (const auto& plan : rank_objectives(view, faction, combat_.enabled)) {
                if (std::ranges::any_of(view.missions, [&](const auto& mission) {
                    return mission.target == plan.target && active(mission.status);
                })) {
                    continue;
                }
                const auto result = schedule_mission(faction, plan.kind, plan.target, false);
                operations_log_.push_back({tick, faction, plan.target, result.mission,
                                           result.mission ? plan.reason : result.reason});
                if (result.mission) {
                    equipment_changed = true;
                    break;
                }
            }
        }
    }
    return equipment_changed;
}

}  // namespace openwar
