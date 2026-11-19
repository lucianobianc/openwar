#include "command.hpp"
#include "launcher.hpp"
#include "launcher_model.hpp"
#include "viewer_geography.hpp"
#include "viewer_model.hpp"
#include "viewer_symbols.hpp"
#include "realtime.hpp"
#include "scenario.hpp"

#include <raylib.h>

#include <algorithm>
#include <charconv>
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

enum Layer { sectors, territory, units, carriers, roads, hydrography, sites, routes, layer_count };
constexpr std::array layer_names{"Sectors", "Control", "Units", "Carriers", "Roads", "Water", "Sites", "Routes"};
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
    Rectangle missions;
    Rectangle supplies;
    Rectangle save;
    Rectangle menu;
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
                  {16, 65, 98, 32}, {126, 65, 37, 32}, {230, 65, 37, 32}, {16, 108, 73, 31}, {366, 19, 124, 32},
                  {278, 65, 118, 32},
                  {width - 213, 65, 104, 32}, {width - 101, 65, 85, 32}, {}};
    for (std::size_t i = 0; i < screen.layers.size(); ++i) {
        screen.layers[i] = {100 + static_cast<float>(i) * 87, 108, 81, 31};
    }
    return screen;
}

struct OrderDraft {
    OrderAction action;
    UnitId selected_formation;
    std::string selected_name;
    bool automatic = false;
    std::optional<OrderTarget> target{};
    geography::Point pin{};
    SectorId sector{};
    std::optional<OrderPreview> preview{};
    std::vector<geography::Point> via{};

    std::optional<UnitId> requested_formation() const
    {
        return automatic ? std::nullopt : std::optional{selected_formation};
    }
};

struct CommandFeedback {
    std::string message;
    bool accepted;
};

struct SupplyDraft {
    StockpileId destination;
    Resource resource = Resource::supplies;
    std::string quantity = "1";
    std::optional<SupplyOrderPreview> preview{};
    std::uint64_t preview_tick = 0;

    std::uint64_t amount() const
    {
        std::uint64_t value{};
        const auto [end, error] = std::from_chars(quantity.data(), quantity.data() + quantity.size(), value);
        return error == std::errc{} && end == quantity.data() + quantity.size() ? value : 0;
    }
};

struct SupplyPanel {
    bool open = false;
    std::optional<SupplyDraft> draft{};
    std::optional<ShipmentId> selected{};
    bool confirm_cancel = false;
    std::size_t offset = 0;
};

