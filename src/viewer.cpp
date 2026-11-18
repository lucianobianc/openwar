#include "command.hpp"
#include "viewer_geography.hpp"
#include "viewer_model.hpp"
#include "viewer_symbols.hpp"
#include "realtime.hpp"
#include "scenario.hpp"

#include <raylib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace {

using namespace openwar;
using namespace openwar::viewer;

constexpr Color background{15, 22, 33, 255};
constexpr Color panel{24, 33, 47, 255};
constexpr Color border{47, 62, 83, 255};
constexpr Color ink{231, 240, 250, 255};
constexpr Color muted{161, 177, 196, 255};
constexpr Color accent{86, 211, 184, 255};
constexpr Color west{46, 105, 174, 255};
constexpr Color east{168, 64, 77, 255};
constexpr Color neutral{84, 96, 111, 255};
constexpr Color unknown{36, 46, 62, 255};
constexpr Color contested{216, 172, 63, 255};

enum Layer { sectors, territory, units, carriers, roads, hydrography, sites, layer_count };
constexpr std::array layer_names{"Sectors", "Control", "Units", "Carriers", "Roads", "Water", "Sites"};
using Selection = std::variant<MarkerKey, FeatureHit>;

struct Layout {
    Rectangle map;
    Rectangle details;
    Rectangle events;
    std::array<Rectangle, 3> perspective;
    Rectangle play;
    Rectangle slower;
    Rectangle faster;
    Rectangle fit;
    std::array<Rectangle, layer_count> layers;
};

Layout layout()
{
    const float width = GetScreenWidth();
    const float height = GetScreenHeight();
    constexpr float side = 350;
    const float details_height = std::min(350.f, height - 290);
    Layout screen{{16, 150, width - side - 48, height - 194},
                  {width - side - 16, 150, side, details_height},
                  {width - side - 16, 163 + details_height, side, height - details_height - 205},
                  {{{width - 294, 19, 108, 32}, {width - 178, 19, 76, 32}, {width - 94, 19, 78, 32}}},
                  {16, 65, 98, 32}, {126, 65, 37, 32}, {230, 65, 37, 32}, {16, 108, 73, 31}, {}};
    for (std::size_t i = 0; i < screen.layers.size(); ++i) {
        screen.layers[i] = {104 + static_cast<float>(i) * 92, 108, 84, 31};
    }
    return screen;
}

struct OrderDraft {
    OrderAction action;
    std::optional<OrderTarget> target{};
    geography::Point pin{};
    SectorId sector{};
};

struct CommandFeedback {
    std::string message;
    bool accepted;
};

struct ViewControls {
    std::optional<FactionId> perspective;
    std::optional<SectorId> selected;
    std::optional<Selection> object;
    std::optional<OrderDraft> draft;
    std::optional<MarkerKey> cancel_prompt;
    std::optional<CommandFeedback> feedback;
    std::array<bool, layer_count> layers{false, true, true, true, true, true, true};
    Camera2D camera{};
    bool panning = false;
    std::uint32_t speed = 60;
    bool paused = true;
    std::size_t event_offset = 0;
    std::size_t detail_offset = 0;
};

void label(std::string text, float x, float y, int size, Color color, float width)
{
    if (width < MeasureText("...", size)) {
        return;
    }
    if (MeasureText(text.c_str(), size) > width) {
        while (!text.empty() && MeasureText((text + "...").c_str(), size) > width) {
            text.pop_back();
        }
        text += "...";
    }
    DrawText(text.c_str(), static_cast<int>(x), static_cast<int>(y), size, color);
}

void button(Rectangle bounds, const char* title, bool selected = false)
{
    const bool hovered = CheckCollisionPointRec(GetMousePosition(), bounds);
    DrawRectangleRec(bounds, selected ? Color{34, 74, 73, 255} : hovered ? border : panel);
    DrawRectangleLinesEx(bounds, 1, selected ? accent : border);
    const auto text_width = MeasureText(title, 16);
    DrawText(title, static_cast<int>(bounds.x + (bounds.width - text_width) / 2),
             static_cast<int>(bounds.y + 8), 16, selected ? accent : ink);
}

bool clicked(Rectangle bounds)
{
    return IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), bounds);
}

float fit_zoom(const Snapshot& snapshot, Rectangle map)
{
    const float width = static_cast<float>(snapshot.width) * snapshot.sector_size_m;
    const float height = static_cast<float>(snapshot.height) * snapshot.sector_size_m;
    return std::min({(map.width - 42) / width, (map.height - 42) / height, 2.0f});
}

void fit_map(ViewControls& controls, const Snapshot& snapshot, Rectangle map)
{
    controls.camera.target = {static_cast<float>(snapshot.width) * snapshot.sector_size_m / 2,
                              static_cast<float>(snapshot.height) * snapshot.sector_size_m / 2};
    controls.camera.zoom = fit_zoom(snapshot, map);
}

bool marker_visible(const Marker& marker, const ViewControls& controls)
{
    switch (marker.kind) {
    case MarkerKind::carrier: return controls.layers[carriers];
    case MarkerKind::installation:
    case MarkerKind::depot: return controls.layers[sites];
    default: return controls.layers[units];
    }
}

bool feature_visible(const FeatureHit& feature, const ViewControls& controls)
{
    switch (feature.kind) {
    case FeatureKind::road: return controls.layers[roads];
    case FeatureKind::waterway:
    case FeatureKind::water_tile: return controls.layers[hydrography];
    case FeatureKind::site: return controls.layers[sites];
    }
    return false;
}

