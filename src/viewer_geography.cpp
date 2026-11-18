#include "viewer_geography.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace openwar::viewer {
namespace {

constexpr Color water_fill{60, 112, 140, 255};
constexpr Color river_ink{97, 170, 198, 255};
constexpr Color paved_ink{220, 205, 164, 255};
constexpr Color gravel_ink{172, 151, 120, 255};
constexpr Color bridge_ink{255, 219, 137, 255};
constexpr Color closed_ink{237, 135, 126, 255};
constexpr Color site_ink{229, 207, 172, 255};

Rectangle line_bounds(Vector2 a, Vector2 b)
{
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)};
}

bool overlaps(Rectangle a, Rectangle b)
{
    // Centre lines can have zero width or height.
    return a.x <= b.x + b.width && a.x + a.width >= b.x &&
           a.y <= b.y + b.height && a.y + a.height >= b.y;
}

Rectangle view_bounds(Camera2D camera, Rectangle viewport)
{
    constexpr float margin = 8;
    const auto a = GetScreenToWorld2D({viewport.x - margin, viewport.y - margin}, camera);
    const auto b = GetScreenToWorld2D({viewport.x + viewport.width + margin,
                                       viewport.y + viewport.height + margin}, camera);
    return line_bounds(a, b);
}

Vector2 project(Vector2 point, Camera2D camera)
{
    // The strategic camera has no rotation. Share raylib's scale and translation.
    return {camera.offset.x + (point.x - camera.target.x) * camera.zoom,
            camera.offset.y + (point.y - camera.target.y) * camera.zoom};
}

float nearest_fraction(Vector2 point, Vector2 a, Vector2 b)
{
    const auto dx = b.x - a.x;
    const auto dy = b.y - a.y;
    const auto length_squared = dx * dx + dy * dy;
    if (length_squared == 0) {
        return 0;
    }
    return std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / length_squared, 0.f, 1.f);
}

float distance_squared(Vector2 point, Vector2 a, Vector2 b, float fraction)
{
    const auto dx = point.x - std::lerp(a.x, b.x, fraction);
    const auto dy = point.y - std::lerp(a.y, b.y, fraction);
    return dx * dx + dy * dy;
}

geography::Point interpolate(geography::Point a, geography::Point b, float fraction)
{
    const auto mix = [fraction](std::int64_t start, std::int64_t end) {
        return start + std::llround(static_cast<double>(end - start) * fraction);
    };
    return {mix(a.east, b.east), mix(a.south, b.south), mix(a.elevation, b.elevation)};
}

std::string_view site_kind(geography::SiteKind kind)
{
    constexpr std::array names{"Aerodrome", "Runway", "Helipad", "Port", "Rail station", "Road junction",
                               "Open land", "Industrial land", "Urban land"};
    return names.at(static_cast<std::size_t>(kind));
}

bool show_site(geography::SiteKind kind, float zoom)
{
    // Transport landmarks remain at overview. The denser catalog appears below 25 m/px.
    return kind <= geography::SiteKind::rail_station || zoom >= 0.04f;
}

std::string metres(std::uint64_t mm)
{
    return std::to_string(mm / 1000) + " m";
}

}  // namespace

Vector2 map_position(geography::Point point)
{
    return {static_cast<float>(point.east / 1000.0), static_cast<float>(point.south / 1000.0)};
}

GeographicLayers::GeographicLayers(const geography::Map* map, const WorldDefinition& world) : map_(map)
{
    extent_ = {0, 0, static_cast<float>(world.width()) * world.sector_size_m(),
                    static_cast<float>(world.height()) * world.sector_size_m()};
    if (!map_) {
        build_terrain(world);
        return;
    }
    for (const auto& node : map_->nodes) {
        nodes_.emplace(node.id, node.position);
    }
    roads_.reserve(map_->roads.size());
    for (const auto& road : map_->roads) {
        Line line;
        line.points.reserve(road.waypoints.size() + 2);
        line.points.push_back(map_position(nodes_.at(road.from)));
        for (const auto& point : road.waypoints) {
            line.points.push_back(map_position(point));
        }
        line.points.push_back(map_position(nodes_.at(road.to)));
        Vector2 low = line.points.front();
        auto high = low;
        for (const auto& point : line.points) {
            low = {std::min(low.x, point.x), std::min(low.y, point.y)};
            high = {std::max(high.x, point.x), std::max(high.y, point.y)};
        }
        line.bounds = line_bounds(low, high);
        roads_.push_back(std::move(line));
    }
    road_draw_order_.resize(roads_.size());
    std::iota(road_draw_order_.begin(), road_draw_order_.end(), std::size_t{0});
    // Keep thin lines together in raylib's batch and draw bridges over the road network.
    std::ranges::stable_sort(road_draw_order_, {}, [&](std::size_t i) { return map_->roads[i].crossing; });
    waterway_bounds_.reserve(map_->waterways.size());
    for (const auto& waterway : map_->waterways) {
        waterway_bounds_.push_back(line_bounds(map_position(waterway.from), map_position(waterway.to)));
    }
    build_terrain(world);
    build_overview();
}

