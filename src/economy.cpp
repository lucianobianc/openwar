#include "campaign.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <utility>

namespace openwar {
namespace {

constexpr std::uint32_t full_integrity = 100;

bool empty(const ResourceAmounts& amounts)
{
    return std::ranges::all_of(amounts, [](auto amount) { return amount == 0; });
}

bool can_apply(const Stockpile& stock, const ResourceAmounts& inputs,
               const ResourceAmounts& outputs)
{
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i] > stock.quantity[i] - stock.reserved[i] ||
            outputs[i] > stock.capacity[i] - (stock.quantity[i] - inputs[i])) {
            return false;
        }
    }
    return true;
}

void apply(Stockpile& stock, const ResourceAmounts& inputs, const ResourceAmounts& outputs)
{
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        stock.quantity[i] = stock.quantity[i] - inputs[i] + outputs[i];
    }
}

}  // namespace

std::string_view resource_name(Resource resource)
{
    constexpr std::array<std::string_view, static_cast<std::size_t>(Resource::count)> names{
        "oil reserve (L)", "crude oil (L)", "fuel (L)", "materials", "spare parts", "supplies", "ammunition"
    };
    return names.at(static_cast<std::size_t>(resource));
}

void CampaignState::initialize_economy(ScenarioDefinition& scenario)
{
    if (economy_interval_ticks_ == 0 || minimum_integrity_ > full_integrity) {
        throw std::invalid_argument("Invalid economy interval or minimum integrity");
    }
    for (const auto& stock : scenario.stockpiles) {
        world_.sector(stock.sector);
        faction_name(stock.owner);
        if (!empty(stock.reserved)) {
            throw std::invalid_argument("Initial stock cannot have transport reservations");
        }
        if (!std::ranges::equal(stock.quantity, stock.capacity, std::less_equal<>{})) {
            throw std::invalid_argument("Initial stock exceeds capacity");
        }
        if (!stockpiles_.emplace(stock.id, stock).second) {
            throw std::invalid_argument("Duplicate stockpile ID");
        }
    }

    std::ranges::sort(scenario.installations, {}, &Installation::id);
    for (auto& installation : scenario.installations) {
        const auto& recipe = installation.recipe;
        const auto& stock = stockpiles_.at(installation.stockpile);
        faction_name(installation.operator_faction);
        if (!installations_.empty() && installations_.back().installation.id == installation.id) {
            throw std::invalid_argument("Duplicate installation ID");
        }
        if (installation.name.empty() || installation.integrity > full_integrity ||
            installation.operator_faction != stock.owner) {
            throw std::invalid_argument("Invalid installation name, integrity, or stockpile owner");
        }
        if (recipe.intervals_per_batch == 0 || empty(recipe.inputs) || empty(recipe.outputs) ||
            recipe.outputs[static_cast<std::size_t>(Resource::oil_reserve)] != 0 ||
            !std::ranges::equal(recipe.inputs, stock.capacity, std::less_equal<>{}) ||
            !std::ranges::equal(recipe.outputs, stock.capacity, std::less_equal<>{})) {
            throw std::invalid_argument("Invalid production recipe or insufficient configured capacity");
        }
        installations_.push_back({std::move(installation)});
    }
}

bool CampaignState::consume(StockpileId id, const ResourceAmounts& amounts)
{
    return consume_at(id, amounts, clock_.tick());
}

bool CampaignState::consume_at(StockpileId id, const ResourceAmounts& amounts, std::uint64_t tick,
                               std::optional<EquipmentId> equipment)
{
    auto& stock = stockpiles_.at(id);
    if (!can_apply(stock, amounts, {})) {
        return false;
    }
    if (!empty(amounts)) {
        economy_ledger_.push_back({tick, id, std::nullopt, amounts, {}, {}, equipment});
        apply(stock, amounts, {});
    }
    return true;
}

void CampaignState::set_operating_conditions(InstallationId id, bool has_power,
                                            std::uint32_t workers, std::uint32_t integrity)
{
    if (integrity > full_integrity) {
        throw std::invalid_argument("Integrity must be between 0 and 100");
    }
    const auto found = std::ranges::find_if(installations_, [id](const auto& state) {
        return state.installation.id == id;
    });
    if (found == installations_.end()) {
        throw std::out_of_range("Unknown installation ID");
    }
    found->installation.has_power = has_power;
    found->installation.workers = workers;
    found->installation.integrity = integrity;
}

bool CampaignState::update_economy(std::uint64_t tick)
{
    // Reserve the maximum number of entries for this interval before changing state.
    const auto remaining = economy_ledger_.max_size() - economy_ledger_.size();
    if (installations_.size() > remaining) {
        throw std::length_error("Economy ledger limit exceeded");
    }
    if (economy_ledger_.capacity() - economy_ledger_.size() < installations_.size()) {
        const auto extra = std::max(installations_.size(),
                                    std::min(remaining, economy_ledger_.capacity()));
        economy_ledger_.reserve(economy_ledger_.size() + extra);
    }

    bool changed = false;
    for (auto& state : installations_) {
        const auto& installation = state.installation;
        const auto& recipe = installation.recipe;
        auto& stock = stockpiles_.at(installation.stockpile);
        if (tick * clock_.tick_seconds() < state.reactivation_seconds ||
            installation.operator_faction == FactionId::neutral ||
            installation.integrity == 0 || installation.integrity < minimum_integrity_ ||
            installation.workers < recipe.workers_required ||
            (recipe.requires_power && !installation.has_power) ||
            !can_apply(stock, recipe.inputs, recipe.outputs)) {
            continue;
        }

        changed = true;
        state.work += installation.integrity;
        const auto batch_work = std::uint64_t{recipe.intervals_per_batch} * full_integrity;
        if (state.work >= batch_work) {
            economy_ledger_.push_back({tick, stock.id, installation.id, recipe.inputs, recipe.outputs});
            apply(stock, recipe.inputs, recipe.outputs);
            state.work -= batch_work;
        }
    }
    return changed;
}

}  // namespace openwar
