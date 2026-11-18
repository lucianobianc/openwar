#include "campaign.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace openwar {
namespace {

constexpr auto supplies = static_cast<std::size_t>(Resource::supplies);

bool military_order_active(MissionStatus status)
{
    return status == MissionStatus::scheduled || status == MissionStatus::preparing ||
           status == MissionStatus::in_progress;
}

WarStatus winner(FactionId faction)
{
    return faction == FactionId::west ? WarStatus::west_victory : WarStatus::east_victory;
}

}  // namespace

std::string_view control_name(ControlState state)
{
    switch (state) {
    case ControlState::neutral: return "neutral";
    case ControlState::secure: return "secure";
    case ControlState::contested: return "contested";
    case ControlState::unstable: return "unstable";
    }
    throw std::invalid_argument("Unknown territorial state");
}

std::string_view war_status_name(WarStatus status)
{
    switch (status) {
    case WarStatus::ongoing: return "ongoing";
    case WarStatus::ceasefire_pending: return "ceasefire pending";
    case WarStatus::west_victory: return "WEST victory";
    case WarStatus::east_victory: return "EAST victory";
    case WarStatus::draw: return "draw";
    }
    throw std::invalid_argument("Unknown war status");
}

bool CampaignState::war_ended() const
{
    return war_result_.status != WarStatus::ongoing && war_result_.status != WarStatus::ceasefire_pending;
}

std::vector<std::pair<SectorId, SectorId>> CampaignState::front_line() const
{
    std::vector<std::pair<SectorId, SectorId>> result;
    for (const auto& sector : world_.sectors()) {
        const auto faction = sector_state(sector.id).controller;
        for (const auto neighbor : sector.neighbors) {
            const auto other = sector_state(neighbor).controller;
            if (sector.id < neighbor && faction != FactionId::neutral &&
                other != FactionId::neutral && faction != other) {
                result.emplace_back(sector.id, neighbor);
            }
        }
    }
    return result;
}

void CampaignState::update_territory()
{
    const auto tick = clock_.tick();
    const auto now = clock_.elapsed_seconds();
    std::map<SectorId, std::array<bool, 3>> presence;
    for (const auto& [id, item] : equipment_) {
        const auto state = combat_units_.find(item.assignment.unit);
        if (state == combat_units_.end() || state->second.status != GroundStatus::holding ||
            !equipment_types_.at(item.assignment.type).can_occupy ||
            (item.status != EquipmentStatus::available && item.status != EquipmentStatus::reserved &&
             item.status != EquipmentStatus::ready && item.status != EquipmentStatus::in_operation)) {
            continue;
        }
        const auto& unit = units_.at(item.assignment.unit);
        if (world_.sector(unit.sector).properties.traversable) {
            presence[unit.sector][static_cast<std::size_t>(unit.faction)] = true;
        }
    }
    for (const auto& sector : world_.sectors()) {
        auto& state = sector_states_.at(static_cast<std::uint32_t>(sector.id));
        const auto& sides = presence[sector.id];
        const bool opposed = sides[1] && sides[2];
        std::optional<FactionId> occupying;
        if (!opposed && (sides[1] || sides[2])) {
            const auto faction = sides[1] ? FactionId::west : FactionId::east;
            if (state.controller != faction && std::ranges::any_of(missions_, [&](const auto& entry) {
                const auto& mission = entry.second;
                if (!mission.ground_unit || mission.faction != faction || mission.target != sector.id ||
                    mission.kind != MissionKind::attack || mission.status != MissionStatus::in_progress ||
                    units_.at(*mission.ground_unit).sector != sector.id ||
                    combat_units_.at(*mission.ground_unit).status != GroundStatus::holding ||
                    equipment_.at(mission.equipment).status != EquipmentStatus::in_operation) {
                    return false;
                }
                const auto& stock = stockpiles_.at(combat_units_.at(*mission.ground_unit).stores);
                return stock.quantity[supplies] > stock.reserved[supplies];
            })) {
                occupying = faction;
            }
        }
        if (state.occupier != occupying) {
            if (state.occupier) {
                record_event(battle_log_, {tick, sector.id, "occupation interrupted"});
            }
            state.occupier = occupying;
            state.occupation_since = now;
            if (occupying) {
                record_event(battle_log_, {tick, sector.id, "occupation started"});
            }
        }
        if (opposed || occupying) {
            if (state.control != ControlState::contested) {
                record_event(battle_log_, {tick, sector.id, "sector contested"});
            }
            state.control = ControlState::contested;
            const auto hold = state.objective_points ? combat_.critical_occupation_seconds :
                                                       combat_.occupation_seconds;
            if (occupying && now - state.occupation_since >= hold) {
                capture_sector(sector.id, *occupying);
            }
        } else if (state.controller == FactionId::neutral) {
            state.control = ControlState::neutral;
        } else {
            const auto age = (tick - state.last_change_tick) * clock_.tick_seconds();
            if (state.last_change_tick && age < combat_.stabilization_seconds) {
                state.control = ControlState::unstable;
            } else {
                if (state.control == ControlState::unstable) {
                    record_event(battle_log_, {tick, sector.id, "sector stabilized"});
                }
                state.control = ControlState::secure;
            }
        }
        // Movement and combat reports made this tick can include the resolved dispute.
        for (auto& [faction, store] : intelligence_) {
            const auto report = store.sectors.find(sector.id);
            if (report != store.sectors.end() && report->second.observed_tick == tick) {
                report->second.contested = state.control == ControlState::contested;
            }
        }
    }
}