const Marker* selected_marker(const Snapshot& snapshot, const ViewControls& controls)
{
    const auto* key = controls.object ? std::get_if<MarkerKey>(&*controls.object) : nullptr;
    if (key) {
        const auto marker = std::ranges::find(snapshot.markers, *key, &Marker::key);
        if (marker != snapshot.markers.end() && marker_visible(*marker, controls)) {
            return &*marker;
        }
    }
    return nullptr;
}

std::optional<FeatureHit> selected_feature(const ViewControls& controls)
{
    const auto* feature = controls.object ? std::get_if<FeatureHit>(&*controls.object) : nullptr;
    if (feature && feature_visible(*feature, controls)) {
        return *feature;
    }
    return {};
}

std::ptrdiff_t visible_marker_count(const Snapshot& snapshot, const ViewControls& controls)
{
    return std::ranges::count_if(snapshot.markers, [&](const Marker& marker) {
        return marker.sector == controls.selected && marker_visible(marker, controls);
    });
}

std::optional<SectorId> sector_at(const Snapshot& snapshot, Vector2 point)
{
    const float size = snapshot.sector_size_m;
    if (point.x < 0 || point.y < 0 || point.x >= snapshot.width * size || point.y >= snapshot.height * size) {
        return {};
    }
    return SectorId{static_cast<std::uint32_t>(point.y / size) * snapshot.width +
                    static_cast<std::uint32_t>(point.x / size)};
}

void select_at(ViewControls& controls, const Snapshot& snapshot, const GeographicLayers& geography, Vector2 mouse)
{
    const auto point = GetScreenToWorld2D(mouse, controls.camera);
    controls.detail_offset = 0;
    controls.selected = sector_at(snapshot, point);
    std::vector<std::pair<float, MarkerKey>> markers;
    for (const auto& marker : snapshot.markers) {
        if (!marker_visible(marker, controls)) {
            continue;
        }
        const auto anchor = GetWorldToScreen2D(map_position(marker.position), controls.camera);
        const auto dx = mouse.x - anchor.x;
        const auto dy = mouse.y - anchor.y;
        const auto distance = dx * dx + dy * dy;
        if (marker.symbol ? hit_military_symbol(marker, {dx, dy}) : std::abs(dx) + std::abs(dy) <= 14) {
            markers.emplace_back(distance, marker.key());
        }
    }
    std::ranges::stable_sort(markers, {}, &std::pair<float, MarkerKey>::first);
    std::vector<Selection> hits;
    for (const auto& candidate : markers) {
        hits.emplace_back(candidate.second);
    }
    for (const auto& hit : geography.hits(mouse, controls.camera, controls.layers[hydrography],
                                         controls.layers[roads], controls.layers[sites])) {
        hits.emplace_back(hit);
    }
    if (hits.empty()) {
        controls.object.reset();
        return;
    }
    // Repeated clicks cycle through overlapping assets and feature layers.
    const auto current = controls.object ? std::ranges::find_if(hits, [&](const Selection& hit) {
        const auto* feature = std::get_if<FeatureHit>(&hit);
        const auto* selected = std::get_if<FeatureHit>(&*controls.object);
        return feature && selected ? feature->kind == selected->kind && feature->index == selected->index :
            hit == *controls.object;
    }) : hits.end();
    controls.object = (current == hits.end() || std::next(current) == hits.end()) ? hits.front() : *std::next(current);
}

void handle_playback(ViewControls& controls, const Layout& screen, bool ended, bool observer)
{
    constexpr std::array<std::optional<FactionId>, 3> perspectives{{{}, FactionId::west, FactionId::east}};
    constexpr std::array keys{KEY_ONE, KEY_TWO, KEY_THREE};
    for (std::size_t i = 0; observer && i < perspectives.size(); ++i) {
        if ((clicked(screen.perspective[i]) || IsKeyPressed(keys[i])) && controls.perspective != perspectives[i]) {
            controls.perspective = perspectives[i];
            controls.object.reset();
            controls.event_offset = 0;
            controls.detail_offset = 0;
        }
    }
    if (!ended && (clicked(screen.play) || IsKeyPressed(KEY_SPACE))) {
        controls.paused = !controls.paused;
    }
    if (clicked(screen.slower) || IsKeyPressed(KEY_LEFT_BRACKET)) {
        controls.speed = std::max(1u, controls.speed / 2);
    }
    if (clicked(screen.faster) || IsKeyPressed(KEY_RIGHT_BRACKET)) {
        controls.speed = std::min(3840u, controls.speed * 2);
    }
}

void choose_destination(ViewControls& controls, const Snapshot& snapshot, const GeographicLayers& geography,
                        Vector2 mouse)
{
    const auto point = GetScreenToWorld2D(mouse, controls.camera);
    if (!sector_at(snapshot, point)) {
        return;
    }
    const auto sector_mm = std::int64_t{snapshot.sector_size_m} * 1000;
    const auto east = std::min(std::llround(point.x * 1000.0),
                              static_cast<long long>(snapshot.width * sector_mm - 1));
    const auto south = std::min(std::llround(point.y * 1000.0),
                               static_cast<long long>(snapshot.height * sector_mm - 1));
    const auto sector = SectorId{static_cast<std::uint32_t>(south / sector_mm) * snapshot.width +
                                 static_cast<std::uint32_t>(east / sector_mm)};
    auto& draft = *controls.draft;
    draft.sector = sector;
    if (draft.action.geographic_destination) {
        const auto tile = geography.tile_at(point);
        draft.pin = {east, south, tile ? tile->elevation : 0};
        draft.target = draft.pin;
    } else {
        draft.pin = snapshot.sector_center(sector);
        draft.target = sector;
    }
    controls.feedback.reset();
}

