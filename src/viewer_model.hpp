#ifndef OPENWAR_VIEWER_MODEL_HPP
#define OPENWAR_VIEWER_MODEL_HPP

#include "campaign.hpp"

namespace openwar::viewer {

struct Tile {
    Terrain terrain;
    std::optional<FactionId> controller{};
    bool contested = false;
    std::uint16_t confidence = 1000;
    std::optional<std::uint64_t> observed_tick{};
};

enum class MarkerKind { unit, contact, installation, mission };

struct Marker {
    SectorId sector;
    MarkerKind kind;
    std::optional<FactionId> faction;
    std::string label;
    std::string detail;
};

struct Event {
    std::uint64_t tick;
    std::string description;
    std::uint64_t sequence;
};

struct Snapshot {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t sector_size_m;
    std::uint64_t tick;
    std::uint64_t elapsed_seconds;
    WarResult war;
    std::vector<Tile> tiles;
    std::vector<Marker> markers;
    std::vector<Event> events;
};

// A null perspective is the explicit observer mode.
Snapshot read_snapshot(const CampaignState& campaign, std::optional<FactionId> perspective);

}  // namespace openwar::viewer

#endif
