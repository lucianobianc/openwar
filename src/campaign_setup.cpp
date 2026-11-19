#include "scenario.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace openwar {
namespace {

constexpr std::array sides{FactionId::west, FactionId::east};

void adjust_strength(ScenarioDefinition& scenario, FactionId side, StartingPreset preset)
{
    std::set<UnitId> deployed;
    for (const auto& combat : scenario.combat_units) {
        const auto unit = std::ranges::find(scenario.units, combat.unit, &MilitaryUnit::id);
        if (unit != scenario.units.end() && unit->faction == side)
            deployed.insert(unit->id);
    }
    std::map<EquipmentTypeId, std::uint32_t> service_intervals;
    for (const auto& type : scenario.equipment_types) {
        if (type.faction == side && type.can_occupy && type.category != EquipmentCategory::helicopter)
            service_intervals.emplace(type.id, type.service_interval_seconds);
    }

    std::map<std::pair<UnitId, EquipmentTypeId>, std::vector<EquipmentId>> groups;
    std::uint32_t next = 0;
    for (const auto& equipment : scenario.equipment) {
        next = std::max(next, static_cast<std::uint32_t>(equipment.id));
        const auto service = service_intervals.find(equipment.type);
        if (service != service_intervals.end() && deployed.contains(equipment.unit) &&
            equipment.reserve_source.empty() && equipment.operating_seconds < service->second) {
            groups[{equipment.unit, equipment.type}].push_back(equipment.id);
        }
    }

    bool changed = false;
    std::set<EquipmentId> removed;
    for (const auto& [group, items] : groups) {
        if (items.size() < 2)
            continue;
        changed = true;
        if (preset == StartingPreset::reduced) {
            removed.insert(*std::ranges::max_element(items));
        } else {
            if (next == std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("No free equipment identifiers for reinforcements");
            scenario.equipment.push_back({EquipmentId{++next}, group.second, group.first});
        }
    }
    if (!changed)
        throw std::invalid_argument(std::string(faction_name(side)) + " has no repeated ready ground equipment to adjust");
    if (!removed.empty()) {
        std::erase_if(scenario.equipment, [&](const auto& item) { return removed.contains(item.id); });
    }
}

void adjust_stocks(ScenarioDefinition& scenario, FactionId side, StartingPreset preset)
{
    bool changed = false;
    for (auto& stock : scenario.stockpiles) {
        if (stock.owner != side)
            continue;
        for (std::size_t i = 0; i < stock.quantity.size(); ++i) {
            const auto amount = stock.quantity[i];
            if (stock.reserved[i] > amount || amount > stock.capacity[i])
                throw std::invalid_argument("Invalid starting stock balance");
            const auto free = amount - stock.reserved[i];
            // Use differences instead of multiplying potentially full-width quantities.
            auto& updated = stock.quantity[i];
            if (preset == StartingPreset::reduced) {
                updated -= free / 4 + (free % 4 != 0);
            } else {
                updated += std::min(free / 4, stock.capacity[i] - amount);
            }
            changed |= updated != amount;
        }
    }
    if (!changed)
        throw std::invalid_argument(std::string(faction_name(side)) + " has no stock balance affected by this preset");
}

}  // namespace

ScenarioDefinition customize_scenario(ScenarioDefinition scenario, const std::filesystem::path& source,
                                     const CampaignOptions& requested)
{
    auto options = requested;
    if (!scenario.geographic_world || source.extension() != ".owscenario")
        throw std::invalid_argument("Campaign settings require an authored military scenario");
    if (options.duration_minutes) {
        const auto minutes = *options.duration_minutes;
        const auto seconds = std::uint64_t{minutes} * 60;
        if (minutes == 0)
            throw std::invalid_argument("Time limit must be positive");
        if (scenario.combat.time_limit_seconds == seconds) {
            options.duration_minutes.reset();
        } else {
            if (!scenario.combat.enabled || std::ranges::any_of(scenario.victory_objectives, [&](const auto& rule) {
                return rule.hold_seconds >= seconds;
            }))
                throw std::invalid_argument("Time limit must exceed the scenario's victory hold periods");
            scenario.combat.time_limit_seconds = seconds;
        }
    }
    if (options.autonomous_orders) {
        const auto limit = *options.autonomous_orders;
        if (!scenario.operations.enabled || !scenario.operations.planner_interval_ticks || limit == 0)
            throw std::invalid_argument("A positive autonomous order limit requires an enabled planner");
        if (limit == scenario.operations.max_autonomous_missions) {
            options.autonomous_orders.reset();
        } else {
            scenario.operations.max_autonomous_missions = limit;
        }
    }
    for (std::size_t i = 0; i < sides.size(); ++i) {
        if (options.strength[i] > StartingPreset::increased || options.strength[i] < StartingPreset::original ||
            options.stocks[i] > StartingPreset::increased || options.stocks[i] < StartingPreset::original)
            throw std::invalid_argument("Unknown campaign preset");
        if (options.strength[i] != StartingPreset::original)
            adjust_strength(scenario, sides[i], options.strength[i]);
        if (options.stocks[i] != StartingPreset::original)
            adjust_stocks(scenario, sides[i], options.stocks[i]);
    }
    if (options != CampaignOptions{}) {
        const auto reference = referenced_military_world(source);
        const auto world = std::filesystem::absolute(reference).lexically_normal();
        const auto package = std::filesystem::absolute(source).parent_path().lexically_normal();
        const auto relative = world.lexically_relative(package);
        scenario.setup = CampaignSetupRecord{source.filename().generic_string(),
            (relative.empty() ? world : relative).generic_string(), options};
    }
    return scenario;
}

}  // namespace openwar