std::size_t inspection_rows(Rectangle area)
{
    return static_cast<std::size_t>(std::max(1.f, (area.height - 248) / 18));
}

std::size_t inspection_max_offset(const Marker& marker, Rectangle area)
{
    const auto visible = inspection_rows(area);
    return marker.inspection.size() > visible ? marker.inspection.size() - visible : 0;
}

void handle_map_input(ViewControls& controls, const Snapshot& snapshot, const GeographicLayers& geography,
                      const Layout& screen)
{
    for (std::size_t i = 0; i < screen.layers.size(); ++i) {
        if (clicked(screen.layers[i])) {
            controls.layers[i] = !controls.layers[i];
            controls.detail_offset = 0;
            if (!selected_marker(snapshot, controls) && !selected_feature(controls)) {
                controls.object.reset();
            }
        }
    }
    controls.camera.offset = {screen.map.x + screen.map.width / 2, screen.map.y + screen.map.height / 2};
    if (clicked(screen.fit) || IsKeyPressed(KEY_R) || IsKeyPressed(KEY_HOME)) {
        fit_map(controls, snapshot, screen.map);
    }
    const auto mouse = GetMousePosition();
    const auto wheel = GetMouseWheelMove();
    if (CheckCollisionPointRec(mouse, screen.events) && wheel != 0) {
        const auto offset = static_cast<long long>(controls.event_offset) + (wheel > 0 ? 3 : -3);
        controls.event_offset = static_cast<std::size_t>(std::clamp(
            offset, 0LL, snapshot.events.empty() ? 0LL : static_cast<long long>(snapshot.events.size() - 1)));
    }
    if (CheckCollisionPointRec(mouse, screen.details) && wheel != 0) {
        const auto* marker = selected_marker(snapshot, controls);
        const auto last = marker ? static_cast<std::ptrdiff_t>(inspection_max_offset(*marker, screen.details)) :
            controls.selected && !controls.object ? std::max<std::ptrdiff_t>(0, visible_marker_count(snapshot, controls) - 1) : 0;
        const auto offset = static_cast<std::ptrdiff_t>(controls.detail_offset) + (wheel > 0 ? -1 : 1);
        controls.detail_offset = static_cast<std::size_t>(std::clamp(offset, std::ptrdiff_t{0}, last));
    }

    if (CheckCollisionPointRec(mouse, screen.map)) {
        if (wheel != 0) {
            const auto anchor = GetScreenToWorld2D(mouse, controls.camera);
            controls.camera.zoom = std::clamp(controls.camera.zoom * std::pow(1.16f, wheel),
                                              fit_zoom(snapshot, screen.map) / 4, 2.0f);
            const auto after = GetScreenToWorld2D(mouse, controls.camera);
            controls.camera.target.x += anchor.x - after.x;
            controls.camera.target.y += anchor.y - after.y;
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) || IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
            controls.panning = true;
        }
        if (clicked(screen.map)) {
            if (controls.draft) {
                choose_destination(controls, snapshot, geography, mouse);
            } else {
                controls.cancel_prompt.reset();
                select_at(controls, snapshot, geography, mouse);
            }
        }
    }
    if (controls.panning) {
        controls.panning = IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
        const auto delta = GetMouseDelta();
        controls.camera.target.x -= delta.x / controls.camera.zoom;
        controls.camera.target.y -= delta.y / controls.camera.zoom;
    }
    const float step = 450 * GetFrameTime() / controls.camera.zoom;
    controls.camera.target.x += step * (IsKeyDown(KEY_RIGHT) - IsKeyDown(KEY_LEFT));
    controls.camera.target.y += step * (IsKeyDown(KEY_DOWN) - IsKeyDown(KEY_UP));
}

Rectangle action_button(Rectangle panel, std::size_t index)
{
    return {panel.x + 15 + static_cast<float>(index % 3) * 107,
            panel.y + 159 + static_cast<float>(index / 3) * 32, 103, 29};
}

Rectangle review_button(Rectangle panel, bool confirm)
{
    return {panel.x + (confirm ? 148 : 15), panel.y + 192, confirm ? 184.f : 123.f, 32};
}

Rectangle cancel_mission_button(Rectangle area)
{
    return {area.x + 15, area.y + 159, 146, 29};
}

std::string action_label(MissionKind kind)
{
    switch (kind) {
    case MissionKind::recon: return "Recon";
    case MissionKind::patrol: return "Patrol";
    case MissionKind::defend: return "Defend";
    case MissionKind::reposition: return "Reposition";
    case MissionKind::attack: return "Attack";
    }
    return {};
}

bool handle_command_input(ViewControls& controls, const Snapshot& snapshot, Rectangle panel,
                          CommandSession& session)
{
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        return false;
    }
    if (controls.draft || controls.cancel_prompt) {
        if (clicked(review_button(panel, false))) {
            controls.draft.reset();
            controls.cancel_prompt.reset();
            controls.feedback.reset();
            return false;
        }
        if (!clicked(review_button(panel, true))) {
            return false;
        }
        if (controls.cancel_prompt) {
            const auto key = *controls.cancel_prompt;
            const auto result = session.cancel(*key.faction, MissionId{key.id});
            controls.cancel_prompt.reset();
            controls.feedback = CommandFeedback{std::string(result.reason), result.cancelled};
            return result.cancelled;
        }
        if (!controls.draft->target) {
            return false;
        }
        const auto result = session.submit(controls.draft->action.kind, *controls.draft->target);
        if (!result.mission) {
            controls.feedback = CommandFeedback{std::string(result.reason), false};
            return false;
        }
        const auto id = static_cast<std::uint32_t>(*result.mission);
        controls.feedback = CommandFeedback{"Order #" + std::to_string(id) + " scheduled", true};
        controls.selected = controls.draft->sector;
        controls.object = MarkerKey{MarkerKind::mission, session.permissions().faction, id};
        controls.draft.reset();
        return true;
    }
    if (const auto* marker = selected_marker(snapshot, controls)) {
        if (marker->kind == MarkerKind::unit) {
            const auto actions = session.actions_for(UnitId{marker->id});
            for (std::size_t i = 0; i < actions.size(); ++i) {
                if (clicked(action_button(panel, i))) {
                    controls.draft = OrderDraft{actions[i]};
                    controls.feedback.reset();
                    break;
                }
            }
        } else if (marker->kind == MarkerKind::mission && marker->faction &&
                   session.can_cancel(*marker->faction, MissionId{marker->id}) &&
                   clicked(cancel_mission_button(panel))) {
            controls.cancel_prompt = marker->key();
            controls.feedback.reset();
        }
    }
    return false;
}

