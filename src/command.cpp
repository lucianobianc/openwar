#include "command.hpp"

#include <stdexcept>

namespace openwar {

CommandPermissions CommandSession::permissions() const
{
    return {faction_, faction_ && campaign_.can_issue_orders(*faction_)};
}

viewer::Snapshot CommandSession::snapshot(std::optional<FactionId> observer_perspective) const
{
    if (faction_ && (!campaign_.has_operational_view() || campaign_.player_faction() != faction_)) {
        throw std::invalid_argument("campaign does not grant access to that command faction");
    }
    return viewer::read_snapshot(campaign_, faction_ ? faction_ : observer_perspective);
}

std::vector<OrderAction> CommandSession::actions_for(UnitId formation) const
{
    std::vector<OrderAction> actions;
    if (faction_) {
        for (const auto kind : campaign_.available_orders(*faction_, formation)) {
            const bool geographic = kind == MissionKind::attack ||
                (kind == MissionKind::defend && campaign_.combat_enabled());
            actions.push_back({kind, geographic});
        }
    }
    return actions;
}

OrderResult CommandSession::submit(MissionKind kind, const OrderTarget& target)
{
    if (!permissions().may_issue_orders) {
        return {{}, "player has no authority for this faction"};
    }
    if (const auto* point = std::get_if<geography::Point>(&target)) {
        return campaign_.issue_ground_order(*faction_, kind, *point);
    }
    return campaign_.issue_order(*faction_, kind, std::get<SectorId>(target));
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

}  // namespace openwar