GeographicLayers::~GeographicLayers()
{
    if (road_overview_.id) {
        UnloadRenderTexture(road_overview_);
    }
    if (waterway_overview_.id) {
        UnloadRenderTexture(waterway_overview_);
    }
    if (water_.id) {
        UnloadTexture(water_);
    }
    if (terrain_.id) {
        UnloadTexture(terrain_);
    }
}

void GeographicLayers::build_overview()
{
    // Dense networks use a bounded overview texture. Closer views use the original vectors.
    if (roads_.size() + waterway_bounds_.size() < 1000) {
        return;
    }
    const auto scale = std::min(0.06f, 1536.f / std::max(extent_.width, extent_.height));
    const auto width = std::max(1, static_cast<int>(std::ceil(extent_.width * scale)));
    const auto height = std::max(1, static_cast<int>(std::ceil(extent_.height * scale)));
    overview_zoom_ = 0.95f * scale;
    const Camera2D camera{{}, {}, 0, scale};
    const auto bake = [&](bool roads) {
        auto layer = LoadRenderTexture(width, height);
        if (layer.id) {
            BeginTextureMode(layer);
            ClearBackground(BLANK);
            draw_network(camera, extent_, !roads, roads, false, {}, NetworkMode::bake_overview);
            EndTextureMode();
            GenTextureMipmaps(&layer.texture);
            SetTextureFilter(layer.texture, layer.texture.mipmaps > 1 ? TEXTURE_FILTER_TRILINEAR : TEXTURE_FILTER_BILINEAR);
        }
        return layer;
    };
    if (!roads_.empty()) {
        road_overview_ = bake(true);
    }
    if (!waterway_bounds_.empty()) {
        waterway_overview_ = bake(false);
    }
}