Color faction_color(std::optional<FactionId> faction)
{
    if (!faction) {
        return unknown;
    }
    if (*faction == FactionId::west) {
        return west;
    }
    return *faction == FactionId::east ? east : neutral;
}

Color territory_tint(const Tile& tile)
{
    const auto fill = tile.confidence < 350 ? unknown : tile.contested ? contested : faction_color(tile.controller);
    const auto alpha = tile.confidence < 750 ? 0.18f :
        tile.contested ? 0.41f : tile.controller == FactionId::neutral ? 0.12f : 0.32f;
    return Fade(fill, alpha);
}

class TerritoryLayer {
public:
    TerritoryLayer() = default;
    TerritoryLayer(const TerritoryLayer&) = delete;
    TerritoryLayer& operator=(const TerritoryLayer&) = delete;
    ~TerritoryLayer()
    {
        if (texture_.id) {
            UnloadTexture(texture_);
        }
    }

    void update(const Snapshot& snapshot)
    {
        pixels_.resize(snapshot.tiles.size());
        bool changed = false;
        for (std::size_t i = 0; i < pixels_.size(); ++i) {
            const auto tint = territory_tint(snapshot.tiles[i]);
            changed |= ColorToInt(tint) != ColorToInt(pixels_[i]);
            pixels_[i] = tint;
        }
        if (!texture_.id) {
            texture_ = LoadTextureFromImage({pixels_.data(), static_cast<int>(snapshot.width),
                static_cast<int>(snapshot.height), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8});
        } else if (changed) {
            UpdateTexture(texture_, pixels_.data());
        }
    }

    void draw(float sector_size_m) const
    {
        const auto width = static_cast<float>(texture_.width);
        const auto height = static_cast<float>(texture_.height);
        DrawTexturePro(texture_, {0, 0, width, height}, {0, 0, width * sector_size_m, height * sector_size_m},
                       {}, 0, WHITE);
    }

private:
    Texture2D texture_{};
    std::vector<Color> pixels_;
};

void draw_sectors(const Snapshot& snapshot, const ViewControls& controls, Rectangle map)
{
    if (!controls.layers[sectors] && (!controls.selected || controls.object)) {
        return;
    }
    const auto camera = controls.camera;
    const float size = snapshot.sector_size_m;
    const auto corner = GetScreenToWorld2D({map.x, map.y}, camera);
    const auto end = GetScreenToWorld2D({map.x + map.width, map.y + map.height}, camera);
    const auto index = [size](float coordinate, std::uint32_t count) {
        return static_cast<std::uint32_t>(std::clamp(
            std::floor(static_cast<double>(coordinate) / size), 0.0, static_cast<double>(count)));
    };
    const auto first_x = index(corner.x, snapshot.width);
    const auto first_y = index(corner.y, snapshot.height);
    const auto last_x = index(end.x + size, snapshot.width);
    const auto last_y = index(end.y + size, snapshot.height);
    BeginMode2D(camera);
    if (controls.layers[sectors] && first_x < last_x && first_y < last_y) {
        const auto grid = Fade(background, 0.50f);
        for (auto x = first_x; x <= last_x; ++x) {
            DrawLineV({x * size, first_y * size}, {x * size, last_y * size}, grid);
        }
        for (auto y = first_y; y <= last_y; ++y) {
            DrawLineV({first_x * size, y * size}, {last_x * size, y * size}, grid);
        }
    }
    if (controls.selected && !controls.object) {
        const auto id = static_cast<std::uint32_t>(*controls.selected);
        DrawRectangleLinesEx({(id % snapshot.width) * size, (id / snapshot.width) * size, size, size},
                             2 / camera.zoom, RAYWHITE);
    }
    EndMode2D();
    if (controls.layers[sectors] && size * camera.zoom >= 52) {
        for (auto y = first_y; y < last_y; ++y) {
            for (auto x = first_x; x < last_x; ++x) {
                const auto origin = GetWorldToScreen2D({x * size, y * size}, camera);
                DrawText(TextFormat("%u", y * snapshot.width + x), origin.x + 5, origin.y + 5, 12, ink);
            }
        }
    }
}

void draw_marker(const Marker& marker, Vector2 at, bool selected)
{
    if (marker.symbol) {
        draw_military_symbol(marker, at);
    } else {
        DrawLineV({at.x, at.y - 12}, {at.x + 12, at.y}, accent);
        DrawLineV({at.x + 12, at.y}, {at.x, at.y + 12}, accent);
        DrawLineV({at.x, at.y + 12}, {at.x - 12, at.y}, accent);
        DrawLineV({at.x - 12, at.y}, {at.x, at.y - 12}, accent);
    }
    if (selected) {
        DrawCircleLines(std::lround(at.x), std::lround(at.y), 17, WHITE);
    }
}

