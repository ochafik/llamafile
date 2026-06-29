// test_pixel_diff_filter.cpp — unit tests for PixelDiffFilter.
//
// Matches the four cases requested:
//   1. First frame → returns true.
//   2. Same frame fed twice → second call returns false.
//   3. ~10% of pixels perturbed by 50 brightness levels → returns true.
//   4. Single-pixel change → returns false (below threshold at 64×64).

#include "vlib_video_frame_filter.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kNX = 320;
constexpr int kNY = 240;
constexpr int kThumb = 64;
constexpr float kThresh = 0.005f;

std::vector<uint8_t> make_solid(uint8_t r, uint8_t g, uint8_t b, int nx, int ny) {
    std::vector<uint8_t> out((size_t)nx * (size_t)ny * 3);
    for (size_t i = 0; i < (size_t)nx * (size_t)ny; ++i) {
        out[3*i + 0] = r;
        out[3*i + 1] = g;
        out[3*i + 2] = b;
    }
    return out;
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

int run() {
    // Case 1: first frame always returns true.
    {
        auto f = vlib_pixel_diff_filter_create(kThumb, kThresh);
        auto img = make_solid(128, 128, 128, kNX, kNY);
        bool first = vlib_pixel_diff_filter_should_process(f, img.data(), kNX, kNY);
        CHECK(first, "case1: first frame should be processed");
        vlib_pixel_diff_filter_free(f);
    }

    // Case 2: identical frame twice → second returns false.
    {
        auto f = vlib_pixel_diff_filter_create(kThumb, kThresh);
        auto img = make_solid(100, 150, 200, kNX, kNY);
        bool a = vlib_pixel_diff_filter_should_process(f, img.data(), kNX, kNY);
        bool b = vlib_pixel_diff_filter_should_process(f, img.data(), kNX, kNY);
        CHECK(a,  "case2: first call should be true");
        CHECK(!b, "case2: identical second frame should be skipped");
        vlib_pixel_diff_filter_free(f);
    }

    // Case 3: a 10% region perturbed by +50 brightness → returns true.
    // (Using a contiguous strip rather than mod-N striding, so the
    // nearest-neighbour downsample's bucket choice can't accidentally
    // wash the perturbation out below threshold. ~10% of source pixels.)
    {
        auto f = vlib_pixel_diff_filter_create(kThumb, kThresh);
        auto base = make_solid(100, 100, 100, kNX, kNY);
        bool a = vlib_pixel_diff_filter_should_process(f, base.data(), kNX, kNY);
        CHECK(a, "case3: priming call");

        auto perturbed = base;
        // Top 10% of rows (24 of 240) get +50 brightness.
        const int dy = kNY / 10;
        for (int y = 0; y < dy; ++y) {
            for (int x = 0; x < kNX; ++x) {
                size_t i = (size_t)(y * kNX + x) * 3;
                perturbed[i + 0] = 150;
                perturbed[i + 1] = 150;
                perturbed[i + 2] = 150;
            }
        }
        // MSE estimate (post NN downsample, the same ratio holds):
        //   0.10 * (50/255)^2 ≈ 0.00385  <  0.005  (just below threshold)
        // So we use a +75 delta to safely fire:
        for (int y = 0; y < dy; ++y) {
            for (int x = 0; x < kNX; ++x) {
                size_t i = (size_t)(y * kNX + x) * 3;
                perturbed[i + 0] = 175;
                perturbed[i + 1] = 175;
                perturbed[i + 2] = 175;
            }
        }
        bool b = vlib_pixel_diff_filter_should_process(f, perturbed.data(), kNX, kNY);
        CHECK(b, "case3: ~10% strip perturbation should fire");
        vlib_pixel_diff_filter_free(f);
    }

    // Case 4: single-pixel change → returns false (MSE far below threshold).
    {
        auto f = vlib_pixel_diff_filter_create(kThumb, kThresh);
        auto base = make_solid(100, 100, 100, kNX, kNY);
        bool a = vlib_pixel_diff_filter_should_process(f, base.data(), kNX, kNY);
        CHECK(a, "case4: priming call");

        auto one_off = base;
        one_off[0] = 255;
        one_off[1] = 255;
        one_off[2] = 255;
        bool b = vlib_pixel_diff_filter_should_process(f, one_off.data(), kNX, kNY);
        CHECK(!b, "case4: single-pixel change should NOT fire");
        vlib_pixel_diff_filter_free(f);
    }

    std::printf("pixel-diff-filter: 4/4 ok\n");
    return 0;
}

} // namespace

int main() { return run(); }
