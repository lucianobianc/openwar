#include "geographic_movement.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <queue>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace openwar::geography {
namespace {

// Bound coordinates so coordinate products and projected offsets fit in a wide integer.
using Wide = __int128_t;
constexpr std::int64_t coordinate_limit = 1'000'000'000'000;
constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

bool valid_elevation(std::int64_t elevation)
{
    return elevation >= -coordinate_limit && elevation <= coordinate_limit;
}

std::int64_t extent(std::int64_t sectors, std::int64_t size)
{
    if (sectors <= 0 || size <= 0 || Wide(sectors) * size > coordinate_limit)
        throw std::invalid_argument("invalid geographic extent");
    return sectors * size;
}

std::int64_t checked(Wide value)
{
    if (value > maximum || value < 0)
        throw std::overflow_error("geographic movement cost overflow");
    return static_cast<std::int64_t>(value);
}

std::int64_t distance(Point a, Point b)
{
    const Wide dx = b.east - a.east;
    const Wide dy = b.south - a.south;
    const Wide squared = dx * dx + dy * dy;
    std::int64_t low = 0;
    std::int64_t high = std::abs(b.east - a.east) + std::abs(b.south - a.south);
    while (low < high) {
        const auto mid = low + (high - low) / 2;
        if (Wide(mid) * mid < squared)
            low = mid + 1;
        else
            high = mid;
    }
    return low;
}

Point interpolate(Point a, Point b, Wide fraction, Wide total)
{
    const auto coordinate = [fraction, total](std::int64_t from, std::int64_t to) {
        return from + static_cast<std::int64_t>(Wide(to - from) * fraction / total);
    };
    return {coordinate(a.east, b.east), coordinate(a.south, b.south),
        coordinate(a.elevation, b.elevation)};
}

Point project(Point a, Point b, Point point)
{
    const Wide dx = b.east - a.east;
    const Wide dy = b.south - a.south;
    const Wide squared = dx * dx + dy * dy;
    const Wide dot = dx * (point.east - a.east) + dy * (point.south - a.south);
    return interpolate(a, b, std::clamp(dot, Wide(0), squared), squared);
}

Wide side(Point a, Point b, Point p)
{
    return (Wide(b.east) - a.east) * (Wide(p.south) - a.south)
        - (Wide(b.south) - a.south) * (Wide(p.east) - a.east);
}

bool intersects(Point a, Point b, Rectangle box)
{
    if (std::max(a.east, b.east) < box.west || std::min(a.east, b.east) > box.east
        || std::max(a.south, b.south) < box.north || std::min(a.south, b.south) > box.south)
        return false;
    const std::array corners{side(a, b, {box.west, box.north}),
        side(a, b, {box.east, box.north}), side(a, b, {box.west, box.south}),
        side(a, b, {box.east, box.south})};
    const auto [low, high] = std::minmax_element(corners.begin(), corners.end());
    return *low <= 0 && *high >= 0;
}

bool intersects(Point a, Point b, Waterway waterway)
{
    const auto c = waterway.from;
    const auto d = waterway.to;
    if (std::max(a.east, b.east) < std::min(c.east, d.east) ||
        std::max(c.east, d.east) < std::min(a.east, b.east) ||
        std::max(a.south, b.south) < std::min(c.south, d.south) ||
        std::max(c.south, d.south) < std::min(a.south, b.south))
        return false;
    const auto straddles = [](Wide first, Wide last) {
        return (first <= 0 && last >= 0) || (first >= 0 && last <= 0);
    };
    return straddles(side(a, b, c), side(a, b, d)) &&
           straddles(side(c, d, a), side(c, d, b));
}

const Rate& terrain_rate(const Profile& profile, Cover cover)
{
    return profile.terrain.at(static_cast<std::size_t>(cover == Cover::urban ? Cover::plain : cover));
}

std::optional<Leg> priced(Point a, Point b, Rate rate, std::int64_t cap,
    std::optional<std::uint32_t> road = {})
{
    if (rate.speed_mm_s <= 0)
        return {};
    const auto length = distance(a, b);
    const auto rise = b.elevation - a.elevation;
    if (length == 0 && rise != 0)
        return {};
    if (Wide(rise) * 1000 > Wide(length) * rate.max_uphill_permille
        || -Wide(rise) * 1000 > Wide(length) * rate.max_downhill_permille)
        return {};
    auto speed = std::min(cap, rate.speed_mm_s);
    if (rise > 0 && rate.uphill_speed_mm_s > 0)
        speed = std::min(speed, rate.uphill_speed_mm_s);
    return Leg{a, b, length, checked((Wide(length) * 1'000'000 + speed - 1) / speed),
        checked((Wide(length) * rate.fuel_ml_km + 999'999) / 1'000'000), road};
}

void record_crossings(const Map& map, const Leg& leg, Progress& progress, std::int64_t until);

struct Vertex {
    Point point;
    std::vector<std::pair<std::size_t, Leg>> links;
};

Rectangle sector_bounds(Point a, Point b, std::int64_t size)
{
    return {std::max<std::int64_t>(0, std::min(a.east, b.east) - 1) / size,
        std::max<std::int64_t>(0, std::min(a.south, b.south) - 1) / size,
        std::max(a.east, b.east) / size, std::max(a.south, b.south) / size};
}

struct WaterwayIndex {
    explicit WaterwayIndex(const Map& map) : size(map.sector_mm)
    {
        for (const auto& line : map.waterways) {
            const auto box = sector_bounds(line.from, line.to, size);
            if (Wide(box.east - box.west + 1) * (box.south - box.north + 1) > 64) {
                long_lines.push_back(&line);
                continue;
            }
            for (auto x = box.west; x <= box.east; ++x)
                for (auto y = box.north; y <= box.south; ++y)
                    bins[{x, y}].push_back(&line);
        }
    }

    bool blocked(Point a, Point b) const
    {
        const auto hits = [&](const Waterway* line) { return intersects(a, b, *line); };
        if (std::ranges::any_of(long_lines, hits))
            return true;
        if (bins.empty())
            return false;
        const auto box = sector_bounds(a, b, size);
        for (auto it = bins.lower_bound({box.west, 0});
             it != bins.end() && it->first.first <= box.east; ++it) {
            const auto row = it->first.second;
            if (row >= box.north && row <= box.south && std::ranges::any_of(it->second, hits))
                return true;
        }
        return false;
    }

    std::int64_t size;
    std::map<std::pair<std::int64_t, std::int64_t>, std::vector<const Waterway*>> bins;
    std::vector<const Waterway*> long_lines;
};

struct Search {
    const Map& map;
    const Profile& profile;
    QueryLimits limits;
    Route route;
    const SectorAccess& allowed;
    const RoadAccess& road_allowed;
    const WaterwayIndex& waterways;
    std::int64_t left, top, width, height, tile_columns, tile_rows;
    std::int64_t spacing, offset_east, offset_south;
    std::vector<Vertex> vertices;
    std::size_t terrain_checks{};
    bool coarse = false;
    bool exhausted = false;

    const Tile& tile(std::int64_t x, std::int64_t y) const
    {
        const auto found = map.tiles.find({x, y});
        return found == map.tiles.end() ? map.default_tile : found->second;
    }

    bool blocked(Point a, Point b) const
    {
        return std::any_of(map.barriers.begin(), map.barriers.end(),
            [a, b](Rectangle barrier) { return intersects(a, b, barrier); }) || waterways.blocked(a, b);
    }

    // Null means the road tile validation budget was exhausted.
    std::optional<bool> crosses_water(Point a, Point b)
    {
        const auto size = map.tile_mm;
        const auto west = std::max<std::int64_t>(0, (std::min(a.east, b.east) - 1) / size);
        const auto north = std::max<std::int64_t>(0, (std::min(a.south, b.south) - 1) / size);
        const auto east = std::min(tile_columns - 1, std::max(a.east, b.east) / size);
        const auto south = std::min(tile_rows - 1, std::max(a.south, b.south) / size);
        const auto touches = [&](std::int64_t x, std::int64_t y) {
            return intersects(a, b, Rectangle{x * size, y * size, (x + 1) * size, (y + 1) * size});
        };

        // A sparse set of overrides is cheaper to inspect than the raster line.
        if (map.default_tile.cover != Cover::water &&
            map.tiles.size() <= static_cast<std::uint64_t>(east - west + south - north + 2) * 4) {
            for (auto entry = map.tiles.lower_bound({west, 0});
                 entry != map.tiles.end() && entry->first.first <= east; ++entry) {
                if (route.road_tile_checks == limits.max_road_tile_checks)
                    return {};
                ++route.road_tile_checks;
                const auto [x, y] = entry->first;
                if (y >= north && y <= south && entry->second.cover == Cover::water && touches(x, y))
                    return true;
            }
            return false;
        }

        // Visit only the rows near the line within each tile column.
        if (a.east > b.east)
            std::swap(a, b);
        const auto south_at = [&](std::int64_t x) {
            return a.east == b.east ? a.south : interpolate(a, b,
                std::clamp(x, a.east, b.east) - a.east, b.east - a.east).south;
        };
        for (auto x = west; x <= east; ++x) {
            const auto first = south_at(x * size);
            const auto last = a.east == b.east ? b.south : south_at((x + 1) * size);
            // Include neighbouring rows at rounded boundaries, then test exact geometry.
            const auto begin = std::max(north, (std::min(first, last) - 1) / size);
            const auto end = std::min(south, (std::max(first, last) + 1) / size);
            for (auto y = begin; y <= end; ++y) {
                if (route.road_tile_checks == limits.max_road_tile_checks)
                    return {};
                ++route.road_tile_checks;
                if (tile(x, y).cover == Cover::water && touches(x, y))
                    return true;
            }
        }
        return false;
    }

    Point terrain_point(std::int64_t east, std::int64_t south) const
    {
        const auto elevation = tile(east / map.tile_mm, south / map.tile_mm).elevation;
        if (!valid_elevation(elevation))
            throw std::invalid_argument("terrain elevation outside supported range");
        return {east, south, elevation};
    }

    std::optional<Leg> terrain_step(Point a, Point b)
    {
        if (blocked(a, b))
            return {};
        const auto size = map.tile_mm;
        const auto west = std::max<std::int64_t>(0, (std::min(a.east, b.east) - 1) / size);
        const auto north = std::max<std::int64_t>(0, (std::min(a.south, b.south) - 1) / size);
        const auto east = std::min(tile_columns - 1, std::max(a.east, b.east) / size);
        const auto south = std::min(tile_rows - 1, std::max(a.south, b.south) / size);
        Rate combined{maximum, 0, 0, maximum, maximum};
        const bool uphill = b.elevation > a.elevation;
        for (auto y = north; y <= south; ++y) {
            for (auto x = west; x <= east; ++x) {
                if (terrain_checks == limits.max_terrain_checks) {
                    exhausted = true;
                    return {};
                }
                ++terrain_checks;
                if (!intersects(a, b, Rectangle{x * size, y * size, (x + 1) * size, (y + 1) * size}))
                    continue;
                const auto& rate = terrain_rate(profile, tile(x, y).cover);
                const auto speed = uphill && rate.uphill_speed_mm_s > 0
                    ? std::min(rate.speed_mm_s, rate.uphill_speed_mm_s) : rate.speed_mm_s;
                combined.speed_mm_s = std::min(combined.speed_mm_s, speed);
                combined.fuel_ml_km = std::max(combined.fuel_ml_km, rate.fuel_ml_km);
                combined.max_uphill_permille = std::min(combined.max_uphill_permille, rate.max_uphill_permille);
                combined.max_downhill_permille = std::min(combined.max_downhill_permille, rate.max_downhill_permille);
            }
        }
        return priced(a, b, combined, maximum);
    }

    // Price a sampled edge as a sequence of fine terrain steps. Rebuild that
    // sequence only for selected edges, keeping the search graph small.
    std::optional<Leg> offroad(Point a, Point b, std::vector<Leg>* detail = nullptr)
    {
        if (std::ranges::none_of(profile.terrain, [](const auto& rate) { return rate.speed_mm_s > 0; }))
            return {};
        if (coarse && map.tiles.empty() && a.elevation == map.default_tile.elevation &&
            b.elevation == map.default_tile.elevation) {
            if (terrain_checks == limits.max_terrain_checks) {
                exhausted = true;
                return {};
            }
            ++terrain_checks;
            if (blocked(a, b))
                return {};
            auto leg = priced(a, b, terrain_rate(profile, map.default_tile.cover), maximum);
            if (detail && leg)
                detail->push_back(*leg);
            return leg;
        }

        const auto span = std::max(std::abs(a.east - b.east), std::abs(a.south - b.south));
        const auto steps = coarse ? std::max<std::int64_t>(1, (span + map.tile_mm - 1) / map.tile_mm) : 1;
        if (steps > 1 && blocked(a, b))
            return {};
        Leg sum{a, b, 0, 0, 0, {}};
        auto from = a;
        for (auto i = std::int64_t{1}; i <= steps; ++i) {
            auto to = b;
            if (i != steps) {
                const auto point = interpolate(a, b, i, steps);
                to = terrain_point(point.east, point.south);
            }
            const auto leg = terrain_step(from, to);
            if (!leg || (coarse && allowed && !allows_leg(map, *leg, allowed)))
                return {};
            sum.distance_mm = checked(Wide(sum.distance_mm) + leg->distance_mm);
            sum.duration_us = checked(Wide(sum.duration_us) + leg->duration_us);
            sum.fuel_ml = checked(Wide(sum.fuel_ml) + leg->fuel_ml);
            if (detail)
                detail->push_back(*leg);
            from = to;
        }
        return sum;
    }

    std::size_t index(std::int64_t x, std::int64_t y) const
    {
        return 2 + static_cast<std::size_t>(y * width + x);
    }

    std::size_t add(Point point)
    {
        vertices.push_back({point, {}});
        return vertices.size() - 1;
    }

    void connect(std::size_t from, std::size_t to, std::optional<Leg> leg)
    {
        if (leg)
            vertices[from].links.emplace_back(to, *leg);
    }

    void connector(std::size_t a, std::size_t b)
    {
        const auto from = vertices[a].point;
        const auto to = vertices[b].point;
        // Equal deck and query points permit road-only trips. Raster transfer
        // still has to pass the terrain test, even at a zero-length junction.
        if (a < 2 && from == to) {
            const Leg transfer{from, to, 0, 0, 0, {}};
            connect(a, b, transfer);
            connect(b, a, transfer);
            return;
        }
        connect(a, b, offroad(from, to));
        connect(b, a, offroad(to, from));
    }

    bool add_roads();
    void solve(RoutePriority priority);
};

bool Search::add_roads()
{
    std::map<std::uint32_t, std::size_t> nodes;
    for (const auto& node : map.nodes) {
        if (!map.valid_position(node.position)
            || !nodes.emplace(node.id, add(node.position)).second)
            throw std::invalid_argument("invalid road node");
    }
    std::map<std::uint32_t, bool> segments;
    const auto size = spacing;
    for (const auto& road : map.roads) {
        if (!segments.emplace(road.id, true).second || road.speed_limit_mm_s <= 0)
            throw std::invalid_argument("invalid road segment");
        const auto from = nodes.at(road.from);
        const auto to = nodes.at(road.to);
        const auto rate = profile.roads.at(static_cast<std::size_t>(road.surface));
        if ((road_allowed && !road_allowed(road.id)) || rate.speed_mm_s <= 0)
            continue;
        std::vector<std::size_t> shape{from};
        for (const auto point : road.waypoints) {
            if (!map.valid_position(point))
                throw std::invalid_argument("road waypoint outside map");
            shape.push_back(add(point));
        }
        shape.push_back(to);

        for (std::size_t section = 1; section < shape.size(); ++section) {
            const auto start = shape[section - 1];
            const auto end = shape[section];
            const auto a = vertices[start].point;
            const auto b = vertices[end].point;
            if (a.east == b.east && a.south == b.south)
                throw std::invalid_argument("zero-length road section");
            if (road.crossing == Crossing::ground) {
                if (blocked(a, b))
                    continue;
                const auto water = crosses_water(a, b);
                if (!water)
                    return false;
                if (*water)
                    continue;
            }

            std::map<Point, std::size_t> offsets{{a, start}, {b, end}};
            std::vector<Point> fixed_access;
            if (section == 1)
                fixed_access.push_back(a);
            if (section + 1 == shape.size())
                fixed_access.push_back(b);
            if (road.interior_access) {
                for (const auto point : road.interior_access_points) {
                    if (route.projection_checks == limits.max_projection_checks)
                        return false;
                    ++route.projection_checks;
                    if (map.valid_position(point)
                        && project(a, b, point) == point)
                        fixed_access.push_back(point);
                }
            }
            const bool free_access = road.interior_access && road.interior_access_points.empty();
            const auto attach = [&](std::size_t vertex, Point projected) {
                if (distance(vertices[vertex].point, projected) > size)
                    return;
                // A formation already standing on the deck can resume there.
                const bool on_deck = vertex < 2 && vertices[vertex].point == projected;
                if (!on_deck && blocked(projected, projected))
                    return;
                auto [entry, inserted] = offsets.try_emplace(projected, 0);
                if (inserted)
                    entry->second = add(projected);
                connector(vertex, entry->second);
            };
            const auto access = [&](std::size_t vertex) {
                const auto checks = free_access ? 1 : fixed_access.size() + (vertex < 2);
                if (checks > limits.max_projection_checks - route.projection_checks)
                    return false;
                route.projection_checks += checks;
                if (free_access || vertex < 2) {
                    const auto point = vertices[vertex].point;
                    const auto projected = project(a, b, point);
                    if (free_access || point == projected) {
                        attach(vertex, projected);
                        return true;
                    }
                }
                for (const auto point : fixed_access)
                    attach(vertex, point);
                return true;
            };

            for (std::size_t endpoint = 0; endpoint < 2; ++endpoint) {
                const auto point = vertices[endpoint].point;
                if (point.east >= std::min(a.east, b.east) - size &&
                    point.east <= std::max(a.east, b.east) + size &&
                    point.south >= std::min(a.south, b.south) - size &&
                    point.south <= std::max(a.south, b.south) + size && !access(endpoint))
                    return false;
            }
            const auto west = std::max(left, (std::min(a.east, b.east) - size - offset_east) / size);
            const auto north = std::max(top, (std::min(a.south, b.south) - size - offset_south) / size);
            const auto east = std::min(left + width - 1, (std::max(a.east, b.east) + size - offset_east) / size);
            const auto south = std::min(top + height - 1, (std::max(a.south, b.south) + size - offset_south) / size);
            for (auto y = north; y <= south; ++y) {
                for (auto x = west; x <= east; ++x) {
                    if (exhausted || !access(index(x - left, y - top)))
                        return false;
                }
            }

            // Lexicographic point order follows a straight section in either direction.
            std::vector<std::size_t> ordered;
            for (const auto& [point, vertex] : offsets)
                ordered.push_back(vertex);
            if (a > b)
                std::reverse(ordered.begin(), ordered.end());
            const auto road_cost = [&](Point first, Point last) {
                return priced(first, last, rate, road.speed_limit_mm_s, road.id);
            };
            // Test grade against the original section before millimetre rounding
            // at access points can affect the grade test.
            const bool forward = road.direction != Direction::reverse && road_cost(a, b).has_value();
            const bool reverse = road.direction != Direction::forward && road_cost(b, a).has_value();
            Rate split_rate = rate;
            split_rate.max_uphill_permille = maximum;
            split_rate.max_downhill_permille = maximum;
            split_rate.uphill_speed_mm_s = 0;
            const auto speed_cap = [&](Point first, Point last) {
                if (last.elevation > first.elevation && rate.uphill_speed_mm_s > 0)
                    return std::min(road.speed_limit_mm_s, rate.uphill_speed_mm_s);
                return road.speed_limit_mm_s;
            };
            const auto forward_cap = speed_cap(a, b);
            const auto reverse_cap = speed_cap(b, a);
            for (std::size_t i = 1; i < ordered.size(); ++i) {
                const auto first = ordered[i - 1];
                const auto last = ordered[i];
                const auto first_point = vertices[first].point;
                const auto last_point = vertices[last].point;
                if (forward)
                    connect(first, last, priced(first_point, last_point, split_rate,
                        forward_cap, road.id));
                if (reverse)
                    connect(last, first, priced(last_point, first_point, split_rate,
                        reverse_cap, road.id));
            }
        }
    }
    return !exhausted;
}

void Search::solve(RoutePriority priority)
{
    using Cost = std::pair<std::int64_t, std::int64_t>;
    using Entry = std::tuple<Cost, Cost, Cost, std::size_t>;
    const auto order = [priority](std::int64_t time, std::int64_t fuel) {
        return priority == RoutePriority::time ? Cost{time, fuel} : Cost{fuel, time};
    };
    std::int64_t fastest = 1;
    std::int64_t lowest_fuel = maximum;
    const auto include = [&](const Rate& rate) {
        if (rate.speed_mm_s > 0) {
            fastest = std::max(fastest, rate.speed_mm_s);
            lowest_fuel = std::min(lowest_fuel, rate.fuel_ml_km);
        }
    };
    for (const auto& rate : profile.terrain)
        include(rate);
    for (const auto& road : map.roads)
        if (!road_allowed || road_allowed(road.id))
            include(profile.roads.at(static_cast<std::size_t>(road.surface)));
    const auto heuristic = [&](std::size_t vertex) {
        if (!coarse && width * height < 4096)
            return Cost{};
        const auto length = Wide(distance(vertices[vertex].point, vertices[1].point));
        // A straight line at the fastest and cheapest profile rates bounds every route.
        const auto fuel = lowest_fuel == maximum ? Wide(0) : length * lowest_fuel / 1'000'000;
        return order(static_cast<std::int64_t>(std::min<Wide>(maximum, length * 1'000'000 / fastest)),
            static_cast<std::int64_t>(std::min<Wide>(maximum, fuel)));
    };
    struct Previous {
        std::size_t vertex{};
        Leg leg{};
    };
    std::vector<std::optional<Cost>> costs(vertices.size());
    std::vector<Previous> previous(vertices.size());
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    costs[0] = Cost{};
    const auto enqueue = [&](Cost cost, std::size_t vertex) {
        const auto remaining = heuristic(vertex);
        const Cost estimate{static_cast<std::int64_t>(std::min<Wide>(maximum, Wide(cost.first) + remaining.first)),
            static_cast<std::int64_t>(std::min<Wide>(maximum, Wide(cost.second) + remaining.second))};
        queue.emplace(estimate, remaining, cost, vertex);
    };
    enqueue({}, 0);
    while (!queue.empty() && !exhausted) {
        const auto [estimate, remaining, cost, current] = queue.top();
        queue.pop();
        if (costs[current] != cost)
            continue;
        if (current == 1) {
            std::vector<Leg> selected;
            for (std::size_t vertex = current; vertex != 0; vertex = previous[vertex].vertex) {
                const auto& leg = previous[vertex].leg;
                if (leg.duration_us != 0)
                    selected.push_back(leg);
            }
            std::reverse(selected.begin(), selected.end());
            for (const auto& leg : selected) {
                if (coarse && !leg.road) {
                    if (!offroad(leg.from, leg.to, &route.legs)) {
                        route.legs.clear();
                        return;
                    }
                } else {
                    route.legs.push_back(leg);
                }
                route.duration_us = checked(Wide(route.duration_us) + leg.duration_us);
                route.fuel_ml = checked(Wide(route.fuel_ml) + leg.fuel_ml);
            }
            route.status = RouteStatus::found;
            return;
        }
        const auto relax = [&](std::size_t next, const Leg& leg) {
            if (allowed && !allows_leg(map, leg, allowed))
                return;
            const auto increment = order(leg.duration_us, leg.fuel_ml);
            const Cost candidate{checked(Wide(cost.first) + increment.first),
                checked(Wide(cost.second) + increment.second)};
            if (!costs[next] || candidate < *costs[next]) {
                costs[next] = candidate;
                previous[next] = {current, leg};
                enqueue(candidate, next);
            }
        };

        for (const auto& [next, leg] : vertices[current].links)
            relax(next, leg);

        const auto raster_end = index(0, height);
        if (current >= 2 && current < raster_end) {
            ++route.raster_expanded;
            const auto x = static_cast<std::int64_t>(current - 2) % width;
            const auto y = static_cast<std::int64_t>(current - 2) / width;
            for (auto ny = std::max<std::int64_t>(0, y - 1); ny < height && ny <= y + 1; ++ny) {
                for (auto nx = std::max<std::int64_t>(0, x - 1); nx < width && nx <= x + 1; ++nx) {
                    const auto next = index(nx, ny);
                    if (next != current) {
                        if (const auto leg = offroad(vertices[current].point, vertices[next].point))
                            relax(next, *leg);
                    }
                }
            }
        }
    }
}

void record_crossings(const Map& map, const Leg& leg, Progress& progress, std::int64_t until)
{
    auto previous_time = progress.leg_elapsed_us;
    auto previous_sector = map.sector(progress.position);
    const auto final_sector = map.sector(interpolate(leg.from, leg.to, until, leg.duration_us));
    while (previous_sector != final_sector) {
        // A straight section cannot reenter a sector it has left.
        auto first = previous_time + 1;
        auto last = until;
        while (first < last) {
            const auto mid = first + (last - first) / 2;
            const auto point = interpolate(leg.from, leg.to, mid, leg.duration_us);
            if (map.sector(point) == previous_sector)
                first = mid + 1;
            else
                last = mid;
        }
        const auto point = position_at(leg, first);
        const auto next = map.sector(point);
        progress.crossings.push_back({*previous_sector, *next,
            progress.elapsed_us + (first - progress.leg_elapsed_us), point});
        previous_time = first;
        previous_sector = next;
    }
}

} // namespace

std::optional<std::int64_t> Map::sector(Point point) const
{
    const auto east_limit = extent(columns, sector_mm);
    const auto south_limit = extent(rows, sector_mm);
    if (point.east < 0 || point.east >= east_limit || point.south < 0
        || point.south >= south_limit)
        return {};
    return checked(Wide(point.south / sector_mm) * columns + point.east / sector_mm);
}

bool Map::valid_position(Point point) const
{
    return sector(point).has_value() && valid_elevation(point.elevation);
}

std::vector<const SiteFeature*> Map::site_candidates(SiteKind kind) const
{
    std::vector<const SiteFeature*> candidates;
    for (const auto& feature : site_features)
        if (feature.kind == kind)
            candidates.push_back(&feature);
    return candidates;
}

Route find_route(const Map& map, const Profile& profile, Point origin, Point destination,
    QueryLimits limits, RoutePriority priority, const SectorAccess& allowed, const RoadAccess& road_allowed)
{
    if (map.tile_mm <= 0 || map.tile_mm > coordinate_limit || limits.margin_tiles < 0
        || limits.margin_tiles > coordinate_limit)
        throw std::invalid_argument("invalid terrain grid or query limits");
    const auto size = map.tile_mm;
    const auto columns = (extent(map.columns, map.sector_mm) + size - 1) / size;
    const auto rows = (extent(map.rows, map.sector_mm) + size - 1) / size;
    Route route{};
    route.origin = origin;
    if (!map.valid_position(origin) || !map.valid_position(destination))
        return route;
    if (origin == destination) {
        route.status = RouteStatus::found;
        return route;
    }
    const auto valid_rate = [](Rate rate) {
        return rate.speed_mm_s >= 0 && rate.fuel_ml_km >= 0 && rate.uphill_speed_mm_s >= 0
            && rate.max_uphill_permille >= 0 && rate.max_downhill_permille >= 0;
    };
    if (!std::all_of(profile.terrain.begin(), profile.terrain.end(), valid_rate)
        || !std::all_of(profile.roads.begin(), profile.roads.end(), valid_rate))
        throw std::invalid_argument("invalid movement rate");

    const auto fine_left = std::max<std::int64_t>(0,
        std::min(origin.east, destination.east) / size - limits.margin_tiles);
    const auto fine_top = std::max<std::int64_t>(0,
        std::min(origin.south, destination.south) / size - limits.margin_tiles);
    const auto fine_width = std::min(columns - 1, std::max(origin.east, destination.east) / size +
                                    limits.margin_tiles) - fine_left + 1;
    const auto fine_height = std::min(rows - 1, std::max(origin.south, destination.south) / size +
                                     limits.margin_tiles) - fine_top + 1;
    const bool can_leave_road = std::ranges::any_of(profile.terrain, [](const auto& rate) {
        return rate.speed_mm_s > 0;
    });
    const WaterwayIndex waterways(map);

    const auto search_grid = [&](bool coarse, QueryLimits allowance) {
        const auto budget = std::min<Wide>(allowance.max_raster_vertices, maximum - 2);
        std::int64_t left = coarse ? 0 : fine_left;
        std::int64_t top = coarse ? 0 : fine_top;
        std::int64_t width = coarse ? 0 : fine_width;
        std::int64_t height = coarse ? 0 : fine_height;
        auto spacing = size;
        std::int64_t offset_east = 0, offset_south = 0;
        if (coarse && can_leave_road && budget > 0) {
            // Sample at roughly sector resolution and coarsen further only if needed.
            auto low = std::clamp(map.sector_mm / size, std::int64_t{1}, std::max(columns, rows));
            auto high = std::max(columns, rows);
            while (low < high) {
                const auto stride = low + (high - low) / 2;
                if (Wide((columns - 1) / stride + 1) * ((rows - 1) / stride + 1) <= budget)
                    high = stride;
                else
                    low = stride + 1;
            }
            spacing = low * size;
            const auto east_offset_tiles = std::min(low / 2, columns - 1);
            const auto south_offset_tiles = std::min(low / 2, rows - 1);
            offset_east = east_offset_tiles * size;
            offset_south = south_offset_tiles * size;
            width = (columns - 1 - east_offset_tiles) / low + 1;
            height = (rows - 1 - south_offset_tiles) / low + 1;
        }

        Route result{};
        result.origin = origin;
        Search search{map, profile, allowance, std::move(result), allowed, road_allowed, waterways, left, top,
            width, height, columns, rows, spacing, offset_east, offset_south, {}, 0, coarse};
        search.add(origin);
        search.add(destination);
        for (auto y = top; y < top + height; ++y)
            for (auto x = left; x < left + width; ++x)
                search.add(search.terrain_point(x * spacing + offset_east, y * spacing + offset_south));

        for (std::size_t vertex = 0; vertex < 2; ++vertex) {
            const auto point = search.vertices[vertex].point;
            const auto x = (point.east - offset_east) / spacing - left;
            const auto y = (point.south - offset_south) / spacing - top;
            for (auto ny = std::max<std::int64_t>(0, y - 1); ny < height && ny <= y + 1; ++ny)
                for (auto nx = std::max<std::int64_t>(0, x - 1); nx < width && nx <= x + 1; ++nx) {
                    const auto next = search.index(nx, ny);
                    search.connect(vertex, next, search.offroad(point, search.vertices[next].point));
                    search.connect(next, vertex, search.offroad(search.vertices[next].point, point));
                }
        }
        // A clear straight corridor may fit between regional sample points.
        if (coarse || (std::abs(origin.east - destination.east) <= size &&
                       std::abs(origin.south - destination.south) <= size))
            search.connect(0, 1, search.offroad(origin, destination));
        if (search.add_roads())
            search.solve(priority);
        else
            search.exhausted = true;

        if (search.exhausted) {
            search.route.status = RouteStatus::limit_exceeded;
            search.route.duration_us = search.route.fuel_ml = 0;
        }
        return std::pair{std::move(search.route), search.terrain_checks};
    };

    // A route proved impossible on a complete fine graph needs no retry.
    // A clipped search can retry with the remaining budget on the sparse graph.
    if (Wide(fine_width) * fine_height <= std::min<Wide>(limits.max_raster_vertices, maximum - 2)) {
        auto [fine, checked_tiles] = search_grid(false, limits);
        const bool complete = fine_left == 0 && fine_top == 0 && fine_width == columns && fine_height == rows;
        if (fine.status != RouteStatus::unreachable || complete)
            return fine;
        if (!can_leave_road)
            return fine;

        auto allowance = limits;
        allowance.max_raster_vertices -= std::min(limits.max_raster_vertices, fine.raster_expanded);
        allowance.max_projection_checks -= fine.projection_checks;
        allowance.max_road_tile_checks -= fine.road_tile_checks;
        allowance.max_terrain_checks -= checked_tiles;
        auto [regional, regional_tiles] = search_grid(true, allowance);
        regional.raster_expanded += fine.raster_expanded;
        regional.projection_checks += fine.projection_checks;
        regional.road_tile_checks += fine.road_tile_checks;
        if (regional.status == RouteStatus::unreachable)
            regional.status = RouteStatus::limit_exceeded;
        return regional;
    }

    auto [regional, checked_tiles] = search_grid(true, limits);
    if (regional.status == RouteStatus::unreachable && can_leave_road)
        regional.status = RouteStatus::limit_exceeded;
    return regional;
}

Progress begin(const Route& route, std::int64_t fuel_ml)
{
    if (route.status != RouteStatus::found || fuel_ml < 0)
        throw std::invalid_argument("invalid journey or starting fuel");
    return {route.origin, 0, 0, 0, fuel_ml, false, {}};
}

Point position_at(const Leg& leg, std::int64_t elapsed_us)
{
    return interpolate(leg.from, leg.to, elapsed_us, leg.duration_us);
}

std::int64_t horizontal_distance(Point a, Point b)
{
    return distance(a, b);
}

bool allows_leg(const Map& map, const Leg& leg, const SectorAccess& allowed, std::int64_t elapsed_us)
{
    const auto point = leg.duration_us ? position_at(leg, elapsed_us) : leg.from;
    const auto first = *map.sector(point);
    const auto last = *map.sector(leg.to);
    if (!allowed(first) || !allowed(last))
        return false;
    if (first == last)
        return true;
    Progress trace{};
    trace.position = point;
    trace.leg_elapsed_us = elapsed_us;
    record_crossings(map, leg, trace, leg.duration_us);
    return std::ranges::all_of(trace.crossings, [&](const auto& crossing) {
        return allowed(crossing.to);
    });
}

std::int64_t advance(const Map& map, const Route& route, Progress& progress, std::int64_t duration_us,
    const CrossingObserver& observer, const RoadAccess& road_allowed)
{
    if (duration_us < 0)
        throw std::invalid_argument("negative movement time");
    while (duration_us > 0 && progress.leg < route.legs.size()) {
        const auto& leg = route.legs[progress.leg];
        if (!progress.entry_paid) {
            if (leg.fuel_ml > progress.fuel_ml
                || (leg.road && road_allowed && !road_allowed(*leg.road)))
                break;
            progress.fuel_ml -= leg.fuel_ml;
            progress.entry_paid = true;
        }
        auto spent = std::min(duration_us, leg.duration_us - progress.leg_elapsed_us);
        const auto first_crossing = progress.crossings.size();
        record_crossings(map, leg, progress, progress.leg_elapsed_us + spent);
        bool stopped = false;
        for (auto i = first_crossing; observer && i < progress.crossings.size(); ++i) {
            const auto& crossing = progress.crossings[i];
            if (!observer(crossing)) {
                spent = crossing.elapsed_us - progress.elapsed_us;
                progress.crossings.resize(i + 1);
                stopped = true;
                break;
            }
        }
        const auto until = progress.leg_elapsed_us + spent;
        progress.position = position_at(leg, until);
        progress.elapsed_us += spent;
        duration_us -= spent;
        progress.leg_elapsed_us = until;
        if (until == leg.duration_us) {
            ++progress.leg;
            progress.leg_elapsed_us = 0;
            progress.entry_paid = false;
        }
        if (stopped)
            break;
    }
    return duration_us;
}

} // namespace openwar::geography
