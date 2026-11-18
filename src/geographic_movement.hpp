#ifndef OPENWAR_GEOGRAPHIC_MOVEMENT_HPP
#define OPENWAR_GEOGRAPHIC_MOVEMENT_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openwar::geography {

struct Point {
    std::int64_t east{};
    std::int64_t south{};
    std::int64_t elevation{};
    auto operator<=>(const Point&) const = default;
};

enum class Cover { plain, forest, water, urban };
enum class Surface { paved, gravel, count };
enum class Direction { both, forward, reverse };
enum class Crossing { ground, bridge };

struct Tile {
    Cover cover{Cover::plain};
    std::int64_t elevation{};
};

struct Rectangle {
    std::int64_t west, north, east, south;
};

struct Waterway {
    Point from, to;
};

struct RoadNode {
    std::uint32_t id;
    Point position;
    std::uint64_t osm_node_id{};
};

struct RoadSegment {
    std::uint32_t id, from, to;
    std::vector<Point> waypoints;
    Surface surface{Surface::paved};
    Direction direction{Direction::both};
    Crossing crossing{Crossing::ground};
    std::int64_t speed_limit_mm_s{10'000};
    bool interior_access{true};
    bool closed{false};
    // An empty list allows projected access anywhere on an open interior.
    std::vector<Point> interior_access_points{};
    std::uint32_t slots = 1;
    std::uint64_t osm_way_id{};
};

enum class SiteKind {
    aerodrome, runway, helipad, port, rail_station, road_junction, open_land, industrial_land, urban_land
};

enum class OsmElement { node, way, relation };

struct SiteFeature {
    SiteKind kind;
    OsmElement element;
    std::uint64_t source_id;
    Point position;
    std::uint64_t length_mm{}, width_mm{}, area_m2{};  // Zero indicates unavailable.
    std::string name, surface, geometry_wkt;
};

struct Map {
    std::int64_t columns, rows, sector_mm, tile_mm;
    Tile default_tile;
    std::map<std::pair<std::int64_t, std::int64_t>, Tile> tiles;
    std::vector<Rectangle> barriers;
    std::vector<RoadNode> nodes;
    std::vector<RoadSegment> roads;
    std::vector<Waterway> waterways{};
    std::string provenance{};
    std::vector<SiteFeature> site_features{};

    [[nodiscard]] std::optional<std::int64_t> sector(Point point) const;
    [[nodiscard]] bool valid_position(Point point) const;
    // Stable source order. References remain valid while the catalog is unchanged.
    [[nodiscard]] std::vector<const SiteFeature*> site_candidates(SiteKind kind) const;
};

struct Rate {
    // A zero speed denies access. Fuel is in millilitres per kilometre.
    std::int64_t speed_mm_s{};
    std::int64_t fuel_ml_km{};
    std::int64_t uphill_speed_mm_s{};
    std::int64_t max_uphill_permille{1000};
    std::int64_t max_downhill_permille{1000};
};

struct Profile {
    // Urban cover uses the plain mobility rate.
    std::array<Rate, 3> terrain{};
    std::array<Rate, static_cast<std::size_t>(Surface::count)> roads{};
};

struct Leg {
    Point from, to;
    std::int64_t distance_mm, duration_us, fuel_ml;
    std::optional<std::uint32_t> road;
    bool operator==(const Leg&) const = default;
};

struct QueryLimits {
    // Fine searches use this margin around the two endpoints.
    std::int64_t margin_tiles{8};
    std::size_t max_raster_vertices{20'000};
    std::size_t max_projection_checks{200'000};
    std::size_t max_road_tile_checks{200'000};
    std::size_t max_terrain_checks{2'000'000};
};

enum class RouteStatus { found, unreachable, limit_exceeded };
enum class RoutePriority { time, fuel };

struct Route {
    RouteStatus status{RouteStatus::unreachable};
    Point origin;
    std::vector<Leg> legs;
    std::int64_t duration_us{}, fuel_ml{};
    std::size_t raster_expanded{}, projection_checks{}, road_tile_checks{};
    bool operator==(const Route&) const = default;
};

using SectorAccess = std::function<bool(std::int64_t)>;

[[nodiscard]] Route find_route(const Map& map, const Profile& profile, Point origin,
    Point destination, QueryLimits limits = {}, RoutePriority priority = RoutePriority::time,
    const SectorAccess& allowed = {});

struct SectorCrossing {
    std::int64_t from, to, elapsed_us;
    Point position;
    auto operator<=>(const SectorCrossing&) const = default;
};

struct Progress {
    Point position;
    std::size_t leg{};
    std::int64_t leg_elapsed_us{}, elapsed_us{}, fuel_ml{};
    bool entry_paid{};
    std::vector<SectorCrossing> crossings;
    bool operator==(const Progress&) const = default;
};

[[nodiscard]] Progress begin(const Route& route, std::int64_t fuel_ml);
// Return unspent time. A closed upcoming segment or insufficient fuel stops at the current position.
// An observer can stop the journey at the crossing it receives by returning false.
using CrossingObserver = std::function<bool(const SectorCrossing&)>;
std::int64_t advance(const Map& map, const Route& route, Progress& progress, std::int64_t duration_us,
    const CrossingObserver& observer = {});
[[nodiscard]] Point position_at(const Leg& leg, std::int64_t elapsed_us);
[[nodiscard]] std::int64_t horizontal_distance(Point a, Point b);
[[nodiscard]] bool allows_leg(const Map& map, const Leg& leg, const SectorAccess& allowed,
                              std::int64_t elapsed_us = 0);

} // namespace openwar::geography

#endif
