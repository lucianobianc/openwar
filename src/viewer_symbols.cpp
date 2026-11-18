#include "viewer_symbols.hpp"

#include <cmath>

namespace openwar::viewer {
namespace {

// Historical reference geometry from NATO Map Symbols (Canadian), pp. 5, 8–9, 16, 23 and 27.
struct SymbolFrame {
    std::span<const Vector2> outline;
    Color fill;
    float top;
};

constexpr Vector2 friendly_land[]{{-13, -9}, {13, -9}, {13, 9}, {-13, 9}};
constexpr Vector2 hostile_land[]{{0, -14}, {14, 0}, {0, 14}, {-14, 0}};
constexpr Vector2 neutral_land[]{{-10, -10}, {10, -10}, {10, 10}, {-10, 10}};
constexpr Vector2 unknown_land[]{{-6, -6}, {-5, -11}, {-2, -14}, {2, -14}, {5, -11}, {6, -6},
    {11, -5}, {14, -2}, {14, 2}, {11, 5}, {6, 6}, {5, 11}, {2, 14}, {-2, 14}, {-5, 11}, {-6, 6},
    {-11, 5}, {-14, 2}, {-14, -2}, {-11, -5}};
constexpr Vector2 observer_frame[]{{-8, -10}, {8, -10}, {14, 0}, {8, 10}, {-8, 10}, {-14, 0}};
constexpr Vector2 friendly_air[]{{-10, 9}, {-10, -1}, {-8, -7}, {-4, -11}, {0, -12}, {4, -11}, {8, -7}, {10, -1}, {10, 9}};
constexpr Vector2 hostile_air[]{{-10, 9}, {-10, -3}, {0, -13}, {10, -3}, {10, 9}};
constexpr Vector2 unknown_air[]{{-11, 8}, {-14, 4}, {-14, 0}, {-11, -5}, {-6, -6}, {-5, -11}, {-2, -14},
    {2, -14}, {5, -11}, {6, -6}, {11, -5}, {14, 0}, {14, 4}, {11, 8}};
constexpr Vector2 friendly_sea[]{{0, -11}, {5, -10}, {9, -7}, {11, -3}, {11, 3}, {9, 7}, {5, 10}, {0, 11},
    {-5, 10}, {-9, 7}, {-11, 3}, {-11, -3}, {-9, -7}, {-5, -10}};
constexpr Color friendly_fill{155, 208, 251, 255};
constexpr Color hostile_fill{255, 157, 164, 255};
constexpr Color neutral_fill{168, 224, 169, 255};
constexpr Color unknown_fill{252, 217, 133, 255};
constexpr Color symbol_line{24, 33, 40, 255};
constexpr SymbolFrame land_frames[]{
    {friendly_land, friendly_fill, -9}, {hostile_land, hostile_fill, -14},
    {neutral_land, neutral_fill, -10}, {unknown_land, unknown_fill, -14}, {observer_frame, RAYWHITE, -10}
};
constexpr SymbolFrame air_frames[]{
    {friendly_air, friendly_fill, -12}, {hostile_air, hostile_fill, -13},
    land_frames[2], {unknown_air, unknown_fill, -14}, land_frames[4]
};
constexpr SymbolFrame sea_frames[]{
    {friendly_sea, friendly_fill, -11}, land_frames[1], land_frames[2], land_frames[3], land_frames[4]
};
constexpr std::array echelon_marks{"", "", "", "", "", "X", "XX"};

const SymbolFrame& symbol_frame(MilitarySymbol symbol)
{
    const auto affiliation = static_cast<std::size_t>(symbol.affiliation);
    switch (symbol.identity.domain) {
    case MilitaryDomain::air: return air_frames[affiliation];
    case MilitaryDomain::sea: return sea_frames[affiliation];
    default: return land_frames[affiliation];
    }
}

enum class Interior { none, cross, ellipse, cross_and_ellipse, slash, dot, bridge, lightning };
constexpr std::array land_interiors{Interior::none, Interior::cross, Interior::ellipse,
    Interior::cross_and_ellipse, Interior::slash, Interior::dot, Interior::bridge, Interior::lightning,
    Interior::none, Interior::none};

bool is_installation(const Marker& marker)
{
    return marker.kind == MarkerKind::installation || marker.kind == MarkerKind::depot;
}

void draw_symbol(const MilitarySymbol& symbol, Vector2 at, bool installation)
{
    const auto& frame = symbol_frame(symbol);
    const auto point = [at](Vector2 offset) { return Vector2{at.x + offset.x, at.y + offset.y}; };
    const auto line = [&](Vector2 from, Vector2 to) {
        DrawLineEx(point(from), point(to), 1.5f, symbol_line);
    };
    for (std::size_t i = 0; i < frame.outline.size(); ++i) {
        const auto from = frame.outline[i];
        const auto to = frame.outline[(i + 1) % frame.outline.size()];
        DrawTriangle(at, point(to), point(from), frame.fill);
    }
    for (std::size_t i = 0; i < frame.outline.size(); ++i) {
        line(frame.outline[i], frame.outline[(i + 1) % frame.outline.size()]);
    }
    if (installation) {
        DrawRectangle(at.x - 5, at.y + frame.top - 3, 10, 4, symbol_line);
    }
    const auto identity = symbol.identity;
    if (identity.domain == MilitaryDomain::land) {
        const auto interior = land_interiors[static_cast<std::size_t>(identity.function)];
        switch (interior) {
        case Interior::cross:
        case Interior::cross_and_ellipse:
            line({-7, -5}, {7, 5});
            line({-7, 5}, {7, -5});
            break;
        case Interior::slash:
            line({-8, 5}, {8, -5});
            break;
        case Interior::dot:
            DrawCircleV(at, 3.5f, symbol_line);
            break;
        case Interior::bridge:
            line({-5, 3}, {-5, -3});
            line({-5, -3}, {5, -3});
            line({0, -3}, {0, 3});
            line({5, -3}, {5, 3});
            break;
        case Interior::lightning:
            line({-8, -5}, {0, 2});
            line({0, 2}, {0, -2});
            line({0, -2}, {8, 5});
            break;
        default:
            break;
        }
        if (interior == Interior::ellipse || interior == Interior::cross_and_ellipse) {
            DrawEllipseLines(at.x, at.y, 8, 4, symbol_line);
        }
        const auto echelon = identity.echelon;
        if (echelon >= MilitaryEchelon::company && echelon <= MilitaryEchelon::regiment) {
            const auto count = static_cast<int>(echelon) - static_cast<int>(MilitaryEchelon::platoon);
            for (int n = 0; n < count; ++n) {
                const float x = (n - (count - 1) / 2.f) * 4.f;
                DrawLineEx(point({x, frame.top - 8}), point({x, frame.top - 2}), 2, RAYWHITE);
            }
        } else if (echelon == MilitaryEchelon::platoon) {
            for (float x : {-4.f, 0.f, 4.f}) {
                DrawCircleV(point({x, frame.top - 4.5f}), 1.5f, RAYWHITE);
            }
        } else if (echelon != MilitaryEchelon::unspecified) {
            const auto marks = echelon_marks[static_cast<std::size_t>(echelon)];
            DrawText(marks, at.x - MeasureText(marks, 10) / 2, at.y + frame.top - 11, 10, RAYWHITE);
        }
    }
}

const char* symbol_modifier(const Marker& marker)
{
    const bool west = marker.symbol->affiliation == Affiliation::observer && marker.faction == FactionId::west;
    const bool east = marker.symbol->affiliation == Affiliation::observer && marker.faction == FactionId::east;
    switch (marker.symbol->status) {
    case SymbolStatus::reported:
        return "?";
    case SymbolStatus::destroyed:
        if (marker.kind == MarkerKind::unit) {
            return west ? "W LOST" : east ? "E LOST" : "LOST";
        }
        return west ? "W OUT" : east ? "E OUT" : "OUT";
    case SymbolStatus::present:
        if (marker.kind == MarkerKind::carrier) {
            return west ? "W T" : east ? "E T" : "T";
        }
        return west ? "W" : east ? "E" : nullptr;
    }
    return nullptr;
}

}  // namespace

bool hit_military_symbol(const Marker& marker, Vector2 offset)
{
    const auto& frame = symbol_frame(*marker.symbol);
    const auto outline = frame.outline;
    // Include the visible frame, ancillary mark, and a small selection margin.
    if (CheckCollisionPointPoly(offset, outline.data(), static_cast<int>(outline.size()))) {
        return true;
    }
    for (std::size_t i = 0; i < outline.size(); ++i) {
        if (CheckCollisionPointLine(offset, outline[i], outline[(i + 1) % outline.size()], 2)) {
            return true;
        }
    }
    if (std::abs(offset.x) <= 8 && offset.y <= frame.top && offset.y >= frame.top - 11 &&
        (is_installation(marker) || (marker.symbol->identity.domain == MilitaryDomain::land &&
                                    marker.symbol->identity.echelon != MilitaryEchelon::unspecified))) {
        return true;
    }
    if (const auto* modifier = symbol_modifier(marker)) {
        return std::abs(offset.x) <= MeasureText(modifier, 10) / 2 + 2 && offset.y >= 14 && offset.y <= 25;
    }
    return false;
}

void draw_military_symbol(const Marker& marker, Vector2 at)
{
    draw_symbol(*marker.symbol, at, is_installation(marker));
    // OpenWar annotations are drawn outside the reference symbol.
    if (const auto* modifier = symbol_modifier(marker)) {
        const auto width = MeasureText(modifier, 10);
        DrawRectangle(at.x - width / 2 - 2, at.y + 14, width + 4, 11, symbol_line);
        DrawText(modifier, at.x - width / 2, at.y + 14, 10, RAYWHITE);
    }
}

}  // namespace openwar::viewer
