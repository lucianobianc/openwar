#include "viewer_model.hpp"
#include "realtime.hpp"

#include <raylib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

using namespace openwar;
using namespace openwar::viewer;

constexpr float tile_size = 100;
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

struct Layout {
    Rectangle map;
    Rectangle details;
    Rectangle events;
    std::array<Rectangle, 3> perspective;
    Rectangle play;
    Rectangle slower;
    Rectangle faster;
};

Layout layout()
{
    const float width = GetScreenWidth();
    const float height = GetScreenHeight();
    constexpr float side = 350;
    return {{16, 111, width - side - 48, height - 154},
            {width - side - 16, 111, side, 250},
            {width - side - 16, 374, side, height - 390},
            {{{width - 294, 19, 108, 32}, {width - 178, 19, 76, 32}, {width - 94, 19, 78, 32}}},
            {16, 65, 98, 32}, {126, 65, 37, 32}, {230, 65, 37, 32}};
}

struct ViewControls {
    std::optional<FactionId> perspective;
    std::optional<SectorId> selected;
    Camera2D camera{};
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

void fit_map(ViewControls& controls, const Snapshot& snapshot, Rectangle map)
{
    const Vector2 extent{snapshot.width * tile_size, snapshot.height * tile_size};
    controls.camera.target = {extent.x / 2, extent.y / 2};
    controls.camera.zoom = std::min({(map.width - 42) / extent.x, (map.height - 42) / extent.y, 2.2f});
}

void handle_input(ViewControls& controls, const Snapshot& snapshot, const Layout& screen, bool ended)
{
    constexpr std::array<std::optional<FactionId>, 3> perspectives{{{}, FactionId::west, FactionId::east}};
    constexpr std::array keys{KEY_ONE, KEY_TWO, KEY_THREE};
    for (std::size_t i = 0; i < perspectives.size(); ++i) {
        if (clicked(screen.perspective[i]) || IsKeyPressed(keys[i])) {
            controls.perspective = perspectives[i];
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
    controls.camera.offset = {screen.map.x + screen.map.width / 2, screen.map.y + screen.map.height / 2};
    if (IsKeyPressed(KEY_R)) {
        fit_map(controls, snapshot, screen.map);
    }
    const auto mouse = GetMousePosition();
    const auto wheel = GetMouseWheelMove();
    if (CheckCollisionPointRec(mouse, screen.events) && wheel != 0) {
        const auto offset = static_cast<long long>(controls.event_offset) + (wheel > 0 ? 3 : -3);
        controls.event_offset = static_cast<std::size_t>(std::clamp(
            offset, 0LL, snapshot.events.empty() ? 0LL : static_cast<long long>(snapshot.events.size() - 1)));
    }
    if (controls.selected && CheckCollisionPointRec(mouse, screen.details) && wheel != 0) {
        const auto count = std::ranges::count(snapshot.markers, *controls.selected, &Marker::sector);
        const auto offset = static_cast<long long>(controls.detail_offset) + (wheel > 0 ? -1 : 1);
        controls.detail_offset = static_cast<std::size_t>(
            std::clamp(offset, 0LL, std::max(0LL, static_cast<long long>(count) - 3)));
    }
    if (CheckCollisionPointRec(mouse, screen.map)) {
        if (wheel != 0) {
            const auto anchor = GetScreenToWorld2D(mouse, controls.camera);
            controls.camera.zoom = std::clamp(controls.camera.zoom * std::pow(1.16f, wheel), 0.000001f, 6.0f);
            const auto after = GetScreenToWorld2D(mouse, controls.camera);
            controls.camera.target.x += anchor.x - after.x;
            controls.camera.target.y += anchor.y - after.y;
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            const auto delta = GetMouseDelta();
            controls.camera.target.x -= delta.x / controls.camera.zoom;
            controls.camera.target.y -= delta.y / controls.camera.zoom;
        }
        if (clicked(screen.map)) {
            controls.detail_offset = 0;
            const auto world = GetScreenToWorld2D(mouse, controls.camera);
            const auto x = std::floor(world.x / tile_size);
            const auto y = std::floor(world.y / tile_size);
            if (x >= 0 && y >= 0 && x < snapshot.width && y < snapshot.height) {
                controls.selected = SectorId{static_cast<std::uint32_t>(y) * snapshot.width +
                                             static_cast<std::uint32_t>(x)};
            } else {
                controls.selected.reset();
            }
        }
    }
    const float step = 450 * GetFrameTime() / controls.camera.zoom;
    controls.camera.target.x += step * (IsKeyDown(KEY_RIGHT) - IsKeyDown(KEY_LEFT));
    controls.camera.target.y += step * (IsKeyDown(KEY_DOWN) - IsKeyDown(KEY_UP));
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

void draw_map(const Snapshot& snapshot, const ViewControls& controls, Rectangle map)
{
    DrawRectangleRec(map, {11, 17, 27, 255});
    BeginScissorMode(static_cast<int>(map.x), static_cast<int>(map.y),
                     static_cast<int>(map.width), static_cast<int>(map.height));
    BeginMode2D(controls.camera);
    const auto corner = GetScreenToWorld2D({map.x, map.y}, controls.camera);
    const auto end = GetScreenToWorld2D({map.x + map.width, map.y + map.height}, controls.camera);
    const auto index = [](float coordinate, std::uint32_t size) {
        return static_cast<std::uint32_t>(std::clamp(
            std::floor(static_cast<double>(coordinate) / tile_size), 0.0, static_cast<double>(size)));
    };
    const auto last_x = index(end.x + tile_size, snapshot.width);
    const auto last_y = index(end.y + tile_size, snapshot.height);
    for (auto y = index(corner.y, snapshot.height); y < last_y; ++y) {
        for (auto x = index(corner.x, snapshot.width); x < last_x; ++x) {
            const auto id = y * snapshot.width + x;
            const auto& tile = snapshot.tiles[id];
            const Rectangle rect{x * tile_size + 1, y * tile_size + 1, tile_size - 2, tile_size - 2};
            const auto fill = tile.contested ? contested :
                tile.confidence < 350 ? unknown : faction_color(tile.controller);
            DrawRectangleRec(rect, tile.confidence >= 350 && tile.confidence < 750 ? Fade(fill, 0.58f) : fill);
            if (controls.camera.zoom > 0.35f) {
                DrawText(TextFormat("%u", id), static_cast<int>(rect.x + 8), static_cast<int>(rect.y + 7), 13, ink);
            }
            if (controls.selected == SectorId{id}) {
                DrawRectangleLinesEx(rect, 2 / controls.camera.zoom, RAYWHITE);
            }
        }
    }
    struct TileMarkers {
        bool west_units = false;
        bool east_units = false;
        bool contacts = false;
        bool installations = false;
        bool missions = false;
    };
    std::map<SectorId, TileMarkers> visible;
    for (const auto& marker : snapshot.markers) {
        auto& indicators = visible[marker.sector];
        switch (marker.kind) {
        case MarkerKind::unit:
            if (marker.faction == FactionId::west) {
                indicators.west_units = true;
            } else if (marker.faction == FactionId::east) {
                indicators.east_units = true;
            }
            break;
        case MarkerKind::contact: indicators.contacts = true; break;
        case MarkerKind::installation: indicators.installations = true; break;
        case MarkerKind::mission: indicators.missions = true; break;
        }
    }
    for (const auto& [sector, indicators] : visible) {
        const auto id = static_cast<std::uint32_t>(sector);
        const float x = (id % snapshot.width) * tile_size;
        const float y = (id / snapshot.width) * tile_size;
        if (indicators.west_units) {
            DrawRectangleRec({x + 13, y + 45, 14, 16}, {155, 208, 251, 255});
        }
        if (indicators.east_units) {
            DrawRectangleRec({x + 29, y + 45, 14, 16}, {255, 157, 164, 255});
        }
        if (indicators.contacts) {
            DrawCircleLines(static_cast<int>(x + 56), static_cast<int>(y + 53), 8, RAYWHITE);
        }
        if (indicators.installations) {
            DrawRectangleLinesEx({x + 72, y + 45, 15, 16}, 2, RAYWHITE);
        }
        if (indicators.missions) {
            DrawTriangle({x + 50, y + 74}, {x + 38, y + 91}, {x + 62, y + 91}, accent);
        }
    }
    EndMode2D();
    EndScissorMode();
    DrawRectangleLinesEx(map, 1, border);
}

void draw_details(const Snapshot& snapshot, const ViewControls& controls, Rectangle area)
{
    DrawRectangleRec(area, panel);
    const float x = area.x + 15;
    float y = area.y + 14;
    const float width = area.width - 30;
    if (!controls.selected) {
        label("Sector details", x, y, 23, ink, width);
        label("Click a sector to inspect its reports.", x, y + 35, 16, muted, width);
        label("Solid square / Unit", x, y + 70, 15, muted, width);
        label("Ring / Reported contact", x, y + 92, 15, muted, width);
        label("Outlined square / Installation", x, y + 114, 15, muted, width);
        label("Triangle / Mission target", x, y + 136, 15, muted, width);
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
        if (marker.sector != controls.selected) {
            continue;
        }
        if (skip) {
            --skip;
            continue;
        }
        if (y + 35 > area.y + area.height - 22) {
            ++remaining;
            continue;
        }
        label(marker.label, x, y, 16, ink, width);
        label(marker.detail, x, y + 18, 14, muted, width);
        y += 39;
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

void draw(const Snapshot& snapshot, const ViewControls& controls, const Layout& screen, bool ended)
{
    BeginDrawing();
    ClearBackground(background);
    label("OpenWar", 16, 15, 27, ink, 170);
    label("Campaign viewer", 166, 22, 17, muted, 185);
    button(screen.perspective[0], "Observer", !controls.perspective);
    button(screen.perspective[1], "WEST", controls.perspective == FactionId::west);
    button(screen.perspective[2], "EAST", controls.perspective == FactionId::east);
    button(screen.play, ended ? "Ended" : controls.paused ? "Resume" : "Pause");
    button(screen.slower, "-");
    button(screen.faster, "+");
    label(std::to_string(controls.speed) + "x", 171, 74, 16, ink, 59);
    const auto clock = "Tick " + std::to_string(snapshot.tick) + "  /  " +
                       std::to_string(snapshot.elapsed_seconds) + " s  /  " +
                       std::string(war_status_name(snapshot.war.status));
    label(clock, 289, 74, 17, ink, GetScreenWidth() - 305);
    draw_map(snapshot, controls, screen.map);
    draw_details(snapshot, controls, screen.details);
    draw_events(snapshot, controls, screen.events);
    const auto y = static_cast<int>(screen.map.y + screen.map.height + 12);
    int x = 16;
    for (const auto& [name, color] : std::array{
            std::pair{"WEST", west}, std::pair{"EAST", east}, std::pair{"Neutral", neutral},
            std::pair{"Contested", contested}, std::pair{"Unknown", unknown}}) {
        DrawRectangle(x, y, 11, 11, color);
        DrawText(name, x + 16, y - 1, 13, muted);
        x += MeasureText(name, 13) + 27;
    }
    label("Wheel: zoom  /  Right or middle drag: pan  /  R: fit", 16, y + 15, 13, muted, screen.map.width);
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
        SetTargetFPS(60);
    }

    ~Window() { CloseWindow(); }
};

void run(CampaignState& campaign)
{
    using Clock = std::chrono::steady_clock;
    const Window window;
    ViewControls controls;
    auto snapshot = read_snapshot(campaign, controls.perspective);
    fit_map(controls, snapshot, layout().map);
    auto previous = Clock::now();
    std::chrono::duration<long double> simulated{};
    while (!WindowShouldClose()) {
        const auto now = Clock::now();
        if (!controls.paused && !campaign.war_ended()) {
            simulated += std::chrono::duration<long double>(now - previous) * controls.speed;
            const auto due = cli::ticks_due(simulated, 1, campaign.clock().tick_seconds());
            if (due > campaign.clock().tick()) {
                campaign.advance_ticks(std::min<std::uint64_t>(due - campaign.clock().tick(), 256));
            }
        }
        previous = now;
        const auto screen = layout();
        const auto old_perspective = controls.perspective;
        handle_input(controls, snapshot, screen, campaign.war_ended());
        if (snapshot.tick != campaign.clock().tick() || old_perspective != controls.perspective) {
            snapshot = read_snapshot(campaign, controls.perspective);
            controls.event_offset = std::min(controls.event_offset,
                snapshot.events.empty() ? std::size_t{0} : snapshot.events.size() - 1);
            if (controls.selected) {
                const auto count = std::ranges::count(snapshot.markers, *controls.selected, &Marker::sector);
                controls.detail_offset = std::min(controls.detail_offset,
                    static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, count - 3)));
            }
        }
        draw(snapshot, controls, screen, campaign.war_ended());
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    try {
        std::uint32_t seed = 0;
        std::string_view scenario = "combat";
        for (int i = 1; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--help") {
                std::cout << "Usage: openwar-gui [--demo combat|operations] [--seed UINT32]\n";
                return 0;
            }
            if ((option != "--seed" && option != "--demo") || i + 1 == argc) {
                throw std::invalid_argument("Expected --demo combat|operations or --seed UINT32");
            }
            const std::string_view value = argv[++i];
            if (option == "--demo") {
                if (value != "combat" && value != "operations") {
                    throw std::invalid_argument("Unknown viewer demonstration");
                }
                scenario = value;
            } else {
                const auto end = value.data() + value.size();
                const auto result = std::from_chars(value.data(), end, seed);
                if (result.ec != std::errc{} || result.ptr != end) {
                    throw std::invalid_argument("Seed must be a 32-bit unsigned integer");
                }
            }
        }
        auto campaign = scenario == "combat" ? CampaignState(combat_example_scenario(), seed) :
                                               operations_example_campaign(seed);
        run(campaign);
    } catch (const std::exception& error) {
        std::cerr << "openwar-gui: " << error.what() << '\n';
        return 1;
    }
}