void GeographicLayers::build_terrain(const WorldDefinition& world)
{
    if (!map_) {
        const auto width = static_cast<int>(world.width());
        const auto height = static_cast<int>(world.height());
        std::vector<Color> land;
        std::vector<Color> water;
        land.reserve(world.sectors().size());
        water.reserve(world.sectors().size());
        for (const auto& sector : world.sectors()) {
            land.push_back(sector.properties.terrain == Terrain::forest ? Color{81, 114, 89, 255} :
                                                                         Color{129, 142, 103, 255});
            water.push_back(sector.properties.terrain == Terrain::water ? water_fill : BLANK);
        }
        terrain_ = LoadTextureFromImage({land.data(), width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8});
        water_ = LoadTextureFromImage({water.data(), width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8});
        return;
    }
    constexpr std::int64_t max_texture_size = 2048;
    const auto columns = 1 + (map_->columns * map_->sector_mm - 1) / map_->tile_mm;
    const auto rows = 1 + (map_->rows * map_->sector_mm - 1) / map_->tile_mm;
    const auto width = static_cast<int>(std::min(columns, max_texture_size));
    const auto height = static_cast<int>(std::min(rows, max_texture_size));
    std::vector<geography::Tile> samples(static_cast<std::size_t>(width) * height, map_->default_tile);

    if (width == columns && height == rows) {
        for (const auto& [cell, tile] : map_->tiles) {
            if (cell.first >= 0 && cell.first < width && cell.second >= 0 && cell.second < height) {
                samples[cell.second * width + cell.first] = tile;
            }
        }
    } else {
        // Sample tile centres for an unusually large raster without allocating the full grid.
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto found = map_->tiles.find({static_cast<std::int64_t>((x + 0.5) * columns / width),
                                                    static_cast<std::int64_t>((y + 0.5) * rows / height)});
                if (found != map_->tiles.end()) {
                    samples[y * width + x] = found->second;
                }
            }
        }
    }

    const auto [low, high] = std::ranges::minmax_element(samples, {}, &geography::Tile::elevation);
    const auto relief = std::max(50'000.0, static_cast<double>(high->elevation) - low->elevation);
    const auto sample = [&](int x, int y) -> const geography::Tile& {
        return samples[std::clamp(y, 0, height - 1) * width + std::clamp(x, 0, width - 1)];
    };
    std::vector<Color> land(samples.size());
    std::vector<Color> water(samples.size(), Color{water_fill.r, water_fill.g, water_fill.b, 0});
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto& tile = sample(x, y);
            Color base{133, 143, 103, 255};
            if (tile.cover == geography::Cover::forest) {
                base = {82, 118, 92, 255};
            } else if (tile.cover == geography::Cover::urban) {
                base = {148, 143, 126, 255};
            } else if (tile.cover == geography::Cover::water) {
                water[y * width + x] = water_fill;
            }

            // Illuminate from the northwest. Scale slope by the displayed sample spacing.
            const auto east = (static_cast<double>(sample(x + 1, y).elevation) - sample(x - 1, y).elevation) /
                (extent_.width * 2000.0 / width);
            const auto south = (static_cast<double>(sample(x, y + 1).elevation) - sample(x, y - 1).elevation) /
                (extent_.height * 2000.0 / height);
            const auto altitude = (static_cast<double>(tile.elevation) - low->elevation) / relief;
            const auto shade = std::clamp(0.89 + 0.23 * altitude + 0.58 * (east + south), 0.66, 1.30);
            land[y * width + x] = {static_cast<unsigned char>(base.r * shade),
                                   static_cast<unsigned char>(base.g * shade),
                                   static_cast<unsigned char>(base.b * shade), 255};
        }
    }
    terrain_ = LoadTextureFromImage({land.data(), width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8});
    water_ = LoadTextureFromImage({water.data(), width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8});
    SetTextureFilter(terrain_, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(water_, TEXTURE_FILTER_BILINEAR);
}

void GeographicLayers::draw_terrain(bool hydrography) const
{
    DrawTexturePro(terrain_, {0, 0, static_cast<float>(terrain_.width), static_cast<float>(terrain_.height)},
                   extent_, {}, 0, WHITE);
    if (hydrography) {
        DrawTexturePro(water_, {0, 0, static_cast<float>(water_.width), static_cast<float>(water_.height)},
                       extent_, {}, 0, WHITE);
    }
}

void GeographicLayers::draw_network(Camera2D camera, Rectangle visible, bool hydrography, bool roads, bool observer,
                                    const std::optional<FeatureHit>& selected, NetworkMode mode) const
{
    const bool baking = mode == NetworkMode::bake_overview;
    const bool cached_roads = mode == NetworkMode::cached_details && road_overview_.id;
    const bool cached_waterways = mode == NetworkMode::cached_details && waterway_overview_.id;
    const auto is_selected = [&](FeatureKind kind, std::size_t index) {
        return selected && selected->kind == kind && selected->index == index;
    };
    if (hydrography) {
        for (std::size_t i = 0; i < map_->waterways.size(); ++i) {
            const bool highlight = is_selected(FeatureKind::waterway, i);
            if ((cached_waterways && !highlight) || !overlaps(visible, waterway_bounds_[i])) {
                continue;
            }
            const auto& river = map_->waterways[i];
            const auto from = project(map_position(river.from), camera);
            const auto to = project(map_position(river.to), camera);
            if (highlight) {
                DrawLineEx(from, to, 4, WHITE);
            }
            DrawLineEx(from, to, baking ? 4.3f : 1.7f, river_ink);
        }
    }
    if (roads) {
        for (const auto i : road_draw_order_) {
            const auto& road = map_->roads[i];
            const bool bridge = road.crossing == geography::Crossing::bridge;
            const bool highlight = is_selected(FeatureKind::road, i);
            const bool closed = observer && road.closed;
            if (cached_roads && !highlight && !closed) {
                continue;
            }
            const auto& line = roads_[i];
            if (!overlaps(visible, line.bounds)) {
                continue;
            }
            const auto color = closed ? closed_ink : bridge ? bridge_ink :
                road.surface == geography::Surface::paved ? paved_ink : gravel_ink;
            const auto stroke = baking ? (bridge ? 4.6f : 3.2f) :
                bridge ? 2.8f : camera.zoom >= 0.09f ? 1.65f : 1.0f;
            for (std::size_t j = 1; j < line.points.size(); ++j) {
                const auto a = project(line.points[j - 1], camera);
                const auto b = project(line.points[j], camera);
                if (highlight) {
                    DrawLineEx(a, b, stroke + 3, WHITE);
                }
                if (stroke == 1) {
                    DrawLineV(a, b, color);
                } else {
                    DrawLineEx(a, b, stroke, color);
                }
            }
        }
    }
}

