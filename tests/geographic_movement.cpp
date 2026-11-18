#include "geographic_movement.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace openwar::geography;

namespace {

constexpr std::int64_t metre = 1000;
constexpr std::int64_t second = 1'000'000;
constexpr QueryLimits entire_map{60};

void require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

Point point(std::int64_t east, std::int64_t south, std::int64_t elevation = 0)
{
    return {east * metre, south * metre, elevation * metre};
}

Map plain(std::int64_t columns = 3, std::int64_t rows = 2, std::int64_t resolution = 100)
{
    return {columns, rows, 2000 * metre, resolution * metre, {}, {}, {}, {}, {}};
}

Profile wheeled()
{
    Profile profile;
    profile.terrain = {Rate{5000, 20'000}, Rate{2500, 30'000}, Rate{}};
    profile.roads = {Rate{10'000, 10'000, 5000}, Rate{8000, 15'000}};
    return profile;
}

Map river()
{
    auto map = plain();
    map.barriers = {{1900 * metre, 0, 2100 * metre, 4000 * metre}};
    for (int row = 0; row < 40; ++row)
        for (int column = 19; column < 21; ++column)
            map.tiles[{column, row}] = {Cover::water, 0};
    map.nodes = {{10, point(800, 800)}, {20, point(3200, 800)}};
    map.roads = {{100, 10, 20, {}, Surface::paved, Direction::both, Crossing::bridge}};
    map.roads.front().interior_access_points = {point(1300, 800), point(2700, 800)};
    return map;
}

void bridge_fixture()
{
    auto map = river();
    const auto origin = point(1300, 1000);
    const auto destination = point(2700, 1000);
    const auto profile = wheeled();
    const auto route = find_route(map, profile, origin, destination);
    require(route.status == RouteStatus::found, "mixed route is reachable");
    require(route.duration_us == 220 * second && route.fuel_ml == 22'000, "mixed route costs 220 s and 22 L");

    std::int64_t on_road = 0;
    std::int64_t off_road = 0;
    for (const auto& leg : route.legs) {
        if (leg.road)
            on_road += leg.distance_mm;
        else
            off_road += leg.distance_mm;
    }
    require(on_road == 1400 * metre && off_road == 400 * metre, "exact road and terrain distances");
    require(map.sector(point(1999, 800)) == 0 && map.sector(point(2000, 800)) == 1
        && map.sector(point(0, 2000)) == 3 && !map.sector(point(6000, 0))
        && !map.sector(point(0, 4000)) && !map.sector(point(-1, 0)), "half-open sector bounds");

    auto batched = begin(route, 30'000);
    require(advance(map, route, batched, 240 * second) == 20 * second, "unused arrival time");
    auto stepped = begin(route, 30'000);
    advance(map, route, stepped, 60 * second);
    require(stepped.position == point(1500, 800) && stepped.crossings.empty(), "position at 60 s");
    auto resumed = stepped;
    advance(map, route, stepped, 60 * second);
    require(stepped.position == point(2100, 800) && map.sector(stepped.position) == 1
        && stepped.position != destination, "position at 120 s and destination point still ahead");
    advance(map, route, stepped, 60 * second);
    advance(map, route, stepped, 60 * second);
    advance(map, route, resumed, 180 * second);
    for (const auto& result : {batched, stepped, resumed}) {
        require(result.position == destination && result.fuel_ml == 8000 && result.leg == route.legs.size()
            && result.elapsed_us == 220 * second, "batched, stepped and resumed arrival");
        require(result.crossings == std::vector<SectorCrossing>{{0, 1, 110 * second, point(2000, 800)}},
            "single crossing at exactly 110 s");
    }

    auto mid_leg = begin(route, 30'000);
    advance(map, route, mid_leg, 65 * second);
    require(mid_leg.position == point(1550, 800) && mid_leg.entry_paid, "mid-leg checkpoint");
    auto copied = mid_leg;
    advance(map, route, copied, 175 * second);
    require(copied.position == batched.position && copied.fuel_ml == batched.fuel_ml
        && copied.crossings == batched.crossings, "mid-leg checkpoint preserves fuel debit");

    auto road_only = profile;
    road_only.terrain = {};
    require(find_route(map, road_only, origin, destination, entire_map).status == RouteStatus::unreachable,
        "road-only vehicle rejects terrain connectors");
    require(find_route(map, road_only, point(1300, 800), point(2700, 800)).duration_us == 140 * second,
        "road-only vehicle can enter at an interior access point");
    auto without_bridge = map;
    without_bridge.barriers.clear();
    without_bridge.roads.front().crossing = Crossing::ground;
    require(find_route(without_bridge, profile, origin, destination, entire_map).status == RouteStatus::unreachable,
        "ordinary roads cannot cross water even without a vector barrier");
    map.roads.front().interior_access = false;
    require(find_route(map, road_only, point(2000, 800), map.nodes.back().position).duration_us == 120 * second,
        "road-only movement can resume from a point already on the bridge deck");
    map.roads.front().interior_access = true;
    map.roads.front().direction = Direction::forward;
    require(find_route(map, profile, destination, origin, entire_map).status == RouteStatus::unreachable,
        "one-way bridge denies reverse travel");
    map.roads.front().closed = true;
    require(find_route(map, profile, origin, destination, entire_map).status == RouteStatus::unreachable,
        "closed bridge disconnects the map");
    require(find_route(map, profile, origin, destination).status == RouteStatus::limit_exceeded,
        "a cropped search cannot prove a route physically impossible");
    auto interrupted = begin(route, 30'000);
    require(advance(map, route, interrupted, 60 * second) == 20 * second
        && interrupted.position == point(1300, 800) && interrupted.fuel_ml == 26'000,
        "closure before road entry stops at the standing point");
    auto empty = begin(route, 0);
    require(advance(map, route, empty, second) == second && empty.position == origin,
        "insufficient fuel stops before entering the first leg");
    std::cout << "Bridge fixture expanded " << route.raster_expanded << " raster vertices with "
        << route.projection_checks << " access checks.\n";
}

void grade_and_cover()
{
    auto map = river();
    map.nodes = {{10, point(800, 800)}, {20, point(3200, 800, 60)}};
    map.roads.front().waypoints = {point(1300, 800), point(1900, 800, 60)};
    map.roads.front().interior_access_points.back().elevation = 60 * metre;
    for (int row = 0; row < 40; ++row)
        for (int column = 21; column < 60; ++column)
            map.tiles[{column, row}] = {Cover::plain, 60 * metre};
    const auto origin = point(1300, 1000);
    const auto destination = point(2700, 1000, 60);
    const auto profile = wheeled();
    const auto uphill = find_route(map, profile, origin, destination);
    const auto downhill = find_route(map, profile, destination, origin);
    require(uphill.status == RouteStatus::found && uphill.duration_us == 280 * second,
        "600 m uphill approach takes 280 s eastbound");
    require(downhill.status == RouteStatus::found && downhill.duration_us == 220 * second,
        "same approach takes 220 s downhill westbound");
    auto climbing = begin(uphill, 30'000);
    advance(map, uphill, climbing, 100 * second);
    require(climbing.position == point(1600, 800, 30), "elevation interpolates on the road");
    auto steep_limit = profile;
    steep_limit.roads.front().max_uphill_permille = 99;
    require(find_route(map, steep_limit, origin, destination, entire_map).status == RouteStatus::unreachable,
        "vehicle grade limit rejects the uphill section");

    auto forest = plain(1, 1, 50);
    const auto start = point(200, 200);
    const auto finish = point(300, 200);
    const auto clear = find_route(forest, profile, start, finish);
    forest.default_tile.cover = Cover::forest;
    const auto wooded = find_route(forest, profile, start, finish);
    require(clear.duration_us == 20 * second && wooded.duration_us == 40 * second
        && clear.fuel_ml == 2000 && wooded.fuel_ml == 3000,
        "50 m terrain grid prices equal-length plain and forest paths differently");
}

void connectors_and_limits()
{
    auto map = plain();
    const auto profile = wheeled();
    const auto origin = point(1075, 1001);
    const auto destination = point(2703, 997);
    map.nodes = {{10, point(800, 800)}, {20, point(3200, 800)}};
    map.roads = {{100, 10, 20, {}}};
    map.barriers = {{900 * metre, 850 * metre, 1200 * metre, 950 * metre}};
    const auto route = find_route(map, profile, origin, destination);
    require(route.status == RouteStatus::found && route.legs.back().to == destination,
        "arbitrary destination is preserved exactly");
    bool entered = false;
    for (const auto& leg : route.legs) {
        if (leg.road) {
            require(leg.from.east > 1200 * metre || leg.from.east < 900 * metre,
                "blocked nearest access loses to a reachable road entry");
            entered = true;
            break;
        }
    }
    require(entered, "detour still uses the road");

    auto corners = plain(1, 1);
    corners.tiles[{1, 0}] = {Cover::water, 0};
    corners.tiles[{0, 1}] = {Cover::water, 0};
    require(find_route(corners, profile, point(50, 50), point(150, 150),
        {20, 400, 0}).status == RouteStatus::unreachable, "diagonal cannot cut blocked corners");

    auto large = plain(128, 128);
    const auto local = find_route(large, profile, point(125300, 120100), point(126700, 120100));
    require(local.status == RouteStatus::found && local.raster_expanded <= 31 * 17,
        "local search on large map stays inside its raster window");
    require(find_route(map, profile, origin, destination, {8, 20'000, 1}).status
        == RouteStatus::limit_exceeded, "road projection work is capped");
    std::cout << "Mixed fixture expanded " << route.raster_expanded << " raster vertices with "
        << route.projection_checks << " access checks. Large-map local query expanded "
        << local.raster_expanded << " raster vertices.\n";
}

void road_network()
{
    auto map = plain();
    auto profile = wheeled();
    profile.terrain = {};
    profile.roads[static_cast<std::size_t>(Surface::gravel)].fuel_ml_km = 1000;
    const auto origin = point(500, 500);
    const auto destination = point(1500, 500);
    map.nodes = {{1, origin}, {2, destination}, {3, point(1000, 600)},
        {4, point(800, 300)}, {5, point(800, 700)}};
    map.roads = {{1, 1, 2, {}}, {2, 1, 3, {}, Surface::gravel},
        {3, 3, 2, {}, Surface::gravel}, {4, 4, 5, {}}};
    const auto fastest = find_route(map, profile, origin, destination);
    const auto cheapest = find_route(map, profile, origin, destination, {}, RoutePriority::fuel);
    require(fastest.duration_us == 100 * second && cheapest.duration_us > fastest.duration_us
        && cheapest.fuel_ml < fastest.fuel_ml, "time and fuel priorities can choose different roads");
    require(find_route(map, profile, origin, map.nodes.back().position, entire_map).status == RouteStatus::unreachable,
        "crossing polylines without a declared junction do not connect");
}

void long_distance_routing()
{
    auto map = plain(128, 128);
    const auto profile = wheeled();
    const auto origin = point(1000, 1000);
    const auto destination = point(255000, 255000);
    const auto cross_country = find_route(map, profile, origin, destination);
    require(cross_country.status == RouteStatus::found && cross_country.legs.back().to == destination &&
        cross_country.raster_expanded < 2000, "map-wide off-road route needs only a sparse search");
    require(cross_country == find_route(map, profile, origin, destination),
        "regional search geometry, costs and tie-breaking are deterministic");

    auto narrow = plain(128, 128);
    narrow.default_tile.cover = Cover::water;
    for (std::int64_t x = 9; x <= 2550; ++x)
        narrow.tiles[{x, 13}] = {};
    const auto through_strip = find_route(narrow, profile, point(1000, 1350), point(255'000, 1350));
    require(through_strip.status == RouteStatus::found && through_strip.legs.size() == 2540 &&
        through_strip.duration_us == 50'800 * second,
        "a straight fine corridor between regional samples remains usable");

    QueryLimits limited{};
    limited.max_terrain_checks = 1;
    require(find_route(map, profile, origin, destination, limited).status == RouteStatus::limit_exceeded,
        "terrain refinement has a hard work budget");

    // A short rectangle around the requested endpoints misses the passage around this wall.
    map.barriers = {{64'900 * metre, 0, 65'100 * metre, 46'000 * metre}};
    const auto detour = find_route(map, profile, point(40'000, 40'000), point(90'000, 40'000));
    require(detour.status == RouteStatus::found && detour.raster_expanded <= 20'000 &&
        std::ranges::any_of(detour.legs, [](const auto& leg) {
            return leg.to.south > 46'000 * metre;
        }), "regional retry finds a detour beyond the fine window");

    // Fine water and elevation data still constrain regional edges.
    map.barriers.clear();
    map.tiles[{100, 100}] = {Cover::water, 0};
    map.tiles[{120, 120}] = {Cover::plain, 1000 * metre};
    const auto fine_detour = find_route(map, profile, origin, destination);
    require(fine_detour.status == RouteStatus::found && fine_detour.duration_us > cross_country.duration_us &&
        fine_detour.legs.back().to == destination &&
        std::ranges::all_of(fine_detour.legs, [&](const auto& leg) {
            constexpr auto fine_step = 100 * metre;
            return !leg.road && std::abs(leg.to.east - leg.from.east) <= fine_step &&
                std::abs(leg.to.south - leg.from.south) <= fine_step;
        }), "regional terrain edges resolve to fine validated legs");

    map.tiles.clear();
    map.barriers = {{127'900 * metre, 0, 128'100 * metre, 256'000 * metre}};
    map.nodes = {{1, point(2000, 110'000)}, {2, point(127'000, 110'000)},
        {3, point(129'000, 110'000)}, {4, point(254'000, 110'000)}};
    map.roads = {{1, 1, 2, {}, Surface::paved, Direction::forward, Crossing::ground, 10'000, false},
        {2, 2, 3, {}, Surface::paved, Direction::forward, Crossing::bridge, 10'000, false},
        {3, 3, 4, {}, Surface::paved, Direction::forward, Crossing::ground, 10'000, false}};
    auto road_only = profile;
    road_only.terrain = {};
    const auto start = map.nodes.front().position;
    const auto end = map.nodes.back().position;
    const auto highway = find_route(map, road_only, start, end);
    require(highway.status == RouteStatus::found && highway.legs.size() == 3 &&
        highway.raster_expanded == 0 && highway.duration_us == 25'200 * second,
        "long-distance road-only trips use the directed road graph with no raster expansion");
    require(find_route(map, road_only, end, start).status == RouteStatus::unreachable,
        "regional road graph preserves one-way travel");

    const auto mixed_start = point(1800, 110'400);
    const auto mixed_end = point(254'200, 110'400);
    const auto mixed = find_route(map, profile, mixed_start, mixed_end);
    require(mixed.status == RouteStatus::found && mixed.legs.front().from == mixed_start &&
        mixed.legs.back().to == mixed_end && !mixed.legs.front().road && !mixed.legs.back().road &&
        std::ranges::any_of(mixed.legs, [](const auto& leg) { return leg.road == 2; }),
        "mixed regional route connects exact endpoints to a remote bridge");
    auto progress = begin(mixed, mixed.fuel_ml);
    advance(map, mixed, progress, mixed.duration_us);
    require(progress.position == mixed_end && progress.fuel_ml == 0,
        "regional legs are executable with their stated total fuel");
    const SectorAccess denied = [](std::int64_t sector) { return sector % 128 != 64; };
    require(find_route(map, road_only, start, end, {}, RoutePriority::time, denied).status ==
        RouteStatus::unreachable, "a long road cannot bypass intermediate sector permissions");
    map.roads[1].closed = true;
    require(find_route(map, road_only, start, end).status == RouteStatus::unreachable,
        "closing the only remote bridge disconnects the directed network");
    auto drainage = plain(128, 128);
    drainage.default_tile.cover = Cover::water;
    for (std::int64_t x = 9; x <= 2550; ++x)
        for (auto y = x - 1; y <= x + 1; ++y)
            drainage.tiles[{x, y}] = {};
    drainage.nodes = {{1, origin}, {2, destination}};
    drainage.roads = {{1, 1, 2, {}}};
    const auto dry_road = find_route(drainage, road_only, origin, destination);
    require(dry_road.status == RouteStatus::found && dry_road.road_tile_checks < 20'000,
        "validating a diagonal road inspects the terrain along the line");
    drainage.tiles[{1290, 1290}].cover = Cover::water;
    require(find_route(drainage, road_only, origin, destination).status == RouteStatus::unreachable,
        "diagonal line validation still detects water on a non-bridge road");

    std::cout << "Regional off-road, road and mixed queries expanded " << cross_country.raster_expanded
        << ", " << highway.raster_expanded << " and " << mixed.raster_expanded << " vertices.\n";
}

} // namespace

int main()
{
    bridge_fixture();
    grade_and_cover();
    connectors_and_limits();
    road_network();
    long_distance_routing();
}