void draw_map(const Snapshot& snapshot, const ViewControls& controls, const GeographicLayers& geography,
              const TerritoryLayer& control, Rectangle map)
{
    DrawRectangleRec(map, {11, 17, 27, 255});
    BeginScissorMode(static_cast<int>(map.x), static_cast<int>(map.y),
                     static_cast<int>(map.width), static_cast<int>(map.height));
    BeginMode2D(controls.camera);
    geography.draw_terrain(controls.layers[hydrography]);
    if (controls.layers[territory]) {
        control.draw(snapshot.sector_size_m);
    }
    EndMode2D();
    draw_sectors(snapshot, controls, map);
    geography.draw_vectors(controls.camera, map, controls.layers[hydrography], controls.layers[roads],
                           controls.layers[sites], snapshot.roads, selected_feature(controls));
    constexpr float margin = 27;
    const Rectangle marker_area{map.x - margin, map.y - margin, map.width + 2 * margin, map.height + 2 * margin};
    const auto* selection = selected_marker(snapshot, controls);
    for (const auto& marker : snapshot.markers) {
        if (&marker != selection && marker_visible(marker, controls)) {
            const auto position = GetWorldToScreen2D(map_position(marker.position), controls.camera);
            if (CheckCollisionPointRec(position, marker_area)) {
                draw_marker(marker, position, false);
            }
        }
    }
    if (selection) {
        const auto at = GetWorldToScreen2D(map_position(selection->position), controls.camera);
        if (CheckCollisionPointRec(at, marker_area)) {
            draw_marker(*selection, at, true);
        }
    }
    if (controls.draft && controls.draft->target) {
        const auto pin = GetWorldToScreen2D(map_position(controls.draft->pin), controls.camera);
        DrawCircleLines(std::lround(pin.x), std::lround(pin.y), 10, accent);
        DrawLineV({pin.x - 14, pin.y}, {pin.x + 14, pin.y}, accent);
        DrawLineV({pin.x, pin.y - 14}, {pin.x, pin.y + 14}, accent);
    }
    EndScissorMode();
    DrawRectangleLinesEx(map, 1, border);

    const auto metres_at_80_px = 80 / controls.camera.zoom;
    const auto power = std::pow(10.0, std::floor(std::log10(metres_at_80_px)));
    const auto metres = power * (metres_at_80_px >= 5 * power ? 5 : metres_at_80_px >= 2 * power ? 2 : 1);
    const auto scale_width = static_cast<float>(metres) * controls.camera.zoom;
    const auto right = map.x + map.width - 11;
    const auto bottom = map.y + map.height - 12;
    DrawRectangleRec({right - 96, bottom - 22, 100, 29}, Fade(background, 0.87f));
    DrawLineV({right - scale_width, bottom}, {right, bottom}, ink);
    DrawLineV({right - scale_width, bottom - 4}, {right - scale_width, bottom + 2}, ink);
    DrawLineV({right, bottom - 4}, {right, bottom + 2}, ink);
    const auto distance = metres >= 1000 ? std::to_string(std::llround(metres / 1000)) + " km" :
                                           std::to_string(std::llround(metres)) + " m";
    label(distance, right - 92, bottom - 19, 13, ink, 89);
    label("N", right - 9, map.y + 13, 14, ink, 16);
    DrawTriangle({right - 4, map.y + 29}, {right - 8, map.y + 38}, {right, map.y + 38}, ink);
}

std::string position_text(geography::Point point, bool include_elevation)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << "E " << point.east / 1000.0 << "  S " << point.south / 1000.0;
    if (include_elevation) {
        text << "  Up " << point.elevation / 1000.0;
    }
    text << " m";
    return text.str();
}

