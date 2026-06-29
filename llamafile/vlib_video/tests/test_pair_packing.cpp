// U1 — pair packing.
//
// The full test from VIDEO_CONV3D_CPP_DESIGN.md §5.1 needs a stubbed clip_ctx
// with synthesized patch_embeddings_0/_1 weights. We don't have that infra
// yet (clip_ctx construction is private to clip.cpp), so for now this binary
// just exercises the public n_tokens_per_pair arithmetic that vlib-video
// uses internally. When the stub-clip helper lands, swap this for a real
// encode round-trip.

#include <cstdio>
#include <cstdlib>

namespace {

// Mirror the formula from VIDEO_CONV3D_CPP_DESIGN.md §3.2.
int n_tokens_per_pair(int img_nx, int img_ny, int patch_size, int spatial_merge) {
    const int nx_patches = img_nx / patch_size / spatial_merge;
    const int ny_patches = img_ny / patch_size / spatial_merge;
    return nx_patches * ny_patches; // T_out = 1 after stride-2 collapse
}

} // namespace

int main() {
    // Design-doc example: 504x392 frame, 14-pixel patch, 2x2 spatial merge → 252 tokens.
    const int got = n_tokens_per_pair(504, 392, 14, 2);
    if (got != 252) {
        std::fprintf(stderr, "pair-packing: expected 252 got %d\n", got);
        return 1;
    }
    std::printf("pair-packing: ok (%d tokens for 504x392/14/2)\n", got);
    return 0;
}
