#include "campaign.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace openwar {
namespace {

const ServiceProfile& profile(const EquipmentType& type, ServiceKind kind)
{
    switch (kind) {
    case ServiceKind::preparation: return type.preparation;
    case ServiceKind::inspection: return type.inspection;
    case ServiceKind::scheduled: return type.scheduled;
    case ServiceKind::repair: return type.repair;
    }
    throw std::invalid_argument("Unknown service kind");
}

EquipmentStatus active_status(ServiceKind kind)
{
    switch (kind) {
    case ServiceKind::preparation: return EquipmentStatus::preparing;
    case ServiceKind::inspection: return EquipmentStatus::inspection;
    case ServiceKind::scheduled: return EquipmentStatus::maintenance;
    case ServiceKind::repair: return EquipmentStatus::repair;
    }
    throw std::invalid_argument("Unknown service kind");
}

bool service_due(const Equipment& item, const EquipmentType& type)
{
    return item.operating_seconds - item.last_service_seconds >= type.service_interval_seconds;
}

}  // namespace

std::string_view equipment_status_name(EquipmentStatus status)
{
    switch (status) {
    case EquipmentStatus::available: return "available";
    case EquipmentStatus::reserved: return "reserved";
    case EquipmentStatus::preparing: return "preparing";
    case EquipmentStatus::ready: return "ready";
    case EquipmentStatus::in_operation: return "in operation";
    case EquipmentStatus::returning: return "returning";
    case EquipmentStatus::inspection: return "inspection";
    case EquipmentStatus::maintenance: return "maintenance";
    case EquipmentStatus::repair: return "repair";
    case EquipmentStatus::awaiting_parts: return "waiting for resources";
    case EquipmentStatus::awaiting_crew: return "waiting for technicians or bay";
    case EquipmentStatus::grounded: return "grounded";
    case EquipmentStatus::reserve: return "reserve";
    case EquipmentStatus::in_transit: return "reinforcement in transit";
    case EquipmentStatus::lost: return "permanently lost";
    }
    throw std::invalid_argument("Unknown equipment status");
}

std::string_view service_name(ServiceKind kind)
{
    switch (kind) {
    case ServiceKind::preparation: return "preparation";
    case ServiceKind::inspection: return "inspection";
    case ServiceKind::scheduled: return "scheduled maintenance";
    case ServiceKind::repair: return "repair";
    }
    throw std::invalid_argument("Unknown service kind");
}

std::uint32_t service_seconds(const EquipmentType& type, ServiceKind kind)
{
    return profile(type, kind).seconds;
}