void draw_details(const Snapshot& snapshot, const ViewControls& controls, const GeographicLayers& geography,
                  const CommandSession& session, Rectangle area)
{
    DrawRectangleRec(area, panel);
    const float x = area.x + 15;
    float y = area.y + 14;
    const float width = area.width - 30;
    if (controls.draft) {
        const auto& draft = *controls.draft;
        label(action_label(draft.action.kind) + " order draft", x, y, 21, ink, width);
        label("The engine assigns an eligible formation.", x, y + 34, 14, muted, width);
        if (draft.target) {
            const auto id = static_cast<std::uint32_t>(draft.sector);
            const auto& tile = snapshot.tiles.at(id);
            const auto owner = tile.controller ? std::string(faction_name(*tile.controller)) : "Unknown";
            label("Target sector " + std::to_string(id) + " / " + owner, x, y + 71, 17, accent, width);
            if (draft.action.geographic_destination) {
                label(position_text(draft.pin, geography.has_source_attribution()), x, y + 99, 15, ink, width);
            }
            label("Click the map to change the destination.", x, y + 132, 14, muted, width);
            button(review_button(area, true), "Confirm order", true);
        } else {
            label(draft.action.geographic_destination ? "Click a destination on the map." :
                  "Click the target sector on the map.", x, y + 71, 16, accent, width);
        }
        button(review_button(area, false), "Discard draft");
        return;
    }
    if (controls.cancel_prompt) {
        label("Cancel mission #" + std::to_string(controls.cancel_prompt->id) + "?", x, y, 21, ink, width);
        label("Resources already used remain spent.", x, y + 39, 15, muted, width);
        button(review_button(area, false), "Back");
        button(review_button(area, true), "Confirm cancellation", true);
        return;
    }
    if (const auto* marker = selected_marker(snapshot, controls)) {
        constexpr std::array kinds{"Formation", "Contact", "Carrier", "Installation", "Stockpile", "Mission"};
        label(marker->label, x, y, 21, ink, width);
        const auto identity = marker->kind == MarkerKind::installation && marker->estimated ? "Site report" :
            std::string(kinds.at(static_cast<std::size_t>(marker->kind))) + " #" + std::to_string(marker->id);
        label(identity + " / Sector " + std::to_string(static_cast<std::uint32_t>(marker->sector)),
              x, y + 32, 15, accent, width);
        label(marker->estimated ? "Sector-centre estimate" : "Geographic position", x, y + 58, 14, muted, width);
        label(position_text(marker->position, !marker->estimated), x, y + 78, 15, ink, width);
        label(marker->detail, x, y + 112, 15, ink, width);
        label(marker->movement, x, y + 135, 15, muted, width);
        if (marker->kind == MarkerKind::unit && session.permissions().faction == marker->faction) {
            const auto actions = session.actions_for(UnitId{marker->id});
            for (std::size_t i = 0; i < actions.size(); ++i) {
                button(action_button(area, i), action_label(actions[i].kind).c_str());
            }
            if (session.permissions().may_issue_orders && actions.empty()) {
                label("No orders available from this formation.", x, y + 159, 14, muted, width);
            }
        } else if (marker->kind == MarkerKind::mission && marker->faction &&
                   session.can_cancel(*marker->faction, MissionId{marker->id})) {
            button(cancel_mission_button(area), "Cancel mission");
        }
        if (!marker->inspection.empty()) {
            const auto visible = inspection_rows(area);
            const auto count = marker->inspection.size();
            const auto offset = std::min(controls.detail_offset, inspection_max_offset(*marker, area));
            if (count > visible) {
                label("Scroll for details  /  " + std::to_string(offset + 1) + "-" +
                      std::to_string(std::min(offset + visible, count)) + " of " + std::to_string(count),
                      x, y + 216, 13, muted, width);
            }
            for (auto i = offset; i < std::min(offset + visible, count); ++i) {
                label(marker->inspection[i], x, y + 236 + 18 * (i - offset), 15, ink, width);
            }
        }
        return;
    }
    if (const auto feature = selected_feature(controls)) {
        const auto details = geography.inspect(*feature, !controls.perspective, snapshot.roads);
        label(details.label, x, y, 21, ink, width);
        label(position_text(feature->position, feature->kind != FeatureKind::waterway), x, y + 32, 15, accent, width);
        y += 63;
        for (const auto& line : details.lines) {
            label(line, x, y, 15, ink, width);
            y += 23;
        }
        return;
    }
    if (!controls.selected) {
        label("Map details", x, y, 23, ink, width);
        label("Click a marker, feature, or sector.", x, y + 35, 16, muted, width);
        label("Click again to cycle overlapping items.", x, y + 57, 15, muted, width);
        label("Blue / Friendly    Red / Hostile", x, y + 96, 15, ink, width);
        label("Green / Neutral    Yellow / Unknown", x, y + 119, 15, ink, width);
        label("White / Observer labels W and E", x, y + 142, 15, ink, width);
        label("? / Report    T / Ground transport", x, y + 165, 15, ink, width);
        label("Top bar / Installation or stockpile", x, y + 188, 15, ink, width);
        label("Open diamond / Mission target", x, y + 211, 15, ink, width);
        return;
    }
    const auto id = static_cast<std::uint32_t>(*controls.selected);
    const auto& tile = snapshot.tiles[id];
    label("Sector " + std::to_string(id), x, y, 23, ink, width);
    y += 32;
    const auto owner = tile.controller ? std::string(faction_name(*tile.controller)) : "Unknown";
    label((tile.contested ? "Contested / " : "") + owner + " / " + std::string(terrain_name(tile.terrain)),
          x, y, 16, ink, width);
    y += 22;
    if (tile.observed_tick) {
        label(std::string(confidence_name(tile.confidence)) + " / last report tick " +
                  std::to_string(*tile.observed_tick), x, y, 15, muted, width);
    } else if (controls.perspective) {
        label("No territorial report", x, y, 15, muted, width);
    } else {
        label(std::to_string(snapshot.sector_size_m) + " m sector", x, y, 15, muted, width);
    }
    y += 27;
    std::size_t remaining = 0;
    auto skip = controls.detail_offset;
    for (const auto& marker : snapshot.markers) {
        if (marker.sector != controls.selected || !marker_visible(marker, controls)) {
            continue;
        }
        if (skip) {
            --skip;
            continue;
        }
        const auto height = marker.movement.empty() ? 39 : 57;
        if (y + height - 4 > area.y + area.height - 22) {
            ++remaining;
            continue;
        }
        label(marker.label, x, y, 16, ink, width);
        label(marker.detail, x, y + 18, 14, muted, width);
        if (!marker.movement.empty()) {
            label(marker.movement, x, y + 36, 14, muted, width);
        }
        y += height;
    }
    if (remaining) {
        label("Scroll for " + std::to_string(remaining) + " more", x, area.y + area.height - 22, 14, muted, width);
    }
}