void GeographicLayers::draw_vectors(Camera2D camera, Rectangle viewport, bool hydrography, bool roads, bool sites,
                                    bool observer, const std::optional<FeatureHit>& selected) const
{
    if (!map_) {
        return;
    }
    const auto visible = view_bounds(camera, viewport);
    const bool cached = camera.zoom <= overview_zoom_;
    if (cached && overlaps(visible, extent_)) {
        const auto origin = project({}, camera);
        const auto scale = camera.zoom / (overview_zoom_ / 0.95f);
        const auto draw_overview = [&](RenderTexture2D layer) {
            if (layer.id) {
                const auto w = static_cast<float>(layer.texture.width);
                const auto h = static_cast<float>(layer.texture.height);
                // Render targets use framebuffer coordinates, with their origin at the bottom.
                DrawTexturePro(layer.texture, {0, 0, w, -h}, {origin.x, origin.y, w * scale, h * scale}, {}, 0, WHITE);
            }
        };
        if (hydrography) {
            draw_overview(waterway_overview_);
        }
        if (roads) {
            draw_overview(road_overview_);
        }
    }
    draw_network(camera, visible, hydrography, roads, observer, selected,
                 cached ? NetworkMode::cached_details : NetworkMode::vectors);
    if (sites) {
        for (std::size_t i = 0; i < map_->site_features.size(); ++i) {
            const auto& site = map_->site_features[i];
            const auto position = map_position(site.position);
            const bool highlight = selected && selected->kind == FeatureKind::site && selected->index == i;
            if ((show_site(site.kind, camera.zoom) || highlight) &&
                CheckCollisionPointRec(position, visible)) {
                const auto screen = project(position, camera);
                const float radius = site.kind <= geography::SiteKind::rail_station ? 4 : 2;
                DrawCircleV(screen, radius + 1, {35, 34, 28, 255});
                DrawCircleV(screen, radius, highlight ? WHITE : site_ink);
            }
        }
    }
    if (hydrography && selected && selected->kind == FeatureKind::water_tile) {
        const auto position = map_position(selected->position);
        if (CheckCollisionPointRec(position, visible)) {
            const auto point = project(position, camera);
            DrawCircleLines(std::lround(point.x), std::lround(point.y), 7, WHITE);
        }
    }
}

std::optional<geography::Tile> GeographicLayers::tile_at(Vector2 position) const
{
    if (!map_ || position.x < 0 || position.y < 0 || position.x >= extent_.width || position.y >= extent_.height) {
        return {};
    }
    const auto x = static_cast<std::int64_t>(position.x * 1000.0) / map_->tile_mm;
    const auto y = static_cast<std::int64_t>(position.y * 1000.0) / map_->tile_mm;
    const auto tile = map_->tiles.find({x, y});
    return tile == map_->tiles.end() ? map_->default_tile : tile->second;
}