void CampaignState::initialize_forces(const ScenarioDefinition& scenario)
{
    for (const auto& type : scenario.equipment_types) {
        faction_name(type.faction);
        if (type.faction == FactionId::neutral || type.name.empty() ||
            type.category < EquipmentCategory::helicopter ||
            type.category > EquipmentCategory::air_defense_team ||
            type.service_interval_seconds == 0 || !equipment_types_.emplace(type.id, type).second) {
            throw std::invalid_argument("Invalid or duplicate equipment type");
        }
        for (const auto kind : {ServiceKind::preparation, ServiceKind::inspection,
                               ServiceKind::scheduled, ServiceKind::repair}) {
            const auto& service = profile(type, kind);
            if (service.seconds == 0 || service.technicians == 0 ||
                service.costs[static_cast<std::size_t>(Resource::oil_reserve)] != 0) {
                throw std::invalid_argument("Invalid equipment service profile");
            }
        }
    }
    for (const auto& facility : scenario.facilities) {
        const auto& stock = stockpiles_.at(facility.stockpile);
        if (stock.owner == FactionId::neutral || facility.bays == 0 ||
            !facilities_.emplace(facility.id, facility).second) {
            throw std::invalid_argument("Invalid or duplicate maintenance facility");
        }
        for (const auto type : facility.supported_types) {
            if (equipment_types_.at(type).faction != stock.owner) {
                throw std::invalid_argument("Facility support conflicts with stockpile ownership");
            }
        }
    }
    for (const auto& unit : scenario.units) {
        world_.sector(unit.sector);
        faction_name(unit.faction);
        if (unit.name.empty() || unit.faction == FactionId::neutral ||
            !units_.emplace(unit.id, unit).second) {
            throw std::invalid_argument("Invalid or duplicate military unit");
        }
        if (unit.facility) {
            const auto& stock = stockpiles_.at(facilities_.at(*unit.facility).stockpile);
            if (stock.sector != unit.sector || stock.owner != unit.faction) {
                throw std::invalid_argument("Unit facility must be local and friendly");
            }
        }
    }
    for (const auto& [id, unit] : units_) {
        auto parent = unit.parent;
        auto remaining = units_.size();
        while (parent) {
            const auto& ancestor = units_.at(*parent);
            if (--remaining == 0 || ancestor.faction != unit.faction) {
                throw std::invalid_argument("Cyclic or cross-faction command hierarchy");
            }
            parent = ancestor.parent;
        }
    }
    for (const auto& assignment : scenario.equipment) {
        const auto& unit = units_.at(assignment.unit);
        const auto& type = equipment_types_.at(assignment.type);
        if (type.faction != unit.faction) {
            throw std::invalid_argument("Equipment and unit factions do not match");
        }
        auto [entry, inserted] = equipment_.emplace(assignment.id, Equipment{assignment});
        if (!inserted) {
            throw std::invalid_argument("Duplicate equipment ID");
        }
        entry->second.operating_seconds = assignment.operating_seconds;
    }
    for (auto& [id, item] : equipment_) {
        const auto& assignment = item.assignment;
        const auto& type = equipment_types_.at(assignment.type);
        if (!assignment.reserve_source.empty()) {
            item.status = EquipmentStatus::reserve;
        } else if (service_due(item, type)) {
            queue_service(item, ServiceKind::scheduled, 0);
        }
    }
}

void CampaignState::set_equipment_status(Equipment& item, EquipmentStatus status, std::uint64_t tick)
{
    if (item.status != status) {
        record_event(forces_log_, {tick, item.assignment.id, status,
                              item.job ? std::optional{item.job->kind} : std::nullopt});
        item.status = status;
    }
}

void CampaignState::queue_service(Equipment& item, ServiceKind kind, std::uint64_t tick)
{
    item.job = ServiceJob{kind, tick};
    set_equipment_status(item, active_status(kind), tick);
}

bool CampaignState::reserve_equipment(EquipmentId id)
{
    auto& item = equipment_.at(id);
    if (war_ended() || item.status != EquipmentStatus::available) {
        return false;
    }
    set_equipment_status(item, EquipmentStatus::reserved, clock_.tick());
    return true;
}

UnitReadiness CampaignState::unit_readiness(UnitId id) const
{
    units_.at(id);
    UnitReadiness readiness;
    for (const auto& [equipment_id, item] : equipment_) {
        if (item.assignment.unit != id || item.status == EquipmentStatus::reserve ||
            item.status == EquipmentStatus::in_transit) {
            continue;
        }
        if (item.status == EquipmentStatus::lost) {
            ++readiness.lost;
            continue;
        }
        ++readiness.active;
        if (item.status == EquipmentStatus::available || item.status == EquipmentStatus::ready) {
            ++readiness.ready;
        }
    }
    return readiness;
}

bool CampaignState::cancel_equipment_reservation(EquipmentId id)
{
    auto& item = equipment_.at(id);
    if (item.status != EquipmentStatus::reserved) {
        return false;
    }
    set_equipment_status(item, EquipmentStatus::available, clock_.tick());
    return true;
}

bool CampaignState::prepare_equipment(EquipmentId id)
{
    auto& item = equipment_.at(id);
    if (war_ended() || item.status != EquipmentStatus::reserved) {
        return false;
    }
    queue_service(item, ServiceKind::preparation, clock_.tick());
    return true;
}

bool CampaignState::begin_operation(EquipmentId id)
{
    auto& item = equipment_.at(id);
    if (war_ended() || item.status != EquipmentStatus::ready) {
        return false;
    }
    set_equipment_status(item, EquipmentStatus::in_operation, clock_.tick());
    return true;
}

