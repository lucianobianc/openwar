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
    // Only observer sessions may switch their inspection perspective.
    viewer::Snapshot snapshot(std::optional<FactionId> observer_perspective = {}) const;
    std::vector<OrderAction> actions_for(UnitId formation) const;
    OrderResult submit(MissionKind kind, const OrderTarget& target);
    bool can_cancel(FactionId owner, MissionId id) const;
    CancelResult cancel(FactionId owner, MissionId id);

private:
    CampaignState& campaign_;
    std::optional<FactionId> faction_;
};

}  // namespace openwar

#endif