void CampaignState::capture_sector(SectorId sector, FactionId faction)
{
    auto& state = sector_states_.at(static_cast<std::uint32_t>(sector));
    const auto previous = state.controller;
    state.controller = faction;
    state.control = ControlState::unstable;
    state.last_change_tick = clock_.tick();
    state.occupier.reset();
    for (auto& [id, stock] : stockpiles_) {
        if (stock.sector != sector || stock.owner == faction ||
            std::ranges::any_of(combat_units_, [id](const auto& entry) { return entry.second.stores == id; })) {
            continue;
        }
        for (const auto& [shipment_id, shipment] : shipments_) {
            if (shipment.request.origin == id || shipment.request.destination == id) {
                cancel_shipment(shipment_id);
            }
        }
        for (auto& [key, mission] : missions_) {
            if (mission.launch_stock != id || !military_order_active(mission.status)) {
                continue;
            }
            release_unspent_mission_stock(mission);
            release_air_mission_equipment(mission, EquipmentStatus::grounded);
            mission.status = MissionStatus::failed;
            record_event(operations_log_, {clock_.tick(), mission.faction, mission.target, mission.id,
                                       "launch stockpile captured"});
        }
        stock.owner = faction;
        for (auto& installation : installations_) {
            if (installation.installation.stockpile == id) {
                installation.installation.operator_faction = faction;
                const auto now = clock_.elapsed_seconds();
                installation.reactivation_seconds = now + std::min(combat_.stabilization_seconds,
                    std::numeric_limits<std::uint64_t>::max() - now);
            }
        }
        for (auto& [facility_id, facility] : facilities_) {
            if (facility.stockpile == id) {
                facility.operational = false;
            }
        }
    }
    if (operations_.enabled) {
        observe(faction, sector);
    }
    record_event(battle_log_, {clock_.tick(), sector, "sector captured", {},
                                static_cast<std::uint32_t>(faction), 0, previous});
}

bool CampaignState::declare_surrender(FactionId actor)
{
    if (!combat_.enabled || war_ended() || (actor != FactionId::west && actor != FactionId::east) ||
        std::ranges::find(surrenders_, actor) != surrenders_.end()) {
        return false;
    }
    surrenders_.push_back(actor);
    return true;
}

