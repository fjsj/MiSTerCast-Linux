#pragma once

#include <xcb/xcb.h>

#include <optional>
#include <string_view>
#include <vector>

#include "mistercast/types.hpp"

namespace mistercast {
// Never returns an empty list: a screen whose RandR reports no monitors, or that
// has no RandR at all, is described by one whole-screen entry flagged primary.
std::vector<Monitor> enumerateX11Monitors(xcb_connection_t*, xcb_screen_t*);

// Resolves a configured monitor name against an enumeration. A named request
// matches that name exactly. An empty request means "no preference": the primary
// monitor if one is flagged, otherwise the first, because RandR reports no
// primary at all under Xvfb and after some xrandr changes, and capturing a
// monitor beats refusing to stream. Fails only on a name that is not present.
std::optional<Monitor> selectX11Monitor(const std::vector<Monitor>& monitors,
                                        std::string_view requested);
}
