// vlib_video_frame_filter.cpp — PixelDiffFilter implementation.
//
// Port of mlx-vlm-continuous/watchdawg/adaptive.py:PixelDiffFilter.
// Nearest-neighbour downsample (vs. MLX's PIL bilinear) — for an MSE
// pre-filter the difference is negligible and we avoid an image-lib
// dependency.

#include "vlib_video_frame_filter.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vlib {

struct pixel_diff_filter {
    int   thumb_size      = 64;
    float mse_threshold   = 0.005f;
    bool  has_prev        = false;
    // 3 * thumb_size * thumb_size, normalized to [0,1].
    std::vector<float> prev_thumb;
};

pixel_diff_filter * pixel_diff_filter_create(int thumb_size, float mse_threshold) {
    if (thumb_size <= 0) return nullptr;
    auto * f = new pixel_diff_filter;
    f->thumb_size    = thumb_size;
    f->mse_threshold = mse_threshold;
    f->has_prev      = false;
    f->prev_thumb.assign((size_t)3 * (size_t)thumb_size * (size_t)thumb_size, 0.0f);
    return f;
}

void pixel_diff_filter_free(pixel_diff_filter * f) {
    delete f;
}

// Nearest-neighbour downsample of an nx×ny RGB uint8 image to
// thumb×thumb floats in [0,1]. dst layout matches src: row-major, 3
// interleaved channels per pixel.
static void downsample_rgb_u8_to_f32(const uint8_t * src, int nx, int ny,
                                     float * dst, int thumb) {
    // Guard: empty input → zero buffer (callers shouldn't hit this but
    // we'd rather return false-MSE than UB).
    if (nx <= 0 || ny <= 0) {
        std::fill(dst, dst + (size_t)3 * thumb * thumb, 0.0f);
        return;
    }
    const float inv_255 = 1.0f / 255.0f;
    for (int ty = 0; ty < thumb; ++ty) {
        // Sample center of the bucket: (ty + 0.5) * ny / thumb.
        int sy = (int)((ty + 0.5f) * (float)ny / (float)thumb);
        if (sy >= ny) sy = ny - 1;
        if (sy < 0)   sy = 0;
        for (int tx = 0; tx < thumb; ++tx) {
            int sx = (int)((tx + 0.5f) * (float)nx / (float)thumb);
            if (sx >= nx) sx = nx - 1;
            if (sx < 0)   sx = 0;
            const uint8_t * sp = src + (size_t)(sy * nx + sx) * 3;
            float * dp = dst + (size_t)(ty * thumb + tx) * 3;
            dp[0] = (float)sp[0] * inv_255;
            dp[1] = (float)sp[1] * inv_255;
            dp[2] = (float)sp[2] * inv_255;
        }
    }
}

bool pixel_diff_filter_should_process(pixel_diff_filter * f,
                                      const uint8_t * rgb_data,
                                      int nx, int ny) {
    if (f == nullptr || rgb_data == nullptr) return true;
    const int thumb = f->thumb_size;
    const size_t n  = (size_t)3 * (size_t)thumb * (size_t)thumb;

    // Downsample into a scratch on the stack of vector; we reuse a temp
    // buffer to keep allocations down.
    std::vector<float> cur(n);
    downsample_rgb_u8_to_f32(rgb_data, nx, ny, cur.data(), thumb);

    if (!f->has_prev) {
        f->prev_thumb = std::move(cur);
        f->has_prev = true;
        return true; // first frame: always process.
    }

    // MSE between thumbnails.
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const float d = cur[i] - f->prev_thumb[i];
        acc += (double)d * (double)d;
    }
    const float mse = (float)(acc / (double)n);

    // Update prev unconditionally — matches MLX's behaviour (prev_thumb
    // is always replaced with the most recent observation, even on skip).
    f->prev_thumb = std::move(cur);

    return mse >= f->mse_threshold;
}

} // namespace vlib