void draw_events(const Snapshot& snapshot, const ViewControls& controls, Rectangle area)
{
    DrawRectangleRec(area, panel);
    label("Campaign events", area.x + 15, area.y + 14, 21, ink, area.width - 30);
    float y = area.y + 48;
    if (controls.feedback) {
        const auto color = controls.feedback->accepted ? accent : Color{255, 157, 164, 255};
        std::istringstream words(controls.feedback->message);
        std::string word, line;
        while (words >> word) {
            if (!line.empty() && MeasureText((line + " " + word).c_str(), 14) > area.width - 30) {
                label(line, area.x + 15, y, 14, color, area.width - 30);
                y += 19;
                line.clear();
            }
            line += line.empty() ? word : " " + word;
        }
        label(line, area.x + 15, y, 14, color, area.width - 30);
        y += 26;
    }
    if (snapshot.events.empty()) {
        label("No events reported", area.x + 15, y, 16, muted, area.width - 30);
        return;
    }
    auto i = snapshot.events.size() - std::min(snapshot.events.size(), controls.event_offset);
    while (i > 0 && y + 32 <= area.y + area.height) {
        const auto& event = snapshot.events[--i];
        label("Tick " + std::to_string(event.tick), area.x + 15, y, 13, accent, area.width - 30);
        label(event.description, area.x + 15, y + 15, 15, ink, area.width - 30);
        y += 37;
    }
}

void draw(const Snapshot& snapshot, const ViewControls& controls, const GeographicLayers& geography,
          const TerritoryLayer& control, const CommandSession& session, const Layout& screen, bool ended)
{
    BeginDrawing();
    ClearBackground(background);
    label("OpenWar", 16, 15, 27, ink, 170);
    label("Campaign viewer", 166, 22, 17, muted, 185);
    if (const auto faction = session.permissions().faction) {
        label(std::string(faction_name(*faction)) + " command", screen.perspective.front().x,
              23, 17, accent, 275);
    } else {
        button(screen.perspective[0], "Observer", !controls.perspective);
        button(screen.perspective[1], "WEST", controls.perspective == FactionId::west);
        button(screen.perspective[2], "EAST", controls.perspective == FactionId::east);
    }
    button(screen.play, ended ? "Ended" : controls.paused ? "Resume" : "Pause");
    button(screen.slower, "-");
    button(screen.faster, "+");
    label(std::to_string(controls.speed) + "x", 171, 74, 16, ink, 59);
    const auto clock = "Tick " + std::to_string(snapshot.tick) + "  /  " +
                       std::to_string(snapshot.elapsed_seconds) + " s  /  " +
                       std::string(war_status_name(snapshot.war.status));
    label(clock, 289, 74, 17, ink, GetScreenWidth() - 305);
    button(screen.fit, "Fit (R)");
    for (std::size_t i = 0; i < screen.layers.size(); ++i) {
        button(screen.layers[i], layer_names[i], controls.layers[i]);
    }
    draw_map(snapshot, controls, geography, control, screen.map);
    draw_details(snapshot, controls, geography, session, screen.details);
    draw_events(snapshot, controls, screen.events);
    const auto y = static_cast<int>(screen.map.y + screen.map.height + 11);
    const auto mouse = GetMousePosition();
    const auto point = GetScreenToWorld2D(mouse, controls.camera);
    if (CheckCollisionPointRec(mouse, screen.map) && sector_at(snapshot, point)) {
        const auto tile = geography.tile_at(point);
        const geography::Point coordinate{std::llround(point.x * 1000.0), std::llround(point.y * 1000.0),
                                          tile ? tile->elevation : 0};
        label(position_text(coordinate, tile.has_value()), 16, y, 14, ink, screen.map.width);
    }
    label("Wheel zoom  /  Right or middle drag to pan  /  R fits map", 16, y + 16, 13, muted, screen.map.width);
    if (controls.layers[territory]) {
        int x = static_cast<int>(screen.details.x);
        for (const auto& [name, color] : std::array{
                std::pair{"WEST", west}, std::pair{"EAST", east}, std::pair{"Neutral", neutral},
                std::pair{"Contested", contested}, std::pair{"Unknown", unknown}}) {
            DrawRectangle(x, y, 10, 10, color);
            DrawText(name, x + 13, y - 1, 12, muted);
            x += MeasureText(name, 12) + 19;
        }
    }
    if (geography.has_source_attribution()) {
        label("Map data © OpenStreetMap contributors", screen.details.x, y + 16, 12, muted, screen.details.width);
    }
    EndDrawing();
}

class Window {
public:
    Window()
    {
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        InitWindow(1160, 760, "OpenWar Campaign Viewer");
        if (!IsWindowReady()) {
            throw std::runtime_error("Could not open the viewer window");
        }
        SetWindowMinSize(800, 580);
        SetExitKey(KEY_NULL);
        SetTargetFPS(60);
    }

    ~Window() { CloseWindow(); }
};

