#ifndef OPENWAR_COMMAND_HPP
#define OPENWAR_COMMAND_HPP

#include "viewer_model.hpp"

#include <variant>

namespace openwar {

struct CommandPermissions {
    std::optional<FactionId> faction;
    bool may_issue_orders;
};

struct OrderAction {
    MissionKind kind;
    bool geographic_destination;
};

using OrderTarget = std::variant<SectorId, geography::Point>;

struct OrderPreview {
    std::uint64_t tick;
    std::string_view reason;
    std::optional<UnitId> formation{};
    std::string formation_name{};
    std::optional<viewer::RouteTrace> route{};
    std::optional<std::uint64_t> abstract_duration_seconds{};
    ResourceAmounts launch_reservation{};
};

struct CancelResult {
    bool cancelled;
    std::string_view reason;
};

// A local frontend chooses its role when opening a campaign. The campaign retains authority.
class CommandSession {
public:
    CommandSession(CampaignState& campaign, std::optional<FactionId> faction = {})
        : campaign_(campaign), faction_(faction) {}

    CommandPermissions permissions() const;
    bool allows_perspective() const;
    // Only observer sessions may switch their inspection perspective.
    viewer::Snapshot snapshot(std::optional<FactionId> observer_perspective = {}) const;
    std::vector<OrderAction> actions_for(UnitId formation) const;
    OrderPreview preview(MissionKind kind, const OrderTarget& target,
                         std::optional<UnitId> formation = {}, std::span<const geography::Point> via = {}) const;
    OrderResult submit(MissionKind kind, const OrderTarget& target,
                       std::optional<UnitId> formation = {}, std::span<const geography::Point> via = {});
    bool can_cancel(FactionId owner, MissionId id) const;
    CancelResult cancel(FactionId owner, MissionId id);
    SupplyOrderPreview preview_supply(SupplyTarget target, Resource resource, std::uint64_t quantity) const;
    SupplyOrderResult submit_supply(SupplyTarget target, Resource resource, std::uint64_t quantity);
    bool can_cancel_supply(ShipmentId id) const;
    CancelResult cancel_supply(ShipmentId id);

private:
    CampaignState& campaign_;
    std::optional<FactionId> faction_;
};

}  // namespace openwar

#endif
