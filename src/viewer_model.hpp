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

enum class MarkerKind { unit, contact, carrier, installation, depot, mission };

enum class Affiliation { friendly, hostile, neutral, unknown, observer };
enum class SymbolStatus { present, reported, destroyed };

struct MilitarySymbol {
    Affiliation affiliation;
    MilitaryIdentity identity{};
    SymbolStatus status = SymbolStatus::present;
};

Affiliation relative_affiliation(std::optional<FactionId> faction, std::optional<FactionId> perspective);

struct MarkerKey {
    MarkerKind kind;
    std::optional<FactionId> faction;
    std::uint32_t id;
    bool operator==(const MarkerKey&) const = default;
};

struct RouteTrace {
    std::vector<geography::Point> vertices;
    geography::Point position;
    geography::Point destination;
    std::size_t leg;
    std::int64_t elapsed_us;
    std::int64_t duration_us;
    std::int64_t fuel_ml;
    bool operator==(const RouteTrace&) const = default;
};

RouteTrace trace_journey(const geography::Route& route, const geography::Progress& progress,
                         geography::Point destination);

struct Marker {
    SectorId sector;
    MarkerKind kind;
    std::optional<FactionId> faction;
    std::uint32_t id;
    geography::Point position;
    bool estimated;
    std::string label;
    std::string detail;
    std::string movement{};
    std::optional<MilitarySymbol> symbol{};
    std::vector<std::string> inspection{};
    std::optional<MissionStatus> mission_status{};
    std::optional<RouteTrace> journey{};
    std::optional<StockpileId> supply_stock{};

    MarkerKey key() const { return {kind, faction, id}; }
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
    std::map<std::uint32_t, RoadReport> roads{};
    std::vector<std::size_t> mission_markers{};  // Indices into markers, in operational list order.
    std::vector<SupplyOrderSummary> supply_requests{};

    geography::Point sector_center(SectorId sector) const;
};

// A null perspective is the explicit observer mode.
Snapshot read_snapshot(const CampaignState& campaign, std::optional<FactionId> perspective);

}  // namespace openwar::viewer

#endif
