#include "campaign.hpp"

#include <algorithm>
#include <stdexcept>

namespace openwar {
namespace {

std::uint16_t decay(std::uint16_t confidence, std::uint64_t age, std::uint32_t half_life)
{
    // Rounded samples of 2^(-i/16), in thousandths, with exact integer half-lives.
    constexpr std::array factors{1000u, 958u, 917u, 878u, 841u, 805u, 771u, 738u,
                                 707u, 677u, 648u, 621u, 595u, 569u, 546u, 522u};
    const auto periods = age / half_life;
    if (periods >= 10) {
        return 0;
    }
    const auto sample = (age % half_life) * 16 / half_life;
    return static_cast<std::uint16_t>((confidence * factors[sample] / 1000) >> periods);
}

}  // namespace

std::string_view confidence_name(std::uint16_t confidence)
{
    if (confidence >= 750) {
        return "confirmed";
    }
    if (confidence >= 350) {
        return "uncertain";
    }
    return "stale";
}

void CampaignState::initialize_operations(const ScenarioDefinition& scenario)
{
    operations_ = scenario.operations;
    if (!operations_.enabled) {
        return;
    }
    if (operations_.territory_half_life_seconds == 0 || operations_.contact_half_life_seconds == 0 ||
        operations_.installation_half_life_seconds == 0 ||
        operations_.max_autonomous_missions == 0 ||
        (operations_.player && *operations_.player != FactionId::west && *operations_.player != FactionId::east)) {
        throw std::invalid_argument("Invalid operations settings");
    }
    objectives_ = scenario.objectives;
    for (const auto& objective : objectives_) {
        world_.sector(objective.sector);
        if (objective.faction != FactionId::west && objective.faction != FactionId::east) {
            throw std::invalid_argument("Invalid objective faction");
        }
    }

    for (const auto faction : {FactionId::west, FactionId::east}) {
        auto& store = intelligence_[faction];
        for (const auto& sector : world_.sectors()) {
            if (sector_state(sector.id).controller == faction) {
                store.sectors.emplace(sector.id, SectorReport{sector.id, faction, 0, 1000, true});
            }
        }
        for (const auto sector : scenario.public_sectors) {
            world_.sector(sector);
            store.sectors.try_emplace(sector, SectorReport{sector, sector_state(sector).controller, 0, 1000});
        }
        for (const auto& state : installations_) {
            if (state.installation.operator_faction == faction) {
                const auto sector = stockpiles_.at(state.installation.stockpile).sector;
                store.installations.emplace(sector, InstallationReport{sector, 0, 1000});
            }
        }
    }
}

FactionView CampaignState::faction_view(FactionId faction, bool include_events) const
{
    const auto& store = intelligence_.at(faction);
    FactionView view;
    const auto age = [this](std::uint64_t tick) {
        return (clock_.tick() - tick) * clock_.tick_seconds();
    };
    for (const auto& [sector, report] : store.sectors) {
        auto result = report;
        // Initial friendly claims can become stale once the scenario can change ownership.
        if (!result.internal || combat_.enabled) {
            result.confidence = decay(report.confidence, age(report.observed_tick),
                                      operations_.territory_half_life_seconds);
        }
        view.sectors.emplace(sector, result);
    }
    for (const auto& [key, report] : store.contacts) {
        auto result = report;
        result.confidence = decay(report.confidence, age(report.observed_tick),
                                  operations_.contact_half_life_seconds);
        view.contacts.push_back(result);
    }
    std::ranges::sort(view.contacts, {}, &ContactReport::id);
    for (const auto& [sector, report] : store.installations) {
        auto result = report;
        result.confidence = decay(report.confidence, age(report.observed_tick),
                                  operations_.installation_half_life_seconds);
        view.installations.push_back(result);
    }
    for (const auto& objective : objectives_) {
        if (objective.faction == faction) {
            view.objectives.push_back(objective);
        }
    }
    for (const auto& [id, mission] : missions_) {
        if (mission.faction == faction) {
            view.missions.push_back(mission);
        }
    }
    if (include_events) {
        for (const auto& entry : operations_log_) {
            if (entry.faction == faction) {
                view.events.push_back(entry);
                view.events.back().sequence = view.events.size() - 1;
            }
        }
    }
    return view;
}

std::uint32_t CampaignState::observe(FactionId faction, SectorId sector)
{
    auto& store = intelligence_.at(faction);
    const auto tick = clock_.tick();
    const auto& state = sector_state(sector);
    const bool open = world_.sector(sector).properties.terrain != Terrain::forest;
    store.sectors.insert_or_assign(sector, SectorReport{sector, state.controller, tick, 1000,
        state.controller == faction, state.control == ControlState::contested, open});
    if (!open) {
        return 0;
    }
    if (std::ranges::any_of(installations_, [this, sector](const auto& state) {
        return stockpiles_.at(state.installation.stockpile).sector == sector;
    })) {
        store.installations.insert_or_assign(sector, InstallationReport{sector, tick, 1000});
    }
    std::uint32_t detected = 0;
    for (const auto& [id, item] : equipment_) {
        const auto& unit = units_.at(item.assignment.unit);
        const auto category = equipment_types_.at(item.assignment.type).category;
        const bool airborne = category == EquipmentCategory::helicopter &&
            (item.status == EquipmentStatus::in_operation || item.status == EquipmentStatus::returning);
        if (unit.faction == faction || unit.sector != sector || item.status == EquipmentStatus::reserve ||
            item.status == EquipmentStatus::in_transit || item.status == EquipmentStatus::lost ||
            airborne) {
            continue;
        }
        const auto found = store.contacts.find(id);
        const auto contact = found == store.contacts.end() ?
            ContactId{static_cast<std::uint32_t>(store.contacts.size() + 1)} : found->second.id;
        const auto classification = category == EquipmentCategory::helicopter ? "aircraft" : "ground";
        store.contacts.insert_or_assign(id, ContactReport{contact, sector, classification, tick, 1000});
        ++detected;
    }
    return detected;
}

}  // namespace openwar
