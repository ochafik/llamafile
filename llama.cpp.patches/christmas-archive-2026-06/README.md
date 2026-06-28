# Christmas-2025 upgrade effort — disk-only state archive (captured 2026-06-28)

Backup of the `upgrade`-branch port effort whose CC sessions were purged and whose
applied patch state + submodule pin lived **only on disk** (R8 in
`../../docs/upgrade/RESPAWN-2026-06.md`). Captured before any branch/rebase work on
the respawn.

## Contents
- `superproject-applied.patch` — uncommitted tracked diffs in the llamafile superproject
  working tree (`llamafile/metal.c`, `llamafile/sgemm.h`, `build/config.mk`,
  `docs/source_installation.md`). Includes the live `[MAIN DEBUG]` instrumentation block.
- `submodule-applied-tracked.patch` — uncommitted tracked diffs inside the `llama.cpp`
  submodule (`ggml-metal.m` +cosmo_dlopen/ggml_metal_link, `ggml-backend.cpp`,
  `ggml-backend-impl.h`, `src/llama-model.cpp`).
- `submodule-untracked.tar.gz` — untracked files added inside the submodule working tree
  (per-ISA `ggml-vector-*.c` / `ggml-quants-*.c`, `cores.{h,cpp}`, `ggml-barrier.cpp`,
  `tools/`, etc.) — the llamafile-ported source copied into the vendored tree.
- `submodule-3cea-pin.bundle` — the 3 local-only submodule commits on top of upstream
  `7242dd967` culminating in pin `3cea47541` ("feat: add ggml_metal_link and
  ggml_backend_api for cosmopolitan"). Thin bundle; requires `7242dd967` (recoverable
  from ggerganov/llama.cpp).

## Restore
```sh
# superproject applied state (from repo root, on the target branch):
git apply llama.cpp.patches/christmas-archive-2026-06/superproject-applied.patch

# submodule pin objects (run inside the llama.cpp submodule):
cd llama.cpp
git fetch ../llama.cpp.patches/christmas-archive-2026-06/submodule-3cea-pin.bundle \
    'refs/tags/archive/christmas-pin-3cea:refs/tags/archive/christmas-pin-3cea'
git apply ../llama.cpp.patches/christmas-archive-2026-06/submodule-applied-tracked.patch
tar -xzf ../llama.cpp.patches/christmas-archive-2026-06/submodule-untracked.tar.gz
```

## Refs created (all backed up off-disk)
- superproject tag `archive/christmas-upgrade-2026-06` -> `07b0a1b43` (pushed to `origin`)
- superproject branch `upgrade` (6 formerly-disk-only commits + this archive) -> `origin/upgrade`
- submodule pin `3cea47541` -> pushed to `ochafik/llama.cpp-private` as both
  tag and branch `archive/christmas-pin-3cea` (the private fork was synced from ggml-org
  first so the base `7242dd967` is present). Also captured in `submodule-3cea-pin.bundle`.

## NOTE on the `ggml_metal_link` trampoline
The `ggml_metal_link` per-function trampoline captured here is **superseded** — synced
`main` (v0.10.0) ships a self-contained-dylib Metal loader that solves the same
cosmo_dlopen NULL-pointer problem at the root. This archive is for forensic recovery, not
as the GPU strategy going forward. See RESPAWN-2026-06.md §2.2 (amended).
