#include "campaign.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <set>
#include <tuple>
#include <utility>

namespace openwar {
namespace {

bool active_order(const Mission& mission)
{
    return mission.status == MissionStatus::scheduled || mission.status == MissionStatus::preparing ||
           mission.status == MissionStatus::in_progress;
}

bool friendly(const FactionView& view, FactionId faction, SectorId sector)
{
    const auto report = view.sectors.find(sector);
    return report != view.sectors.end() && report->second.confidence >= 750 &&
           report->second.estimated_controller == faction;
}

bool threatened(const FactionView& view, SectorId sector)
{
    const auto report = view.sectors.find(sector);
    return report != view.sectors.end() &&
           ((report->second.contested && report->second.confidence >= 350) ||
            std::ranges::any_of(view.contacts, [&](const ContactReport& contact) {
                return contact.confidence >= 350 && current_contact(contact, report->second);
            }));
}

bool passable(const Sector& sector)
{
    return sector.properties.traversable && sector.properties.terrain != Terrain::water;
}

// Geometric distances guide staging. Only the operational planner can authorize a route.
std::vector<SectorId> approaches(const WorldDefinition& world, const FactionView& view,
                                 FactionId faction, SectorId focus)
{
    std::map<SectorId, unsigned> distances{{focus, 0}};
    std::queue<SectorId> pending;
    pending.push(focus);
    std::vector<SectorId> targets{focus};
    while (!pending.empty()) {
        const auto sector = pending.front();
        pending.pop();
        for (const auto next : world.sector(sector).neighbors) {
            if (passable(world.sector(next)) && distances.try_emplace(next, distances.at(sector) + 1).second) {
                pending.push(next);
            }
        }
    }
    for (const auto& [sector, distance] : distances) {
        if (sector != focus && !friendly(view, faction, sector) &&
            std::ranges::any_of(world.sector(sector).neighbors, [&](SectorId neighbor) {
                const auto found = distances.find(neighbor);
                return found != distances.end() && found->second > distance && friendly(view, faction, neighbor);
            })) {
            targets.push_back(sector);
        }
    }
    std::ranges::sort(targets, [&](SectorId left, SectorId right) {
        return std::tie(distances.at(left), left) < std::tie(distances.at(right), right);
    });
    return targets;
}

}  // namespace

void CampaignState::dispatch_reinforcement(FactionId faction, const FactionView& view, const StrategicPlan& plan)
{
    const auto tick = clock_.tick();
    const auto delay = operations_.reinforcement_travel_ticks;
    if (!delay || delay > std::numeric_limits<std::uint64_t>::max() / clock_.tick_seconds() - tick) {
        return;
    }
    std::optional<EquipmentId> chosen;
    std::size_t greatest_deficit = 0;
    for (const auto& [id, item] : equipment_) {
        if (item.status != EquipmentStatus::reserve) {
            continue;
        }
        const auto& unit = units_.at(item.assignment.unit);
        if (unit.faction != faction || !friendly(view, faction, unit.sector) || !reinforcement_entry_ready(item)) {
            continue;
        }
        const auto losses = unit_readiness(unit.id).lost;
        std::size_t replacements = 0;
        bool incoming = false;
        for (const auto& [other_id, other] : equipment_) {
            if (other.assignment.unit == unit.id && !other.assignment.reserve_source.empty()) {
                replacements += other.status != EquipmentStatus::reserve;
                incoming = incoming || other.reinforcement_eta.has_value();
            }
        }
        // A formation gets at most one replacement in transit, and keeps its original inventory identities.
        const auto deficit = !incoming && losses > replacements ? losses - replacements : 0;
        const auto threat = threatened(view, unit.sector) || plan.focus == unit.sector;
        const auto priority = deficit ? deficit * 2 + threat : 0;
        if (priority > greatest_deficit) {
            greatest_deficit = priority;
            chosen = id;
        }
    }
    if (chosen) {
        const auto sector = units_.at(equipment_.at(*chosen).assignment.unit).sector;
        if (authorize_reinforcement(*chosen, tick + delay)) {
            record_event(operations_log_, {tick, faction, sector, {}, "strategy: committed a reserve after losses"});
        }
    }
}

bool CampaignState::update_strategy(FactionId faction)
{
    const auto tick = clock_.tick();
    auto view = faction_view(faction, false);
    std::map<SectorId, std::uint64_t> priorities;
    for (const auto& objective : view.objectives) {
        priorities[objective.sector] = std::max<std::uint64_t>(priorities[objective.sector], objective.priority);
    }
    for (const auto& objective : victory_objectives_) {
        if (objective.faction == faction) {
            for (const auto sector : objective.sectors) {
                priorities[sector] += 2;
            }
        }
    }
    std::set<SectorId> command_sites;
    for (const auto& objective : command_objectives_) {
        if (objective.faction == faction) {
            command_sites.insert(objective.sectors.begin(), objective.sectors.end());
        }
    }
    const auto secured = [&](SectorId sector) {
        return friendly(view, faction, sector) && !threatened(view, sector);
    };
    std::size_t losses = 0;
    std::size_t ready = 0;
    std::map<SectorId, std::size_t> garrisons;
    std::vector<UnitId> guards;
    for (const auto& [id, state] : combat_units_) {
        const auto& unit = units_.at(id);
        if (unit.faction != faction) {
            continue;
        }
        losses += unit_readiness(id).lost;
        if (state.status == GroundStatus::holding && ground_speed(id)) {
            const auto strength = static_cast<std::size_t>(std::ranges::count_if(equipment_, [&](const auto& entry) {
                const auto& item = entry.second;
                return item.assignment.unit == id && equipment_types_.at(item.assignment.type).can_occupy &&
                    (item.status == EquipmentStatus::available || item.status == EquipmentStatus::ready ||
                     item.status == EquipmentStatus::reserved || item.status == EquipmentStatus::in_operation);
            }));
            ready += strength;
            garrisons[unit.sector] += strength;
            if (friendly(view, faction, unit.sector) && threatened(view, unit.sector) &&
                (priorities.contains(unit.sector) || command_sites.contains(unit.sector))) {
                guards.push_back(id);
            }
        }
    }
    const auto [it, first_review] = strategies_.try_emplace(faction);
    auto& plan = it->second;
    const bool review_due = first_review || tick - plan.reviewed_tick >= operations_.strategy_review_ticks;
    const bool phase_changed = (plan.focus && secured(*plan.focus)) || plan.ground_losses != losses;
    if (review_due || phase_changed) {
        const auto previous_focus = plan.focus;
        const auto was_recovering = plan.recovering;
        plan.ground_losses = losses;
        plan.recovering = losses >= 2 && ready <= 1;
        std::optional<SectorId> focus;
        std::uint64_t best = 0;
        for (const auto& [sector, priority] : priorities) {
            if (!passable(world_.sector(sector)) || friendly(view, faction, sector)) {
                continue;
            }
            const auto score = priority * 2 + (sector == previous_focus ? 1 : 0);
            if (!focus || score > best) {
                focus = sector;
                best = score;
            }
        }
        plan.focus = focus;
        plan.reviewed_tick = tick;
        if (plan.recovering != was_recovering) {
            record_event(operations_log_, {tick, faction, focus.value_or(SectorId{}), {},
                plan.recovering ? "strategy: recovering after losses" : "strategy: offensive readiness recovered"});
        }
        if (focus && (first_review || focus != previous_focus)) {
            record_event(operations_log_, {tick, faction, *focus, {}, "strategy: set campaign objective"});
        }
        dispatch_reinforcement(faction, view, plan);
    }

    std::size_t assigned = 0;
    std::set<SectorId> busy;
    for (const auto& mission : view.missions) {
        if (active_order(mission)) {
            busy.insert(mission.target);
            assigned += !mission.manual;
        }
    }
    const auto limit = operations_.max_autonomous_missions;
    bool changed = false;
    const auto dispatch = [&](Mission mission, std::string_view reason) {
        const auto target = mission.target;
        const auto id = commit_ground_mission(std::move(mission));
        view.missions.push_back(missions_.at({faction, id}));
        record_event(operations_log_, {tick, faction, target, id, std::string(reason)});
        ++assigned;
        changed = true;
        busy.insert(target);
    };
    const auto recon = [&](SectorId sector) {
        if (busy.contains(sector) || assigned >= limit) {
            return;
        }
        const auto order = schedule_air_mission(faction, MissionKind::recon, sector, false, view);
        if (order.mission) {
            view.missions.push_back(missions_.at({faction, *order.mission}));
            record_event(operations_log_, {tick, faction, sector, order.mission, "strategy: survey approach"});
            changed = true;
            ++assigned;
            busy.insert(sector);
        }
    };

    std::vector<SectorId> defenses;
    for (const auto& [sector, report] : view.sectors) {
        if ((priorities.contains(sector) || command_sites.contains(sector)) &&
            friendly(view, faction, sector) && threatened(view, sector)) {
            defenses.push_back(sector);
        }
    }
    std::ranges::stable_sort(defenses, [&](SectorId left, SectorId right) {
        return command_sites.contains(left) > command_sites.contains(right);
    });
    for (const auto target : defenses) {
        if (assigned >= limit) {
            break;
        }
        const auto& report = view.sectors.at(target);
        const auto opposition = static_cast<std::size_t>(std::ranges::count_if(view.contacts, [&](const auto& contact) {
            return contact.classification == "ground" && contact.confidence >= 350 && current_contact(contact, report);
        }));
        if (busy.contains(target) || garrisons[target] >= std::max<std::size_t>(1, opposition)) {
            continue;
        }
        if (auto order = plan_ground_mission(faction, MissionKind::defend, target, view, guards)) {
            dispatch(std::move(*order), "strategy: defend reported threat");
        }
    }
    if (plan.recovering || assigned >= limit) {
        return changed;
    }

    // Try the persistent focus first, then usable staging sectors and secondary objectives.
    std::vector<SectorId> targets = plan.focus ? approaches(world_, view, faction, *plan.focus) :
                                               std::vector<SectorId>{};
    std::vector<SectorId> secondary;
    for (const auto& [sector, priority] : priorities) {
        if (!friendly(view, faction, sector) && passable(world_.sector(sector))) {
            secondary.push_back(sector);
        }
    }
    std::ranges::stable_sort(secondary, [&](SectorId left, SectorId right) {
        return priorities.at(left) > priorities.at(right);
    });
    targets.insert(targets.end(), secondary.begin(), secondary.end());
    std::set<SectorId> tried;
    for (const auto target : targets) {
        if (assigned >= limit) {
            break;
        }
        if (busy.contains(target) || !tried.insert(target).second) {
            continue;
        }
        const auto report = view.sectors.find(target);
        const bool stale_contact = report != view.sectors.end() &&
            std::ranges::any_of(view.contacts, [&](const auto& contact) {
                return current_contact(contact, report->second) && contact.confidence < 750;
            });
        if (stale_contact || !ground_target_refusal(view, faction, MissionKind::attack, target).empty()) {
            if (!friendly(view, faction, target)) {
                recon(target);
            }
            continue;
        }
        auto lead = plan_ground_mission(faction, MissionKind::attack, target, view, guards);
        if (!lead) {
            continue;
        }
        // Two formations can support the same objective within the configured arrival window.
        if (threatened(view, target) && assigned + 1 < limit) {
            auto exclusions = guards;
            exclusions.push_back(*lead->ground_unit);
            auto support = plan_ground_mission(faction, MissionKind::attack, target, view, exclusions);
            if (support && support->duration_ticks >= lead->duration_ticks &&
                support->duration_ticks - lead->duration_ticks <= operations_.coordination_window_ticks &&
                support->duration_ticks < std::numeric_limits<std::uint64_t>::max() - tick) {
                lead->earliest_launch_tick = tick + 1 + support->duration_ticks - lead->duration_ticks;
                dispatch(std::move(*lead), "strategy: coordinated offensive");
                dispatch(std::move(*support), "strategy: coordinated support");
                continue;
            }
        }
        dispatch(std::move(*lead), target == plan.focus ? "strategy: advance campaign objective" :
                                                        "strategy: advance on another front");
    }
    if (!changed && review_due && plan.focus) {
        record_event(operations_log_, {tick, faction, *plan.focus, {}, "strategy: waiting for a viable operation"});
    }
    return changed;
}

}  // namespace openwar