bool CampaignState::finish_operation(EquipmentId id)
{
    auto& item = equipment_.at(id);
    if (item.status != EquipmentStatus::in_operation) {
        return false;
    }
    set_equipment_status(item, EquipmentStatus::returning, clock_.tick());
    item.needs_turnaround = true;
    return true;
}

bool CampaignState::damage_equipment(EquipmentId id, std::uint32_t damage)
{
    auto& item = equipment_.at(id);
    if (damage == 0 || item.status == EquipmentStatus::lost || item.status == EquipmentStatus::reserve ||
        item.status == EquipmentStatus::in_transit) {
        return false;
    }
    item.damage += std::min(damage, 100u - item.damage);
    if (item.status != EquipmentStatus::in_operation && item.status != EquipmentStatus::returning) {
        queue_service(item, ServiceKind::repair, clock_.tick());
    }
    return true;
}

bool CampaignState::lose_equipment(EquipmentId id)
{
    auto& item = equipment_.at(id);
    if (item.status == EquipmentStatus::lost) {
        return false;
    }
    set_equipment_status(item, EquipmentStatus::lost, clock_.tick());
    item.job.reset();
    item.reinforcement_eta.reset();
    item.arrival_delayed = false;
    return true;
}

bool CampaignState::reinforcement_destination_active(UnitId id) const
{
    const auto combat = combat_units_.find(id);
    return combat == combat_units_.end() ||
           (combat->second.status != GroundStatus::destroyed && combat->second.status != GroundStatus::surrendered);
}

bool CampaignState::authorize_reinforcement(EquipmentId id, std::uint64_t eta)
{
    auto& item = equipment_.at(id);
    auto arrival = clock_;
    if (war_ended() || item.status != EquipmentStatus::reserve || eta <= arrival.tick() ||
        !units_.at(item.assignment.unit).facility || !reinforcement_destination_active(item.assignment.unit)) {
        return false;
    }
    arrival.advance(eta - arrival.tick());
    set_equipment_status(item, EquipmentStatus::in_transit, clock_.tick());
    item.reinforcement_eta = eta;
    return true;
}

void CampaignState::set_facility_conditions(FacilityId id, bool operational, std::uint32_t technicians)
{
    auto& facility = facilities_.at(id);
    facility.operational = operational;
    facility.technicians = technicians;
}