void CampaignState::evaluate_war()
{
    const auto now = clock_.elapsed_seconds();
    std::array<bool, 3> capitulated{};
    std::array<bool, 3> objective_won{};
    bool pending = false;
    for (const auto faction : {FactionId::west, FactionId::east}) {
        const auto index = static_cast<std::size_t>(faction);
        if (std::ranges::find(surrenders_, faction) != surrenders_.end()) {
            capitulated[index] = true;
            continue;
        }
        std::uint64_t points = 0;
        std::uint64_t lost_points = 0;
        bool has_command = false;
        for (const auto& objective : command_objectives_) {
            if (objective.faction != faction) {
                continue;
            }
            for (const auto sector : objective.sectors) {
                const auto& state = sector_state(sector);
                const auto weight = std::max(1u, state.objective_points);
                points += weight;
                if (state.controller != faction) {
                    lost_points += weight;
                } else {
                    bool has_facility = false;
                    bool has_operational = false;
                    for (const auto& [id, facility] : facilities_) {
                        const auto& stock = stockpiles_.at(facility.stockpile);
                        if (stock.sector == sector && stock.owner == faction) {
                            has_facility = true;
                            has_operational = has_operational || facility.operational;
                        }
                    }
                    has_command = has_command || !has_facility || has_operational;
                }
            }
        }
        std::uint64_t strength = 0;
        bool recovery = false;
        for (const auto& [id, item] : equipment_) {
            const auto& unit = units_.at(item.assignment.unit);
            if (unit.faction != faction) {
                continue;
            }
            if (item.reinforcement_eta && reinforcement_entry_ready(item)) {
                const auto eta = std::max(clock_.tick(), *item.reinforcement_eta);
                recovery = recovery || eta - clock_.tick() <=
                    combat_.reinforcement_horizon_seconds / clock_.tick_seconds();
            }
            const auto combat = combat_units_.find(unit.id);
            if ((item.status == EquipmentStatus::available || item.status == EquipmentStatus::ready ||
                 item.status == EquipmentStatus::in_operation || item.status == EquipmentStatus::reserved) &&
                (combat == combat_units_.end() || combat->second.status == GroundStatus::holding)) {
                ++strength;
            }
        }
        const bool resistance_lost = strength * 5 < initial_strength_[faction] ||
                                     lost_points >= points - points / 4;
        if (points && !has_command && resistance_lost && !recovery) {
            const auto timer = capitulation_since_.try_emplace(faction, now).first->second;
            capitulated[index] = now - timer >= combat_.capitulation_seconds;
            pending = true;
        } else {
            capitulation_since_.erase(faction);
        }
    }
    if (capitulated[1] || capitulated[2]) {
        if (capitulated[1] && capitulated[2]) {
            conclude_war(WarStatus::draw, surrenders_.size() == 2 ?
                "mutual surrender" : "simultaneous capitulation");
        } else {
            conclude_war(winner(capitulated[1] ? FactionId::east : FactionId::west), "capitulation");
        }
        return;
    }
    for (std::size_t i = 0; i < victory_objectives_.size(); ++i) {
        const auto& objective = victory_objectives_[i];
        const bool holds_sectors = std::ranges::all_of(objective.sectors, [&](const auto sector) {
                const auto& state = sector_state(sector);
                return state.controller == objective.faction && state.control != ControlState::contested;
            });
        const bool operational_sites = std::ranges::all_of(objective.required_installations, [&](const auto id) {
            const auto site = std::ranges::find_if(installations_, [id](const auto& state) {
                return state.installation.id == id;
            });
            const auto& plant = site->installation;
            return plant.operator_faction == objective.faction && plant.integrity != 0 &&
                   plant.integrity >= minimum_integrity_ && plant.workers >= plant.recipe.workers_required &&
                   (!plant.recipe.requires_power || plant.has_power) && now >= site->reactivation_seconds &&
                   sector_state(stockpiles_.at(plant.stockpile).sector).controller == objective.faction;
        });
        if (!holds_sectors || !operational_sites) {
            victory_since_[i].reset();
            continue;
        }
        if (!victory_since_[i]) {
            victory_since_[i] = now;
        }
        if (now - *victory_since_[i] >= objective.hold_seconds) {
            objective_won[static_cast<std::size_t>(objective.faction)] = true;
        }
    }
    if (objective_won[1] && objective_won[2]) {
        conclude_war(WarStatus::draw, "simultaneous objectives");
    } else if (objective_won[1] || objective_won[2]) {
        conclude_war(winner(objective_won[1] ? FactionId::west : FactionId::east), "scenario objective");
    } else if (combat_.time_limit_seconds && now >= *combat_.time_limit_seconds) {
        conclude_war(WarStatus::draw, "time limit");
    } else {
        war_result_.status = pending ? WarStatus::ceasefire_pending : WarStatus::ongoing;
    }
}

void CampaignState::conclude_war(WarStatus status, std::string_view cause)
{
    war_result_ = {status, clock_.tick(), std::string(cause)};
    for (auto& [key, mission] : missions_) {
        if (!military_order_active(mission.status)) {
            continue;
        }
        release_unspent_mission_stock(mission);
        if (mission.ground_unit) {
            release_ground_equipment(mission.equipment);
        } else {
            release_air_mission_equipment(mission, EquipmentStatus::available);
        }
        mission.status = MissionStatus::cancelled;
        record_event(operations_log_, {clock_.tick(), mission.faction, mission.target, mission.id, "war ended"});
    }
}

}  // namespace openwar
