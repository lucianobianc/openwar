#include "command.hpp"

#include <stdexcept>

namespace openwar {

CommandPermissions CommandSession::permissions() const
{
    return {faction_, faction_ && campaign_.can_issue_orders(*faction_)};
}

bool CommandSession::allows_perspective() const
{
    return !faction_ || (campaign_.has_operational_view() && campaign_.player_faction() == faction_);
}

viewer::Snapshot CommandSession::snapshot(std::optional<FactionId> observer_perspective) const
{
    if (!allows_perspective()) {
        throw std::invalid_argument("campaign does not grant access to that command faction");
    }
    return viewer::read_snapshot(campaign_, faction_ ? faction_ : observer_perspective);
}

std::vector<OrderAction> CommandSession::actions_for(UnitId formation) const
{
    std::vector<OrderAction> actions;
    if (faction_) {
        for (const auto kind : campaign_.available_orders(*faction_, formation)) {
            const bool geographic = geographic_ground_order(kind) || kind == MissionKind::attack ||
                (kind == MissionKind::defend && campaign_.combat_enabled());
            actions.push_back({kind, geographic});
        }
    }
    return actions;
}

OrderPreview CommandSession::preview(MissionKind kind, const OrderTarget& target,
                                   std::optional<UnitId> formation, std::span<const geography::Point> via) const
{
    const auto tick = campaign_.clock().tick();
    if (!permissions().may_issue_orders) {
        return {tick, "player has no authority for this faction"};
    }
    const auto* point = std::get_if<geography::Point>(&target);
    if (!point && !via.empty()) {
        return {tick, "waypoints require an exact geographic destination"};
    }
    const auto plan = point ? campaign_.preview_order(*faction_, kind, *point, formation, via) :
        campaign_.preview_order(*faction_, kind, std::get<SectorId>(target), formation);
    OrderPreview result{tick, plan.reason};
    if (!plan.candidate) {
        return result;
    }
    const auto& mission = *plan.candidate;
    const auto& item = campaign_.equipment().at(mission.equipment);
    result.formation = item.assignment.unit;
    result.formation_name = campaign_.units().at(*result.formation).name;
    if (mission.ground_journey) {
        const auto& journey = *mission.ground_journey;
        result.route = viewer::trace_journey(journey.route, journey.progress, *mission.ground_destination);
    } else {
        result.abstract_duration_seconds = mission.duration_ticks * campaign_.clock().tick_seconds();
        result.launch_reservation = campaign_.equipment_types().at(item.assignment.type).preparation.costs;
        result.launch_reservation[static_cast<std::size_t>(Resource::fuel)] += mission.fuel;
    }
    return result;
}

OrderResult CommandSession::submit(MissionKind kind, const OrderTarget& target,
                                   std::optional<UnitId> formation, std::span<const geography::Point> via)
{
    if (!permissions().may_issue_orders) {
        return {{}, "player has no authority for this faction"};
    }
    if (const auto* point = std::get_if<geography::Point>(&target)) {
        return campaign_.issue_ground_order(*faction_, kind, *point, formation, via);
    }
    if (!via.empty()) {
        return {{}, "waypoints require an exact geographic destination"};
    }
    return campaign_.issue_order(*faction_, kind, std::get<SectorId>(target), formation);
}

std::optional<bool> CommandSession::operational_reserve(UnitId formation) const
{
    return faction_ ? campaign_.operational_reserve(*faction_, formation) : std::nullopt;
}

bool CommandSession::set_operational_reserve(UnitId formation, bool reserved)
{
    return faction_ && campaign_.set_operational_reserve(*faction_, formation, reserved);
}

bool CommandSession::can_cancel(FactionId owner, MissionId id) const
{
    return faction_ == owner && campaign_.can_cancel_order(owner, id);
}

CancelResult CommandSession::cancel(FactionId owner, MissionId id)
{
    if (faction_ != owner || !permissions().may_issue_orders) {
        return {false, "player has no authority for this faction"};
    }
    if (!campaign_.cancel_order(owner, id)) {
        return {false, "order is no longer eligible for cancellation"};
    }
    return {true, "order cancelled"};
}

SupplyOrderPreview CommandSession::preview_supply(SupplyTarget target, Resource resource, std::uint64_t quantity) const
{
    return faction_ ? campaign_.preview_supply(*faction_, target, resource, quantity) :
                      SupplyOrderPreview{"player has no authority to request supplies"};
}

SupplyOrderResult CommandSession::submit_supply(SupplyTarget target, Resource resource, std::uint64_t quantity)
{
    return faction_ ? campaign_.submit_supply(*faction_, target, resource, quantity) :
                      SupplyOrderResult{{}, "player has no authority to request supplies"};
}

bool CommandSession::can_cancel_supply(ShipmentId id) const
{
    return faction_ && campaign_.can_cancel_supply(*faction_, id);
}

CancelResult CommandSession::cancel_supply(ShipmentId id)
{
    if (!faction_ || !campaign_.cancel_supply(*faction_, id)) {
        return {false, "supply request is not eligible for cancellation"};
    }
    return {true, "supply request cancelled"};
}

}  // namespace openwar
