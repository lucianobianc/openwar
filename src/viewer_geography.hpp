#ifndef OPENWAR_VIEWER_GEOGRAPHY_HPP
#define OPENWAR_VIEWER_GEOGRAPHY_HPP

#include "viewer_model.hpp"

#include <raylib.h>

namespace openwar::viewer {

Vector2 map_position(geography::Point point);

enum class FeatureKind { road, waterway, site, water_tile };

struct FeatureHit {
    FeatureKind kind;
    std::size_t index;
    geography::Point position;
    bool operator==(const FeatureHit&) const = default;
};

struct FeatureDetails {
    std::string label;
    std::vector<std::string> lines;
};

class GeographicLayers {
public:
    GeographicLayers(const geography::Map* map, const WorldDefinition& world);
    ~GeographicLayers();
    GeographicLayers(const GeographicLayers&) = delete;
    GeographicLayers& operator=(const GeographicLayers&) = delete;

    bool has_source_attribution() const { return map_ && !map_->provenance.empty(); }
    void draw_terrain(bool hydrography) const;  // Inside the map camera.
    void draw_vectors(Camera2D camera, Rectangle viewport, bool hydrography, bool roads, bool sites,
                      bool observer, const std::optional<FeatureHit>& selected) const;
    std::vector<FeatureHit> hits(Vector2 mouse, Camera2D camera, bool hydrography, bool roads, bool sites) const;
    FeatureDetails inspect(const FeatureHit& hit, bool observer) const;
    std::optional<geography::Tile> tile_at(Vector2 position) const;

private:
    enum class NetworkMode { bake_overview, vectors, cached_details };

    struct Line {
        std::vector<Vector2> points;
        Rectangle bounds;
    };

    void build_terrain(const WorldDefinition& world);
    void build_overview();
    void draw_network(Camera2D camera, Rectangle visible, bool hydrography, bool roads, bool observer,
                      const std::optional<FeatureHit>& selected, NetworkMode mode) const;
    const geography::Map* map_;
    Rectangle extent_{};
    Texture2D terrain_{};
    Texture2D water_{};
    RenderTexture2D road_overview_{};
    RenderTexture2D waterway_overview_{};
    float overview_zoom_{};
    std::map<std::uint32_t, geography::Point> nodes_;
    std::vector<Line> roads_;
    std::vector<std::size_t> road_draw_order_;
    std::vector<Rectangle> waterway_bounds_;
};

}  // namespace openwar::viewer

#endif
