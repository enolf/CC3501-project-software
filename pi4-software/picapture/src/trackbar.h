#pragma once

struct TrackbarSpec {
    int default_val;
    int max_val;
};

namespace Trackbar {
    constexpr TrackbarSpec LowH           = {0,   179};
    constexpr TrackbarSpec HighH          = {179, 179};
    constexpr TrackbarSpec LowS           = {0,   255};
    constexpr TrackbarSpec HighS          = {255, 255};
    constexpr TrackbarSpec LowV           = {0,   255};
    constexpr TrackbarSpec HighV          = {255, 255};
    constexpr TrackbarSpec Open           = {0,   10};
    constexpr TrackbarSpec Close          = {0,   10};
    constexpr TrackbarSpec ContourMinArea = {500, 10000};
    constexpr TrackbarSpec ContourMaxArea = {10000, 10000};
}