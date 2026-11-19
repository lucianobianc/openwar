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
    for (const auto doctrine : operations_.doctrines) {
        if (doctrine < StrategicDoctrine::balanced || doctrine > StrategicDoctrine::defensive ||
            (doctrine != StrategicDoctrine::balanced &&
             !operations_.strategic_planning_enabled(scenario.combat.enabled))) {
            throw std::invalid_argument("Invalid strategic doctrine or disabled strategic planner");
        }
    }
    // Carriers can report road conditions in geographic scenarios without military operations.
    if (authored_geography_) {
        intelligence_.try_emplace(FactionId::west);
        intelligence_.try_emplace(FactionId::east);
    }
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
    if (const auto strategy = strategies_.find(faction); strategy != strategies_.end()) {
        view.strategy = strategy->second;
    }
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
    for (const auto& [road, report] : store.roads) {
        view.roads.emplace(road, aged_road_report(report));
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

RoadReport CampaignState::aged_road_report(RoadReport report) const
{
    // Logistics can report during the next tick, before the campaign clock advances.
    const auto tick = std::max(clock_.tick(), report.observed_tick);
    const auto half_life = operations_.installation_half_life_seconds;
    report.confidence = decay(1000, (tick - report.observed_tick) * clock_.tick_seconds(),
        half_life ? half_life : OperationsSettings{}.installation_half_life_seconds);
    return report;
}

bool CampaignState::road_known_closed(FactionId faction, std::uint32_t road) const
{
    const auto store = intelligence_.find(faction);
    if (store == intelligence_.end()) {
        return false;
    }
    const auto report = store->second.roads.find(road);
    return report != store->second.roads.end() && report->second.closed && aged_road_report(report->second).confirmed();
}

bool CampaignState::observe_road(FactionId faction, std::uint32_t road, SectorId sector, std::uint64_t tick)
{
    const bool closed = closed_roads_.contains(road);
    auto& reports = intelligence_.at(faction).roads;
    const auto prior = reports.find(road);
    if (prior == reports.end() ? closed : prior->second.closed != closed) {
        record_event(operations_log_, {tick, faction, sector, {},
            "road #" + std::to_string(road) + (closed ? " reported closed" : " reported open")});
    }
    reports.insert_or_assign(road, RoadReport{closed, tick});
    return !closed;
}

bool CampaignState::ground_recon_visibility(geography::Point point, SectorId sector) const
{
    const auto tile = movement_map_.tiles.find({point.east / movement_map_.tile_mm,
                                                point.south / movement_map_.tile_mm});
    const auto cover = tile == movement_map_.tiles.end() ? movement_map_.default_tile.cover : tile->second.cover;
    return world_.sector(sector).properties.terrain != Terrain::forest && cover != geography::Cover::forest;
}

std::uint32_t CampaignState::observe(FactionId faction, SectorId sector, ObservationScope scope)
{
    auto& store = intelligence_.at(faction);
    const auto tick = clock_.tick();
    const auto& state = sector_state(sector);
    const bool detects_contacts = scope != ObservationScope::territory &&
        world_.sector(sector).properties.terrain != Terrain::forest;
    SectorReport report{sector, state.controller, tick, 1000, state.controller == faction,
        state.control == ControlState::contested, detects_contacts && scope == ObservationScope::sector_survey};
    if (!report.surveyed && scope != ObservationScope::sector_survey) {
        if (const auto prior = store.sectors.find(sector); prior != store.sectors.end()) {
            report.earlier_survey_tick = prior->second.surveyed ? std::optional{prior->second.observed_tick} :
                                                                prior->second.earlier_survey_tick;
            if (report.earlier_survey_tick == tick) {
                report.surveyed = true;
                report.earlier_survey_tick.reset();
            }
        }
    }
    store.sectors.insert_or_assign(sector, report);
    if (!detects_contacts) {
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