std::vector<FeatureHit> GeographicLayers::hits(Vector2 mouse, Camera2D camera, bool hydrography,
                                               bool roads, bool sites) const
{
    std::vector<FeatureHit> hits;
    if (!map_) {
        return hits;
    }
    const auto point = GetScreenToWorld2D(mouse, camera);
    const auto radius = 6 / camera.zoom;
    const auto radius_squared = radius * radius;
    const Rectangle area{point.x - radius, point.y - radius, 2 * radius, 2 * radius};
    // Keep a single nearest hit for each layer so dense roads do not dominate selection.
    if (sites) {
        std::vector<std::pair<float, FeatureHit>> site_hits;
        for (std::size_t i = 0; i < map_->site_features.size(); ++i) {
            const auto& site = map_->site_features[i];
            const auto position = map_position(site.position);
            const auto distance = distance_squared(point, position, position, 0);
            if (show_site(site.kind, camera.zoom) && distance < radius_squared) {
                site_hits.emplace_back(distance, FeatureHit{FeatureKind::site, i, site.position});
            }
        }
        std::ranges::stable_sort(site_hits, {}, &std::pair<float, FeatureHit>::first);
        for (const auto& hit : site_hits) {
            hits.push_back(hit.second);
        }
    }
    if (roads) {
        auto best = radius_squared;
        std::optional<FeatureHit> closest;
        for (std::size_t i = 0; i < roads_.size(); ++i) {
            const auto& line = roads_[i];
            if (!overlaps(area, line.bounds)) {
                continue;
            }
            for (std::size_t j = 1; j < line.points.size(); ++j) {
                const auto a = line.points[j - 1];
                const auto b = line.points[j];
                const auto fraction = nearest_fraction(point, a, b);
                const auto distance = distance_squared(point, a, b, fraction);
                if (distance < best) {
                    best = distance;
                    const auto& road = map_->roads[i];
                    const auto from = j == 1 ? nodes_.at(road.from) : road.waypoints[j - 2];
                    const auto to = j + 1 == line.points.size() ? nodes_.at(road.to) : road.waypoints[j - 1];
                    closest = FeatureHit{FeatureKind::road, i, interpolate(from, to, fraction)};
                }
            }
        }
        if (closest) {
            hits.push_back(*closest);
        }
    }
    if (hydrography) {
        auto best = radius_squared;
        std::optional<FeatureHit> closest;
        for (std::size_t i = 0; i < map_->waterways.size(); ++i) {
            if (!overlaps(area, waterway_bounds_[i])) {
                continue;
            }
            const auto& river = map_->waterways[i];
            const auto a = map_position(river.from);
            const auto b = map_position(river.to);
            const auto fraction = nearest_fraction(point, a, b);
            const auto distance = distance_squared(point, a, b, fraction);
            if (distance < best) {
                best = distance;
                closest = FeatureHit{FeatureKind::waterway, i, interpolate(river.from, river.to, fraction)};
            }
        }
        if (closest) {
            hits.push_back(*closest);
        } else if (const auto tile = tile_at(point); tile && tile->cover == geography::Cover::water) {
            hits.push_back({FeatureKind::water_tile, 0,
                {std::llround(point.x * 1000.0), std::llround(point.y * 1000.0), tile->elevation}});
        }
    }
    return hits;
}

FeatureDetails GeographicLayers::inspect(const FeatureHit& hit, bool observer) const
{
    switch (hit.kind) {
    case FeatureKind::road: {
        const auto& road = map_->roads[hit.index];
        const bool bridge = road.crossing == geography::Crossing::bridge;
        const auto surface = road.surface == geography::Surface::paved ? "Paved" : "Unpaved";
        constexpr std::array directions{"Two-way", "One-way start to end", "One-way end to start"};
        FeatureDetails details{std::string(bridge ? "Bridge #" : "Road #") + std::to_string(road.id),
            {std::string(surface) + " / " + directions.at(static_cast<std::size_t>(road.direction)),
             "Speed limit " + std::to_string(std::llround(road.speed_limit_mm_s * 0.0036)) + " km/h"}};
        if (road.osm_way_id) {
            details.lines.push_back("OSM way/" + std::to_string(road.osm_way_id));
        }
        if (observer) {
            details.lines.emplace_back(road.closed ? "Closed" : "Open");
        }
        return details;
    }
    case FeatureKind::waterway:
        return {"Waterway", {"Vector segment #" + std::to_string(hit.index + 1)}};
    case FeatureKind::water_tile:
        return {"Surface water", {"Imported land cover"}};
    case FeatureKind::site: {
        const auto& site = map_->site_features[hit.index];
        constexpr std::array elements{"node", "way", "relation"};
        const auto kind = site_kind(site.kind);
        FeatureDetails details{site.name.empty() ? std::string(kind) : site.name,
            {std::string(kind) + " / OSM " + elements.at(static_cast<std::size_t>(site.element)) +
             "/" + std::to_string(site.source_id)}};
        if (site.length_mm || site.width_mm) {
            details.lines.push_back("Length " + (site.length_mm ? metres(site.length_mm) : "?") +
                                    " / Width " + (site.width_mm ? metres(site.width_mm) : "?"));
        }
        if (site.area_m2) {
            details.lines.push_back("Area " + std::to_string(site.area_m2) + " m2");
        }
        if (!site.surface.empty()) {
            details.lines.push_back("Surface " + site.surface);
        }
        return details;
    }
    }
    return {};
}

}  // namespace openwar::viewer
