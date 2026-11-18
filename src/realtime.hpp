#ifndef OPENWAR_REALTIME_HPP
#define OPENWAR_REALTIME_HPP

#include "campaign.hpp"

#include <chrono>
#include <iosfwd>
#include <limits>

namespace openwar::cli {

// Convert absolute elapsed time so refresh intervals cannot accumulate drift.
inline std::uint64_t ticks_due(std::chrono::duration<long double> elapsed,
                               std::uint32_t speed, std::uint32_t tick_seconds)
{
    const auto ticks = elapsed.count() * speed / tick_seconds;
    const auto maximum = std::numeric_limits<std::uint64_t>::max() / tick_seconds;
    if (ticks >= static_cast<long double>(maximum)) {
        return maximum;
    }
    return static_cast<std::uint64_t>(ticks);
}

void print_events(const CampaignState& campaign);
void print_operations_events(const CampaignState& campaign, FactionId perspective, bool logistics = false);
void print_operations(const CampaignState& campaign, FactionId perspective, bool events);
void print_campaign_summary(std::ostream& out, const CampaignState& campaign, std::uint64_t seed);
void report_forces(std::ostream& out, const CampaignState& campaign, bool compact = false);
void run_realtime(CampaignState& campaign, std::uint32_t speed,
                  std::optional<std::uint64_t> tick_limit, bool events,
                  std::optional<FactionId> perspective = {}, bool logistics = false);

}  // namespace openwar::cli

#endif
