#ifndef OPENWAR_VIEWER_SYMBOLS_HPP
#define OPENWAR_VIEWER_SYMBOLS_HPP

#include "viewer_model.hpp"

#include <raylib.h>

namespace openwar::viewer {

// Both functions use screen pixels and require a military symbol on the marker.
void draw_military_symbol(const Marker& marker, Vector2 anchor);
bool hit_military_symbol(const Marker& marker, Vector2 offset);

}  // namespace openwar::viewer

#endif