struct ViewControls {
    std::optional<FactionId> perspective;
    std::optional<SectorId> selected;
    std::optional<Selection> object;
    std::optional<OrderDraft> draft;
    std::optional<MarkerKey> cancel_prompt;
    std::optional<CommandFeedback> feedback;
    SupplyPanel supply;
    std::array<bool, layer_count> layers{false, true, true, true, true, true, true, true};
    bool show_missions = false;
    std::size_t mission_offset = 0;
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

float wrapped_label(const std::string& text, float x, float y, int size, Color color, float width)
{
    std::istringstream words(text);
    std::string word, line;
    while (words >> word) {
        if (!line.empty() && MeasureText((line + " " + word).c_str(), size) > width) {
            label(line, x, y, size, color, width);
            y += size + 4;
            line.clear();
        }
        line += line.empty() ? word : " " + word;
    }
    label(line, x, y, size, color, width);
    return y + size + 4;
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

bool active_mission_marker(const Marker& marker)
{
    return marker.mission_status && mission_active(*marker.mission_status);
}

bool marker_visible(const Marker& marker, const ViewControls& controls)
{
    switch (marker.kind) {
    case MarkerKind::mission: return controls.layers[units] && active_mission_marker(marker);
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
        if (marker != snapshot.markers.end() && (marker->kind == MarkerKind::mission || marker_visible(*marker, controls))) {
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
    controls.show_missions = false;
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
            controls.mission_offset = 0;
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
    if (draft.action.geographic_destination && draft.target &&
        (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))) {
        if (draft.via.size() + 1 == max_ground_waypoints) {
            controls.feedback = CommandFeedback{"A route allows at most 16 points", false};
            return;
        }
        draft.via.push_back(draft.pin);
    }
    draft.sector = sector;
    draft.preview.reset();
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

float inspection_top(const Marker& marker)
{
    return marker.kind == MarkerKind::unit ? 276.f : 236.f;
}

std::size_t inspection_rows(const Marker& marker, Rectangle area)
{
    return static_cast<std::size_t>(std::max(1.f, (area.height - inspection_top(marker) - 12) / 18));
}

std::size_t inspection_max_offset(const Marker& marker, Rectangle area)
{
    const auto visible = inspection_rows(marker, area);
    return marker.inspection.size() > visible ? marker.inspection.size() - visible : 0;
}

std::size_t mission_rows(Rectangle area)
{
    return static_cast<std::size_t>(std::max(1.f, (area.height - 74) / 31));
}

Rectangle mission_row(Rectangle area, std::size_t row)
{
    return {area.x + 15, area.y + 65 + static_cast<float>(row) * 31, area.width - 30, 28};
}

std::size_t mission_max_offset(const Snapshot& snapshot, Rectangle area)
{
    const auto visible = mission_rows(area);
    return snapshot.mission_markers.size() > visible ? snapshot.mission_markers.size() - visible : 0;
}

bool handle_mission_list(ViewControls& controls, const Snapshot& snapshot, const Layout& screen)
{
    if (controls.draft || controls.cancel_prompt || controls.supply.draft || controls.supply.confirm_cancel) {
        return false;
    }
    if (clicked(screen.missions) || IsKeyPressed(KEY_M)) {
        controls.supply.open = false;
        controls.show_missions = !controls.show_missions;
        return true;
    }
    if (!controls.show_missions) {
        return false;
    }
    const auto max_offset = static_cast<std::ptrdiff_t>(mission_max_offset(snapshot, screen.details));
    const auto wheel = CheckCollisionPointRec(GetMousePosition(), screen.details) ? GetMouseWheelMove() : 0.f;
    const auto step = wheel > 0 ? -1 : wheel < 0 ? 1 : 0;
    controls.mission_offset = static_cast<std::size_t>(std::clamp(
        static_cast<std::ptrdiff_t>(controls.mission_offset) + step, std::ptrdiff_t{0}, max_offset));
    const auto visible = mission_rows(screen.details);
    for (std::size_t row = 0; row < visible && controls.mission_offset + row < snapshot.mission_markers.size(); ++row) {
        if (!clicked(mission_row(screen.details, row))) {
            continue;
        }
        const auto& marker = snapshot.markers[snapshot.mission_markers[controls.mission_offset + row]];
        controls.show_missions = false;
        controls.selected = marker.sector;
        controls.object = marker.key();
        controls.detail_offset = 0;
        controls.camera.target = map_position(marker.position);
        return true;
    }
    return false;
}

void handle_map_input(ViewControls& controls, const Snapshot& snapshot, const GeographicLayers& geography,
                      const Layout& screen)
{
    if (controls.draft && !controls.draft->via.empty() && IsKeyPressed(KEY_BACKSPACE)) {
        auto& draft = *controls.draft;
        draft.pin = draft.via.back();
        draft.via.pop_back();
        draft.target = draft.pin;
        const auto size = std::int64_t{snapshot.sector_size_m} * 1000;
        draft.sector = SectorId{static_cast<std::uint32_t>(
            (draft.pin.south / size) * snapshot.width + draft.pin.east / size)};
        draft.preview.reset();
        controls.feedback.reset();
    }
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
    if (!controls.show_missions && !controls.supply.open && CheckCollisionPointRec(mouse, screen.details) && wheel != 0) {
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
            } else if (!controls.supply.draft && !controls.supply.confirm_cancel) {
                controls.cancel_prompt.reset();
                controls.supply.selected.reset();
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

Rectangle assignment_button(Rectangle panel)
{
    return {panel.x + 173, panel.y + 165, 159, 24};
}

Rectangle current_position_button(Rectangle panel)
{
    return {panel.x + 15, panel.y + 103, 175, 29};
}

Rectangle cancel_mission_button(Rectangle area)
{
    return {area.x + 15, area.y + 169, 146, 29};
}

Rectangle new_supply_button(Rectangle area)
{
    return {area.x + 170, area.y + 27, 163, 30};
}

Rectangle supply_resource_button(Rectangle area, bool next)
{
    return {area.x + (next ? 296 : 15), area.y + 60, 36, 27};
}

constexpr std::array supply_resources{Resource::crude_oil, Resource::fuel, Resource::materials,
    Resource::spare_parts, Resource::supplies, Resource::ammunition};

const SupplyOrderSummary* selected_supply(const Snapshot& snapshot, const SupplyPanel& panel)
{
    const auto found = std::ranges::find_if(snapshot.supply_requests, [&](const auto& order) {
        return order.request.id == panel.selected;
    });
    return found == snapshot.supply_requests.end() ? nullptr : &*found;
}

bool handle_supply_input(ViewControls& controls, const Snapshot& snapshot, const Layout& screen, CommandSession& session)
{
    auto& supply = controls.supply;
    const auto area = screen.details;
    if (!controls.draft && !controls.cancel_prompt && !supply.draft && !supply.confirm_cancel &&
        (clicked(screen.supplies) || IsKeyPressed(KEY_L))) {
        supply.open = !supply.open;
        controls.show_missions = false;
        controls.feedback.reset();
    }
    if (!supply.open) {
        return false;
    }
    if (supply.draft) {
        if (clicked(review_button(area, false))) {
            supply.draft.reset();
            controls.feedback.reset();
            return false;
        }
        auto& draft = *supply.draft;
        const auto delta = clicked(supply_resource_button(area, true)) - clicked(supply_resource_button(area, false));
        if (delta) {
            const auto index = std::ranges::find(supply_resources, draft.resource) - supply_resources.begin();
            const auto count = static_cast<std::ptrdiff_t>(supply_resources.size());
            draft.resource = supply_resources[(index + delta + count) % count];
            draft.preview.reset();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !draft.quantity.empty()) {
            draft.quantity.pop_back();
            draft.preview.reset();
        }
        for (int digit = GetCharPressed(); digit; digit = GetCharPressed()) {
            if (digit >= '0' && digit <= '9' && draft.quantity.size() < 20) {
                draft.quantity.push_back(static_cast<char>(digit));
                draft.preview.reset();
            }
        }
        if (!draft.preview || draft.preview_tick != snapshot.tick) {
            draft.preview = session.preview_supply(draft.destination, draft.resource, draft.amount());
            draft.preview_tick = snapshot.tick;
        }
        if (draft.preview->source && clicked(review_button(area, true))) {
            const auto result = session.submit_supply(draft.destination, draft.resource, draft.amount());
            controls.feedback = CommandFeedback{std::string(result.reason), result.shipment.has_value()};
            if (result.shipment) {
                supply.selected = result.shipment;
                supply.draft.reset();
                return true;
            }
            draft.preview.reset();
        }
        return false;
    }
    if (const auto* order = selected_supply(snapshot, supply)) {
        if (supply.confirm_cancel) {
            if (clicked(review_button(area, false))) {
                supply.confirm_cancel = false;
            } else if (clicked(review_button(area, true))) {
                const auto result = session.cancel_supply(order->request.id);
                supply.confirm_cancel = false;
                controls.feedback = CommandFeedback{std::string(result.reason), result.cancelled};
                return result.cancelled;
            }
        } else if (clicked(new_supply_button(area))) {
            supply.selected.reset();
        } else if (clicked(cancel_mission_button(area)) && session.can_cancel_supply(order->request.id)) {
            supply.confirm_cancel = true;
        }
        return false;
    }
    if (const auto* marker = selected_marker(snapshot, controls); marker && marker->supply_stock &&
        session.permissions().may_issue_orders && session.permissions().faction == marker->faction &&
        clicked(new_supply_button(area))) {
        supply.draft = SupplyDraft{*marker->supply_stock};
        controls.feedback.reset();
        return false;
    }
    const auto size = snapshot.supply_requests.size();
    const auto visible = mission_rows(area);
    const auto wheel = CheckCollisionPointRec(GetMousePosition(), area) ? GetMouseWheelMove() : 0.f;
    const auto last = size > visible ? size - visible : 0;
    supply.offset = static_cast<std::size_t>(std::clamp(static_cast<std::ptrdiff_t>(supply.offset) +
        (wheel > 0 ? -1 : wheel < 0 ? 1 : 0), std::ptrdiff_t{0}, static_cast<std::ptrdiff_t>(last)));
    for (std::size_t row = 0; row < visible && supply.offset + row < size; ++row) {
        if (clicked(mission_row(area, row))) {
            supply.selected = snapshot.supply_requests[size - 1 - supply.offset - row].request.id;
            controls.feedback.reset();
            break;
        }
    }
    return false;
}

void draw_supply_panel(const Snapshot& snapshot, const ViewControls& controls, const CommandSession& session,
                       Rectangle area)
{
    const auto& supply = controls.supply;
    const float x = area.x + 15;
    const float y = area.y + 14;
    const float width = area.width - 30;
    if (supply.draft) {
        const auto& draft = *supply.draft;
        label("Supply request", x, y, 21, ink, width);
        label("Destination stockpile #" + std::to_string(static_cast<std::uint32_t>(draft.destination)),
              x, y + 29, 15, accent, width);
        button(supply_resource_button(area, false), "<");
        button(supply_resource_button(area, true), ">");
        label(std::string(resource_name(draft.resource)), x + 46, y + 52, 16, ink, width - 92);
        DrawRectangleLinesEx({x, y + 86, width, 29}, 1, accent);
        label("Quantity " + draft.quantity + "_", x + 8, y + 93, 16, ink, width - 16);
        if (draft.preview) {
            const auto& preview = *draft.preview;
            if (preview.destination) {
                label("Space " + std::to_string(preview.room) + " / Source stock " +
                      std::to_string(preview.source_available), x, y + 121, 15, ink, width);
            }
            wrapped_label(std::string(preview.reason), x, y + 145, 14, preview.source ? accent : muted, width);
            if (preview.source) {
                button(review_button(area, true), "Confirm request", true);
                label("Source #" + std::to_string(static_cast<std::uint32_t>(*preview.source)) +
                      " / Idle carrier capacity " + std::to_string(preview.idle_capacity), x, y + 222, 14, muted, width);
                label("Dispatch checks routes and fuel.", x, y + 242, 14, muted, width);
            }
        }
        button(review_button(area, false), "Discard draft");
        return;
    }
    if (const auto* order = selected_supply(snapshot, supply)) {
        const auto& request = order->request;
        label("Shipment #" + std::to_string(static_cast<std::uint32_t>(request.id)), x, y, 21, ink, width);
        if (supply.confirm_cancel) {
            label("Cancel this supply request?", x, y + 35, 16, accent, width);
            wrapped_label("Fuel already spent stays spent. Loaded cargo stays with the carrier.",
                          x, y + 62, 15, muted, width);
            button(review_button(area, false), "Back");
            button(review_button(area, true), "Confirm cancellation", true);
            return;
        }
        button(new_supply_button(area), "Back to requests");
        label(std::string(faction_name(order->requester)) + " / " + std::string(shipment_status_name(order->status)),
              x, y + 58, 16, accent, width);
        label(std::to_string(request.quantity) + " " + std::string(resource_name(request.resource)),
              x, y + 82, 16, ink, width);
        label("Source stockpile #" + std::to_string(static_cast<std::uint32_t>(request.origin)),
              x, y + 104, 15, muted, width);
        label("Destination stockpile #" + std::to_string(static_cast<std::uint32_t>(request.destination)),
              x, y + 122, 15, muted, width);
        if (order->carrier) {
            label("Carrier #" + std::to_string(static_cast<std::uint32_t>(*order->carrier)) +
                  " / Cargo " + std::to_string(order->cargo), x, y + 142, 15, ink, width);
        }
        if (session.can_cancel_supply(request.id)) {
            button(cancel_mission_button(area), "Cancel request");
        }
        return;
    }
    label("Supply requests", x, y, 21, ink, width);
    const auto* marker = selected_marker(snapshot, controls);
    if (marker && marker->supply_stock && session.permissions().may_issue_orders &&
        session.permissions().faction == marker->faction) {
        button(new_supply_button(area), "Request supply");
        label(marker->label, x, y + 29, 14, muted, 150);
    } else if (session.permissions().may_issue_orders) {
        label("Select a friendly stockpile or formation.", x, y + 29, 14, muted, width);
    }
    const auto size = snapshot.supply_requests.size();
    for (std::size_t row = 0; row < mission_rows(area) && supply.offset + row < size; ++row) {
        const auto& order = snapshot.supply_requests[size - 1 - supply.offset - row];
        const auto bounds = mission_row(area, row);
        DrawRectangleRec(bounds, CheckCollisionPointRec(GetMousePosition(), bounds) ? border : background);
        label("#" + std::to_string(static_cast<std::uint32_t>(order.request.id)) + " / " +
              std::string(resource_name(order.request.resource)) + " / " + std::string(shipment_status_name(order.status)),
              bounds.x + 7, bounds.y + 6, 15, ink, bounds.width - 14);
    }
    if (size == 0) {
        label("No player supply requests.", x, y + 69, 15, muted, width);
    }
}

std::string action_label(MissionKind kind)
{
    switch (kind) {
    case MissionKind::recon: return "Recon";
    case MissionKind::patrol: return "Patrol";
    case MissionKind::defend: return "Defend";
    case MissionKind::reposition: return "Reposition";
    case MissionKind::attack: return "Attack";
    case MissionKind::move: return "Move";
    case MissionKind::hold: return "Hold";
    case MissionKind::ground_recon: return "Ground Recon";
    case MissionKind::ground_patrol: return "Ground Patrol";
    }
    return {};
}

bool handle_command_input(ViewControls& controls, const Snapshot& snapshot, Rectangle panel,
                          CommandSession& session)
{
    if (controls.show_missions || controls.supply.open || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        return false;
    }
    if (controls.draft || controls.cancel_prompt) {
        if (clicked(review_button(panel, false))) {
            controls.draft.reset();
            controls.cancel_prompt.reset();
            controls.feedback.reset();
            return false;
        }
        if (controls.draft && clicked(assignment_button(panel))) {
            auto& draft = *controls.draft;
            draft.automatic = !draft.automatic;
            draft.preview.reset();
            controls.feedback.reset();
            return false;
        }
        if (controls.draft && !controls.draft->target && controls.draft->action.kind == MissionKind::hold &&
            clicked(current_position_button(panel))) {
            if (const auto* marker = selected_marker(snapshot, controls); marker && !marker->estimated) {
                auto& draft = *controls.draft;
                draft.sector = marker->sector;
                draft.pin = marker->position;
                draft.target = draft.pin;
                controls.feedback.reset();
            }
            return false;
        }
        if (!clicked(review_button(panel, true))) {
            return false;
        }
        if (controls.cancel_prompt) {
            const auto key = *controls.cancel_prompt;
            const auto* marker = selected_marker(snapshot, controls);
            const bool holding = marker && marker->mission_status == MissionStatus::holding;
            const auto result = session.cancel(*key.faction, MissionId{key.id});
            controls.cancel_prompt.reset();
            controls.feedback = CommandFeedback{holding && result.cancelled ? "Holding position released" :
                                                std::string(result.reason), result.cancelled};
            return result.cancelled;
        }
        if (!controls.draft->target) {
            return false;
        }
        auto& draft = *controls.draft;
        const auto result = session.submit(draft.action.kind, *draft.target, draft.requested_formation(), draft.via);
        if (!result.mission) {
            draft.preview.reset();
            controls.feedback = CommandFeedback{std::string(result.reason), false};
            return false;
        }
        const auto id = static_cast<std::uint32_t>(*result.mission);
        controls.feedback = CommandFeedback{"Order #" + std::to_string(id) + " " + std::string(result.reason), true};
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
                    controls.draft = OrderDraft{actions[i], UnitId{marker->id}, marker->label};
                    controls.feedback.reset();
                    return false;
                }
            }
            const auto reserve = session.operational_reserve(UnitId{marker->id});
            if (reserve && clicked(action_button(panel, actions.size()))) {
                const bool changed = session.set_operational_reserve(UnitId{marker->id}, !*reserve);
                controls.feedback = CommandFeedback{
                    changed ? (*reserve ? "Formation released from operational reserve" :
                                          "Formation placed in operational reserve") :
                              "Formation reserve status could not be changed", changed};
                return changed;
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

constexpr Color route_preview{255, 207, 103, 255};
constexpr Color route_travelled{168, 184, 196, 255};
constexpr Color route_carrier{150, 204, 255, 255};

void draw_route_segment(Vector2 from, Vector2 to, Rectangle viewport, Color color, float thickness,
                        bool dashed, float& phase)
{
    const Vector2 delta{to.x - from.x, to.y - from.y};
    if (delta.x == 0 && delta.y == 0) {
        return;
    }
    constexpr float dash_period = 14;
    const float previous_phase = phase;
    const float length = dashed ? std::hypot(delta.x, delta.y) : 0;
    if (dashed) {
        phase = std::fmod(phase + length, dash_period);
    }
    float entry = 0;
    float exit = 1;
    const auto clip_axis = [&](float start, float step, float low, float high) {
        if (step == 0) {
            return start >= low && start <= high;
        }
        const auto edge_a = (low - start) / step;
        const auto edge_b = (high - start) / step;
        entry = std::max(entry, std::min(edge_a, edge_b));
        exit = std::min(exit, std::max(edge_a, edge_b));
        return entry <= exit;
    };
    if (!clip_axis(from.x, delta.x, viewport.x, viewport.x + viewport.width) ||
        !clip_axis(from.y, delta.y, viewport.y, viewport.y + viewport.height)) {
        return;
    }
    const auto point = [&](float progress) {
        return Vector2{from.x + delta.x * progress, from.y + delta.y * progress};
    };
    if (!dashed) {
        DrawLineEx(point(entry), point(exit), thickness, color);
        return;
    }
    const float start = entry * length;
    const float end = exit * length;
    for (float dash = start - std::fmod(previous_phase + start, dash_period); dash < end; dash += dash_period) {
        const auto first = std::max(dash, start);
        const auto last = std::min(dash + 8, end);
        if (first < last) {
            DrawLineEx(point(first / length), point(last / length), thickness, color);
        }
    }
}

void draw_route_trace(const RouteTrace& trace, const ViewControls& controls, Rectangle viewport,
                      Color remaining, bool dashed, bool selected)
{
    const auto screen = [&](geography::Point point) {
        return GetWorldToScreen2D(map_position(point), controls.camera);
    };
    const auto progress = screen(trace.position);
    const float thickness = selected ? 3.f : 2.f;
    float phase = 0;
    for (std::size_t leg = 0; leg + 1 < trace.vertices.size(); ++leg) {
        const auto from = screen(trace.vertices[leg]);
        const auto to = screen(trace.vertices[leg + 1]);
        if (leg < trace.leg) {
            draw_route_segment(from, to, viewport, route_travelled, thickness, false, phase);
        } else {
            if (leg == trace.leg) {
                draw_route_segment(from, progress, viewport, route_travelled, thickness, false, phase);
            }
            draw_route_segment(leg == trace.leg ? progress : from, to, viewport, remaining, thickness, dashed, phase);
        }
    }
    const auto destination = screen(trace.destination);
    if (CheckCollisionPointRec(destination, viewport)) {
        DrawRectangleLinesEx({destination.x - 5, destination.y - 5, 10, 10}, 1.5f, remaining);
    }
}

void draw_journeys(const Snapshot& snapshot, const ViewControls& controls, Rectangle viewport)
{
    if (!controls.layers[routes]) {
        return;
    }
    const auto* selected = selected_marker(snapshot, controls);
    for (const auto& marker : snapshot.markers) {
        if (!marker.journey || &marker == selected || !marker_visible(marker, controls)) {
            continue;
        }
        const auto color = marker.kind == MarkerKind::carrier ? route_carrier : accent;
        draw_route_trace(*marker.journey, controls, viewport, color, false, false);
    }
    if (selected && selected->journey) {
        const bool historical = selected->kind == MarkerKind::mission && !active_mission_marker(*selected);
        const auto color = selected->kind == MarkerKind::carrier ? route_carrier : historical ? muted : accent;
        draw_route_trace(*selected->journey, controls, viewport, color, historical, true);
    }
    if (controls.draft && controls.draft->preview && controls.draft->preview->route) {
        draw_route_trace(*controls.draft->preview->route, controls, viewport, route_preview, true, true);
    }
}

Color mission_color(MissionStatus status)
{
    switch (status) {
    case MissionStatus::scheduled:
    case MissionStatus::preparing: return route_carrier;
    case MissionStatus::in_progress:
    case MissionStatus::observing:
    case MissionStatus::holding: return accent;
    case MissionStatus::completed: return {169, 224, 159, 255};
    case MissionStatus::failed: return {255, 157, 164, 255};
    case MissionStatus::cancelled:
    case MissionStatus::released: return muted;
    }
    return muted;
}

void draw_marker(const Marker& marker, Vector2 at, bool selected)
{
    if (marker.symbol) {
        draw_military_symbol(marker, at);
    } else if (marker.mission_status) {
        const auto color = mission_color(*marker.mission_status);
        DrawLineV({at.x, at.y - 12}, {at.x + 12, at.y}, color);
        DrawLineV({at.x + 12, at.y}, {at.x, at.y + 12}, color);
        DrawLineV({at.x, at.y + 12}, {at.x - 12, at.y}, color);
        DrawLineV({at.x - 12, at.y}, {at.x, at.y - 12}, color);
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
    draw_journeys(snapshot, controls, map);
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
        for (std::size_t i = 0; i < controls.draft->via.size(); ++i) {
            const auto at = GetWorldToScreen2D(map_position(controls.draft->via[i]), controls.camera);
            DrawCircleLines(std::lround(at.x), std::lround(at.y), 7, route_preview);
            DrawText(std::to_string(i + 1).c_str(), std::lround(at.x) + 9, std::lround(at.y) - 6, 14, route_preview);
        }
        const auto pin = GetWorldToScreen2D(map_position(controls.draft->pin), controls.camera);
        DrawCircleLines(std::lround(pin.x), std::lround(pin.y), 10, route_preview);
        DrawLineV({pin.x - 14, pin.y}, {pin.x + 14, pin.y}, route_preview);
        DrawLineV({pin.x, pin.y - 14}, {pin.x, pin.y + 14}, route_preview);
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
    if (controls.supply.open) {
        draw_supply_panel(snapshot, controls, session, area);
        return;
    }
    const float x = area.x + 15;
    float y = area.y + 14;
    const float width = area.width - 30;
    if (controls.draft) {
        const auto& draft = *controls.draft;
        label(action_label(draft.action.kind) + " order draft", x, y, 21, ink, width);
        const auto assignment = draft.automatic ? "Automatic assignment on confirmation" :
            "Explicit #" + std::to_string(static_cast<std::uint32_t>(draft.selected_formation)) + " / " + draft.selected_name;
        label(assignment, x, y + 29, 14, muted, width);
        if (draft.target) {
            const auto id = static_cast<std::uint32_t>(draft.sector);
            const auto& tile = snapshot.tiles.at(id);
            const auto owner = tile.controller ? std::string(faction_name(*tile.controller)) : "Unknown";
            label("Target sector " + std::to_string(id) + " / Report " + owner, x, y + 51, 15, accent, width);
            if (draft.action.geographic_destination) {
                label(position_text(draft.pin, geography.has_source_attribution()), x, y + 71, 15, ink, width);
            } else {
                label("Sector objective", x, y + 71, 15, ink, width);
            }
            if (draft.preview) {
                const auto& preview = *draft.preview;
                if (preview.formation) {
                    label(draft.automatic ? "Candidate / " + preview.formation_name : "Selected formation eligible",
                          x, y + 93, 15, accent, width);
                    if (preview.route) {
                        const auto us = preview.route->duration_us;
                        const auto seconds = us / 1'000'000 + (us % 1'000'000 != 0);
                        label("Travel " + std::to_string(seconds) + " s / " + std::to_string(preview.route->fuel_ml) +
                              " mL estimated", x, y + 114, 15, ink, width);
                        label("Uses faction reports. Conditions may change.", x, y + 139, 14, muted, width);
                        label("Shift-click adds a point. Backspace undoes.", x, y + 227, 14, muted, width);
                        label(draft.action.kind == MissionKind::attack ? "Requires supplies and ammunition." :
                              "Requires supplies.", x, y + 248, 14, muted, width);
                        if (position_order(draft.action.kind)) {
                            label(draft.action.kind == MissionKind::hold ? "Friendly destination. Holds until released." :
                                  "Friendly or neutral destination. No capture.", x, y + 262, 14, muted, width);
                        }
                        if (draft.action.kind == MissionKind::ground_recon) {
                            label("One observation tick. Forest limits sighting.", x, y + 262, 14, muted, width);
                        }
                    } else if (preview.abstract_duration_seconds) {
                        label("Duration " + std::to_string(*preview.abstract_duration_seconds) + " s after launch",
                              x, y + 114, 15, ink, width);
                        label("Flight route and travel ETA unavailable.", x, y + 139, 14, muted, width);
                        const auto fuel = preview.launch_reservation[static_cast<std::size_t>(Resource::fuel)];
                        label("Fuel commitment " + std::to_string(fuel) + " L", x, y + 227, 14, muted, width);
                        std::string extra;
                        for (std::size_t i = 0; i < preview.launch_reservation.size(); ++i) {
                            const auto resource = static_cast<Resource>(i);
                            const auto amount = preview.launch_reservation[i];
                            if (amount && resource != Resource::fuel) {
                                extra += (extra.empty() ? "Also reserves " : ", ") + std::to_string(amount) + " " +
                                    std::string(resource_name(resource));
                            }
                        }
                        if (!extra.empty()) {
                            wrapped_label(extra, x, y + 248, 14, muted, width);
                        }
                    }
                } else {
                    wrapped_label("Unavailable / " + std::string(preview.reason), x, y + 99, 14, route_preview, width);
                    label("Revise the draft or discard it.", x, y + 139, 14, muted, width);
                }
                label("Preview at tick " + std::to_string(preview.tick), x, y + 159, 13, muted, width);
            }
            button(review_button(area, true), "Confirm order", true);
            if (!draft.via.empty()) {
                label("Route points " + std::to_string(draft.via.size() + 1), x, y + 178, 13, accent, width);
            }
        } else {
            label(draft.action.geographic_destination ? "Click a destination on the map." :
                  "Click the target sector on the map.", x, y + 71, 16, accent, width);
            if (draft.action.kind == MissionKind::hold) {
                button(current_position_button(area), "Use current position");
            }
        }
        button(assignment_button(area), draft.automatic ? "Use selected" : "Use automatic");
        button(review_button(area, false), "Discard draft");
        return;
    }
    if (controls.cancel_prompt) {
        const auto* marker = selected_marker(snapshot, controls);
        const bool holding = marker && marker->mission_status == MissionStatus::holding;
        label(std::string(holding ? "Release hold #" : "Cancel mission #") +
              std::to_string(controls.cancel_prompt->id) + "?", x, y, 21, ink, width);
        label("Resources already used remain spent.", x, y + 39, 15, muted, width);
        button(review_button(area, false), "Back");
        button(review_button(area, true), holding ? "Confirm release" : "Confirm cancellation", true);
        return;
    }
    if (controls.show_missions) {
        label("Missions", x, y, 21, ink, width);
        if (snapshot.mission_markers.empty()) {
            label("No missions reported.", x, y + 33, 15, muted, width);
            return;
        }
        label("Select an order to inspect and locate it.", x, y + 30, 14, muted, width);
        for (std::size_t row = 0, index = controls.mission_offset;
             row < mission_rows(area) && index < snapshot.mission_markers.size(); ++row, ++index) {
            const auto& marker = snapshot.markers[snapshot.mission_markers[index]];
            const auto bounds = mission_row(area, row);
            DrawRectangleRec(bounds, CheckCollisionPointRec(GetMousePosition(), bounds) ? border : background);
            label(std::string(faction_name(*marker.faction)) + " / " + marker.label + " / " + marker.detail,
                  bounds.x + 7, bounds.y + 6, 15, mission_color(*marker.mission_status), bounds.width - 14);
        }
        return;
    }
    if (const auto* marker = selected_marker(snapshot, controls)) {
        constexpr std::array kinds{"Formation", "Contact", "Carrier", "Installation", "Stockpile", "Mission"};
        label(marker->label, x, y, 21, ink, width);
        const auto identity = marker->kind == MarkerKind::installation && marker->estimated ? "Site report" :
            std::string(kinds.at(static_cast<std::size_t>(marker->kind))) + " #" + std::to_string(marker->id);
        label(identity + " / Sector " + std::to_string(static_cast<std::uint32_t>(marker->sector)),
              x, y + 32, 15, accent, width);
        const auto location = marker->kind == MarkerKind::mission && marker->estimated ? "Sector objective (map centre)" :
            marker->estimated ? "Sector-centre estimate" : "Geographic position";
        label(location, x, y + 58, 14, muted, width);
        label(position_text(marker->position, !marker->estimated), x, y + 78, 15, ink, width);
        label(marker->detail, x, y + 112, 15, ink, width);
        label(marker->movement, x, y + 135, 15, muted, width);
        if (marker->kind == MarkerKind::unit && session.permissions().faction == marker->faction) {
            const auto actions = session.actions_for(UnitId{marker->id});
            for (std::size_t i = 0; i < actions.size(); ++i) {
                const auto kind = actions[i].kind;
                button(action_button(area, i), kind == MissionKind::ground_recon ? "Gnd Recon" :
                    kind == MissionKind::ground_patrol ? "Gnd Patrol" : action_label(kind).c_str());
            }
            if (const auto reserve = session.operational_reserve(UnitId{marker->id})) {
                button(action_button(area, actions.size()), *reserve ? "End reserve" : "Set reserve");
            } else if (session.permissions().may_issue_orders && actions.empty()) {
                label("No orders available from this formation.", x, y + 159, 14, muted, width);
            }
        } else if (marker->kind == MarkerKind::mission && marker->faction &&
                   session.can_cancel(*marker->faction, MissionId{marker->id})) {
            button(cancel_mission_button(area), marker->mission_status == MissionStatus::holding ?
                   "Release hold" : "Cancel mission");
        }
        if (!marker->inspection.empty()) {
            const auto visible = inspection_rows(*marker, area);
            const auto count = marker->inspection.size();
            const auto offset = std::min(controls.detail_offset, inspection_max_offset(*marker, area));
            if (count > visible) {
                label("Scroll for details  /  " + std::to_string(offset + 1) + "-" +
                      std::to_string(std::min(offset + visible, count)) + " of " + std::to_string(count),
                      x, y + inspection_top(*marker) - 20, 13, muted, width);
            }
            for (auto i = offset; i < std::min(offset + visible, count); ++i) {
                label(marker->inspection[i], x, y + inspection_top(*marker) + 18 * (i - offset), 15, ink, width);
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
        label("Dashed gold / Draft route", x, y + 234, 15, route_preview, width);
        label("Grey / Travelled route", x, y + 257, 15, route_travelled, width);
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
        y = wrapped_label(controls.feedback->message, area.x + 15, y, 14, color, area.width - 30) + 8;
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
          const TerritoryLayer& control, const CommandSession& session, const Layout& screen, bool ended, bool launched_from_menu)
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
    label(clock, 410, 74, 17, ink, launched_from_menu ? screen.save.x - 420 : GetScreenWidth() - 426);
    if (launched_from_menu) {
        button(screen.save, "Save (F5)");
        button(screen.menu, "Menu");
    }
    button(screen.fit, "Fit (R)");
    button(screen.missions, "Missions (M)", controls.show_missions);
    button(screen.supplies, "Supplies (L)", controls.supply.open);
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

void run(CampaignState& campaign, std::optional<FactionId> faction,
         const std::optional<std::filesystem::path>& saves = {})
{
    using Clock = std::chrono::steady_clock;
    SetWindowTitle("OpenWar Campaign Viewer");
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
            if (controls.supply.open) {
                if (controls.supply.draft || controls.supply.confirm_cancel) {
                    controls.supply.draft.reset();
                    controls.supply.confirm_cancel = false;
                } else {
                    controls.supply.open = false;
                    controls.supply.selected.reset();
                }
            } else if (!controls.draft && !controls.cancel_prompt) {
                if (!controls.show_missions) {
                    // The launcher shares this input context with the viewer.
                    if (saves)
                        PollInputEvents();
                    break;
                }
                controls.show_missions = false;
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
        if (saves && clicked(screen.menu)) {
            PollInputEvents();
            break;
        }
        if (saves && (IsKeyPressed(KEY_F5) || clicked(screen.save))) {
            try {
                const auto checkpoint = save_new_checkpoint(campaign, *saves);
                controls.feedback = CommandFeedback{"Saved " + checkpoint.filename().string(), true};
            } catch (const std::exception& error) {
                controls.feedback = CommandFeedback{error.what(), false};
            }
        }
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
        const bool mission_click = handle_mission_list(controls, snapshot, screen);
        handle_map_input(controls, snapshot, geography, screen);
        if (controls.draft && controls.draft->target &&
            (!controls.draft->preview || controls.draft->preview->tick != snapshot.tick)) {
            auto& draft = *controls.draft;
            draft.preview = session.preview(draft.action.kind, *draft.target, draft.requested_formation(), draft.via);
        }
        if (!mission_click && (handle_supply_input(controls, snapshot, screen, session) ||
                              handle_command_input(controls, snapshot, screen.details, session))) {
            snapshot = session.snapshot(controls.perspective);
            controls.event_offset = 0;
        }
        draw(snapshot, controls, geography, territory_layer, session, screen, campaign.war_ended(), saves.has_value());
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 1) {
            const Window window;
            const auto data_directory = launcher_data_directory();
            std::string startup_error;
            while (!WindowShouldClose()) {
                SetWindowTitle("OpenWar");
                auto selection = show_launcher(data_directory, std::exchange(startup_error, {}));
                if (!selection)
                    break;
                try {
                    run(selection->campaign, selection->faction, data_directory / "saves");
                } catch (const std::exception& error) {
                    startup_error = error.what();
                }
            }
            return 0;
        }
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
                             "With no arguments, open the campaign launcher.\n"
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
            if (scenario_path) {
                return open_campaign_scenario(*scenario_path, faction, seed_override);
            }
            auto settings = ProceduralSettings{};
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
        if (!CommandSession(campaign, faction).allows_perspective()) {
            throw std::invalid_argument("Campaign does not grant command authority to that faction");
        }
        const Window window;
        run(campaign, faction);
        if (save_path) {
            campaign.save(*save_path);
        }
    } catch (const std::exception& error) {
        std::cerr << "openwar-gui: " << error.what() << '\n';
        return 1;
    }
}
