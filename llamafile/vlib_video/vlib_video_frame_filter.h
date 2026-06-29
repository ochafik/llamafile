// vlib_video_frame_filter.h — pre-VLM frame filters.
//
// PixelDiffFilter: cheap thumbnail-MSE pre-filter ported from
// mlx-vlm-continuous/watchdawg/adaptive.py (PixelDiffFilter, lines 111-150).
//
// Purpose: skip near-identical frames before they hit the VLM encoder.
// Caller downsamples the input to thumb_size×thumb_size (nearest-neighbour
// — matches MLX's behaviour, modulo PIL's bilinear default; nearest is
// fine for an MSE pre-filter and avoids pulling in a resize lib) and
// compares against the previous thumbnail by mean squared error of
// normalized channels (uint8 / 255 in [0,1]). First call always returns
// true; subsequent calls return true iff MSE >= mse_threshold.
//
// MLX default threshold: 0.005 (≈ 0.5% average pixel change).

#pragma once

#include <cstdint>

namespace vlib {

struct pixel_diff_filter;

// thumb_size: thumbnail edge length, e.g. 64. mse_threshold: float in
// [0,1]; frames with MSE strictly below this against the previous
// thumbnail are skipped. The filter owns a small float buffer of size
// 3*thumb_size*thumb_size.
pixel_diff_filter * pixel_diff_filter_create(int thumb_size, float mse_threshold);
void               pixel_diff_filter_free  (pixel_diff_filter * f);

// Returns true if the frame is "different enough" to process. Always
// returns true on the first call. rgb_data is uint8 NHWC of length
// nx*ny*3.
bool pixel_diff_filter_should_process(pixel_diff_filter * f,
                                      const uint8_t * rgb_data,
                                      int             nx,
                                      int             ny);

} // namespace vlib

// C-style aliases for callers that want a flatter API (matches the
// `vlib_pixel_diff_filter` style used in the rest of vlib-video's headers).
using vlib_pixel_diff_filter = vlib::pixel_diff_filter;

inline vlib::pixel_diff_filter * vlib_pixel_diff_filter_create(int thumb_size, float mse_threshold) {
    return vlib::pixel_diff_filter_create(thumb_size, mse_threshold);
}
inline void vlib_pixel_diff_filter_free(vlib::pixel_diff_filter * f) {
    vlib::pixel_diff_filter_free(f);
}
inline bool vlib_pixel_diff_filter_should_process(vlib::pixel_diff_filter * f,
                                                  const uint8_t * rgb_data,
                                                  int nx, int ny) {
    return vlib::pixel_diff_filter_should_process(f, rgb_data, nx, ny);
}
