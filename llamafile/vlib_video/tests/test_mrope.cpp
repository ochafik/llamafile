// U3+U4 — cumulative MRoPE positions and selective rewind.
//
// The full test from VIDEO_CONV3D_CPP_DESIGN.md §5.1 needs a live
// llama_context. We can't bring one up in unit-test CI without burning a
// model load, so this is currently a compile-only smoke that exercises the
// per-frame {t, h, w} bookkeeping arithmetic that vlib_video_session uses
// internally — without touching llama_decode.
//
// When we have a stub llama_context (or a tiny test model checked into
// vendor/), replace this with the full design-doc test.

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct grid_thw { int t; int h; int w; };

// Sum the temporal axis of the accumulated grid — what the session passes
// as pos_0_t on the next frame.
int cumulative_t(const std::vector<grid_thw> & grids) {
    int sum = 0;
    for (const auto & g : grids) sum += g.t;
    return sum;
}

} // namespace

int main() {
    std::vector<grid_thw> grids;
    for (int i = 0; i < 5; ++i) grids.push_back({1, 18, 14});

    // U3: after 5 frames at t=1 each, the next frame's pos_0_t is 5.
    const int got = cumulative_t(grids);
    if (got != 5) {
        std::fprintf(stderr, "mrope: expected cumulative_t=5 got %d\n", got);
        return 1;
    }

    // U4: pop on ignore_frame.
    grids.pop_back();
    if (cumulative_t(grids) != 4) {
        std::fprintf(stderr, "mrope: pop yielded %d (want 4)\n", cumulative_t(grids));
        return 1;
    }

    // U4b — Rewind-sync scenario (MLX_VLM_IMAGE_DIFFING_AUDIT.md Gap #2).
    // Three frames pushed, frame 2 ignored, then a "frame 3" pushed.
    // The cumulative_t must equal 3 (1+1+1), not 4 (1+1+1+1).
    {
        std::vector<grid_thw> g;
        g.push_back({1, 18, 14}); // frame 1
        g.push_back({1, 18, 14}); // frame 2
        g.push_back({1, 18, 14}); // frame 3
        if (cumulative_t(g) != 3) {
            std::fprintf(stderr, "mrope rewind: pre-pop cum_t=%d want 3\n", cumulative_t(g));
            return 1;
        }

        // Ignore frame 3 (simulate ignore_frame action — session pops the
        // most recent push).
        const grid_thw popped = g.back();
        g.pop_back();
        if (popped.t != 1 || popped.h != 18 || popped.w != 14) {
            std::fprintf(stderr, "mrope rewind: popped wrong entry t/h/w=%d/%d/%d\n",
                         popped.t, popped.h, popped.w);
            return 1;
        }
        if (cumulative_t(g) != 2) {
            std::fprintf(stderr, "mrope rewind: post-pop cum_t=%d want 2\n", cumulative_t(g));
            return 1;
        }

        // Push the *new* frame 3 (kept).
        g.push_back({1, 18, 14});
        if (cumulative_t(g) != 3) {
            std::fprintf(stderr, "mrope rewind: final cum_t=%d want 3 (NOT 4)\n",
                         cumulative_t(g));
            return 1;
        }
        if ((int)g.size() != 3) {
            std::fprintf(stderr, "mrope rewind: grid size=%zu want 3\n", g.size());
            return 1;
        }
    }

    std::printf("mrope: ok (U3=5, U4 pop→4, U4b rewind-sync 3-frame=3)\n");
    return 0;
}
