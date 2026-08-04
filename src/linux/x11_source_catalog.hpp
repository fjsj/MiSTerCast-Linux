#pragma once

#include <xcb/xcb.h>

#include <vector>

#include "mistercast/types.hpp"

namespace mistercast {
std::vector<Monitor> enumerateX11Monitors(xcb_connection_t*, xcb_screen_t*);
}