bool CampaignState::update_forces(std::uint64_t tick)
{
    const auto before = forces_log_.size();
    std::vector<Equipment*> jobs;
    for (auto& [id, item] : equipment_) {
        const auto& type = equipment_types_.at(item.assignment.type);
        if (item.status == EquipmentStatus::in_operation) {
            const auto remaining = std::numeric_limits<std::uint64_t>::max() - item.operating_seconds;
            item.operating_seconds += std::min<std::uint64_t>(remaining, clock_.tick_seconds());
        } else if (item.status == EquipmentStatus::returning) {
            queue_service(item, ServiceKind::inspection, tick);
        } else if (item.reinforcement_eta && *item.reinforcement_eta <= tick) {
            const auto& unit = units_.at(item.assignment.unit);
            if (reinforcement_destination_active(unit.id) && facilities_.at(*unit.facility).operational &&
                sector_state(unit.sector).controller == unit.faction) {
                set_equipment_status(item, EquipmentStatus::available, tick);
                item.reinforcement_eta.reset();
                item.arrival_delayed = false;
                if (service_due(item, type)) {
                    queue_service(item, ServiceKind::scheduled, tick);
                }
            } else if (!item.arrival_delayed) {
                record_event(forces_log_, {
                    tick, id, EquipmentStatus::in_transit, {}, ForcesEventKind::arrival_delayed
                });
                item.arrival_delayed = true;
            }
        }
        if (item.job) {
            jobs.push_back(&item);
        }
    }
    // Started work has precedence. New work is ordered by request tick and equipment ID.
    std::ranges::sort(jobs, [](const auto* left, const auto* right) {
        return std::tuple{!left->job->started, left->job->requested_tick, left->assignment.id} <
               std::tuple{!right->job->started, right->job->requested_tick, right->assignment.id};
    });
    struct Capacity { std::uint32_t bays; std::uint32_t technicians; };
    std::map<FacilityId, Capacity> capacity;
    for (const auto& [id, facility] : facilities_) {
        capacity.emplace(id, Capacity{facility.bays, facility.technicians});
    }

    bool progressing = false;
    for (auto* item : jobs) {
        auto& job = *item->job;
        const auto& type = equipment_types_.at(item->assignment.type);
        const auto& service = profile(type, job.kind);
        const auto& unit = units_.at(item->assignment.unit);
        if (!unit.facility) {
            set_equipment_status(*item, EquipmentStatus::grounded, tick);
            continue;
        }
        const auto& facility = facilities_.at(*unit.facility);
        const auto& stock = stockpiles_.at(facility.stockpile);
        const auto controller = sector_state(unit.sector).controller;
        if (!facility.operational || stock.sector != unit.sector || stock.owner != unit.faction ||
            (controller != FactionId::neutral && controller != unit.faction) ||
            (!facility.supported_types.empty() &&
             std::ranges::find(facility.supported_types, type.id) == facility.supported_types.end())) {
            set_equipment_status(*item, EquipmentStatus::grounded, tick);
            continue;
        }
        auto& slots = capacity.at(facility.id);
        if (slots.bays == 0 || slots.technicians < service.technicians) {
            set_equipment_status(*item, EquipmentStatus::awaiting_crew, tick);
            continue;
        }
        if (!job.started) {
            auto mission = missions_.end();
            if (job.kind == ServiceKind::preparation && !item->needs_turnaround) {
                mission = std::ranges::find_if(missions_, [&](const auto& entry) {
                    const auto& order = entry.second;
                    return order.preparation_reserved && order.status == MissionStatus::preparing &&
                           order.equipment == item->assignment.id && order.launch_stock == facility.stockpile;
                });
            }
            if (!consume_at(facility.stockpile, service.costs, tick, item->assignment.id, mission != missions_.end())) {
                set_equipment_status(*item, EquipmentStatus::awaiting_parts, tick);
                continue;
            }
            if (mission != missions_.end()) {
                mission->second.preparation_reserved = false;
            }
            job.started = true;
            set_equipment_status(*item, active_status(job.kind), tick);
            record_event(forces_log_, {
                tick, item->assignment.id, item->status, job.kind, ForcesEventKind::service_started
            });
        } else {
            job.work_seconds += std::min(clock_.tick_seconds(), service.seconds - job.work_seconds);
            if (job.work_seconds == service.seconds) {
                const auto kind = job.kind;
                record_event(forces_log_, {
                    tick, item->assignment.id, active_status(kind), kind, ForcesEventKind::service_completed
                });
                item->job.reset();
                if (kind == ServiceKind::repair) {
                    item->damage = 0;
                } else if (kind == ServiceKind::scheduled) {
                    item->last_service_seconds = item->operating_seconds;
                }
                if (kind == ServiceKind::preparation) {
                    const auto status = item->needs_turnaround ? EquipmentStatus::available : EquipmentStatus::ready;
                    set_equipment_status(*item, status, tick);
                    item->needs_turnaround = false;
                } else if (item->damage != 0) {
                    queue_service(*item, ServiceKind::repair, tick);
                } else if (service_due(*item, type)) {
                    queue_service(*item, ServiceKind::scheduled, tick);
                } else if (item->needs_turnaround) {
                    queue_service(*item, ServiceKind::preparation, tick);
                } else {
                    set_equipment_status(*item, EquipmentStatus::available, tick);
                }
                progressing = true;
                continue;
            }
            set_equipment_status(*item, active_status(job.kind), tick);
        }
        --slots.bays;
        slots.technicians -= service.technicians;
        progressing = true;
    }
    return progressing || forces_log_.size() != before ||
           std::ranges::any_of(equipment_, [](const auto& entry) {
               return entry.second.status == EquipmentStatus::in_operation;
           });
}

}  // namespace openwar