void run(CampaignState& campaign, std::optional<FactionId> faction)
{
    using Clock = std::chrono::steady_clock;
    const Window window;
    const GeographicLayers geography(campaign.geographic_world(), campaign.world());
    TerritoryLayer territory_layer;
    ViewControls controls;
    controls.perspective = faction;
    CommandSession session(campaign, faction);
    auto snapshot = session.snapshot(controls.perspective);
    territory_layer.update(snapshot);
    fit_map(controls, snapshot, layout().map);
    auto previous = Clock::now();
    const auto initial_tick = campaign.clock().tick();
    const auto tick_seconds = campaign.clock().tick_seconds();
    const auto remaining_ticks = std::numeric_limits<std::uint64_t>::max() / tick_seconds - initial_tick;
    std::chrono::duration<long double> simulated{};
    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_ESCAPE)) {
            if (!controls.draft && !controls.cancel_prompt) {
                break;
            }
            controls.draft.reset();
            controls.cancel_prompt.reset();
            controls.feedback.reset();
        }
        const auto now = Clock::now();
        if (!controls.paused && !campaign.war_ended()) {
            simulated += std::chrono::duration<long double>(now - previous) * controls.speed;
            const auto due = std::min(remaining_ticks, cli::ticks_due(simulated, 1, tick_seconds));
            const auto advanced = campaign.clock().tick() - initial_tick;
            if (due > advanced) {
                campaign.advance_ticks(std::min<std::uint64_t>(due - advanced, 256));
            }
        }
        previous = now;
        const auto screen = layout();
        const auto old_perspective = controls.perspective;
        handle_playback(controls, screen, campaign.war_ended(), !faction);
        if (snapshot.tick != campaign.clock().tick() || old_perspective != controls.perspective) {
            snapshot = session.snapshot(controls.perspective);
            territory_layer.update(snapshot);
            controls.event_offset = std::min(controls.event_offset,
                snapshot.events.empty() ? std::size_t{0} : snapshot.events.size() - 1);
            if (controls.selected && !controls.object) {
                const auto count = visible_marker_count(snapshot, controls);
                controls.detail_offset = std::min(controls.detail_offset,
                    static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, count - 1)));
            }
        }
        if (controls.object && !selected_marker(snapshot, controls) && !selected_feature(controls)) {
            controls.object.reset();
            controls.detail_offset = 0;
        }
        handle_map_input(controls, snapshot, geography, screen);
        if (handle_command_input(controls, snapshot, screen.details, session)) {
            snapshot = session.snapshot(controls.perspective);
            controls.event_offset = 0;
        }
        draw(snapshot, controls, geography, territory_layer, session, screen, campaign.war_ended());
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    try {
        std::optional<std::uint64_t> seed_override;
        std::optional<std::filesystem::path> scenario_path;
        std::optional<std::filesystem::path> load_path;
        std::optional<std::filesystem::path> save_path;
        std::optional<std::string_view> demo;
        std::optional<FactionId> faction;
        const auto select = [&](std::string_view selection) {
            if (demo && demo != selection) {
                throw std::invalid_argument("Select one demonstration scenario");
            }
            demo = selection;
        };
        for (int i = 1; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--help") {
                std::cout << "Usage: openwar-gui [--demo NAME | --scenario FILE | --load FILE] "
                             "[--seed N] [--faction WEST|EAST] [--save FILE]\n"
                             "Demos: combat, operations, campaign, procedural.\n"
                             "Use --load to resume and --save to write a checkpoint when the window closes.\n";
                return 0;
            }
            if ((option != "--seed" && option != "--demo" && option != "--scenario" &&
                option != "--load" && option != "--save" && option != "--faction") || i + 1 == argc) {
                throw std::invalid_argument("Expected --demo, --scenario, --seed, --load, --save or --faction with a value");
            }
            const std::string_view value = argv[++i];
            if (option == "--faction") {
                if (faction) {
                    throw std::invalid_argument("Specify one command faction");
                }
                if (value == "WEST" || value == "west") {
                    faction = FactionId::west;
                } else if (value == "EAST" || value == "east") {
                    faction = FactionId::east;
                } else {
                    throw std::invalid_argument("Faction must be WEST or EAST");
                }
            } else if (option == "--demo") {
                if (value != "combat" && value != "operations" && value != "campaign" && value != "procedural") {
                    throw std::invalid_argument("Unknown viewer demonstration");
                }
                select(value);
            } else if (option == "--scenario") {
                select("procedural");
                if (scenario_path) {
                    throw std::invalid_argument("Select one scenario file");
                }
                scenario_path = value;
            } else if (option == "--load" || option == "--save") {
                auto& path = option == "--load" ? load_path : save_path;
                if (path) {
                    throw std::invalid_argument("Specify " + std::string(option) + " only once");
                }
                path = value;
            } else {
                std::uint64_t seed = 0;
                const auto end = value.data() + value.size();
                const auto result = std::from_chars(value.data(), end, seed);
                if (result.ec != std::errc{} || result.ptr != end) {
                    throw std::invalid_argument("Seed must be an unsigned integer");
                }
                seed_override = seed;
            }
        }
        if (load_path && (demo || seed_override)) {
            throw std::invalid_argument("--load cannot be combined with --demo, --scenario or --seed");
        }
        auto campaign = [&] {
            if (load_path) {
                return CampaignState::load(*load_path);
            }
            if (scenario_path && is_geographic_scenario_file(*scenario_path)) {
                const auto seed = seed_override.value_or(0);
                if (seed > UINT32_MAX)
                    throw std::out_of_range("Seed must fit in 32 bits");
                return CampaignState(read_geographic_scenario(*scenario_path, faction), static_cast<std::uint32_t>(seed));
            }
            auto settings = scenario_path ? read_procedural_settings(*scenario_path) : ProceduralSettings{};
            settings.seed = seed_override.value_or(settings.seed);
            const auto scenario = demo.value_or("combat");
            if (scenario != "procedural" && settings.seed > UINT32_MAX) {
                throw std::invalid_argument("Seed must fit in 32 bits");
            }
            const auto seed = static_cast<std::uint32_t>(settings.seed);
            if (scenario == "procedural") {
                return procedural_campaign(settings);
            }
            if (scenario == "campaign") {
                return long_example_campaign(seed);
            }
            if (scenario == "combat") {
                return CampaignState(combat_example_scenario(faction), seed);
            }
            return operations_example_campaign(seed, faction);
        }();
        if (faction && (!campaign.has_operational_view() || campaign.player_faction() != faction)) {
            throw std::invalid_argument("Campaign does not grant command authority to that faction");
        }
        run(campaign, faction);
        if (save_path) {
            campaign.save(*save_path);
        }
    } catch (const std::exception& error) {
        std::cerr << "openwar-gui: " << error.what() << '\n';
        return 1;
    }
}
