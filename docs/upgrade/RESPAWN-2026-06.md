# llamafile Upgrade — Respawn Plan & Report (v3, 2026-06-28)

> Comprehensive findings + plan for respawning the llama.cpp→llamafile port, absorbing the embedded jinja engine, ik_llama.cpp's optimized CPU kernels, and golem's low-RAM machinery.
>
> **v3 corrections from v2** (verified live against the repo this session):
> - **The GPU-under-cosmo blocker is largely dissolved.** Synced `main` (v0.10.0) already ships a *proven self-contained-dylib* Metal loader (`llamafile/metal.c:26-29`) that solves the cosmo_dlopen NULL-pointer problem at the root — by compiling ggml core *into* the dylib so the static struct-initializer function pointers resolve internally and can never come back NULL. The `upgrade` branch's hand-rolled `ggml_metal_link` per-function trampoline (Jan-11, built on the pre-reload base) is **superseded**, not the path forward. `git grep ggml_metal_link` on `main` = 0 hits; `main` also carries `0849f322c "Get CUDA and Metal GPU working in whisperfile"` + `c7d7e3e "vulkan dylibs"`. **This demotes R1 and merges Phase 1 into Phase 2** (see §2.2, §4, §5, §7).
> - **Phase 0 is DONE** (this session). The 6 disk-only `upgrade` commits + applied state are on `origin/upgrade` (+ tag `archive/christmas-upgrade-2026-06`); the local-only submodule pin `3cea47541` is on `ochafik/llama.cpp-private` (synced from ggml-org first) + bundled under `llama.cpp.patches/christmas-archive-2026-06/`. R8 is closed.
>
> **v2 corrections from v1** (v1 was synthesized from prompt-history only and got 4 things wrong; all corrected here against recovered real transcripts + commit-log forensics):
> - The Dec-26 sessions are the **wikifile** project, not the port. The port is the `upgrade` branch (Dec 27→Jan 11), whose CC transcripts are **purged** — recovered below from the commit log + working-tree forensics.
> - The **minja→jinja "replacement attempt" was not a thing** in the prior sessions. llama.cpp *upstream* replacing its vendored minja with `common/jinja/` (#18462) is real and is what this port absorbs; the user's separate `ochafik/minja` fork is a different, successful effort.
> - The **Metal/CUDA blocker is now concretely understood** (`ggml_metal_link` trampoline, reverse-engineered) and is **WIP / unproven**, not a black box. *(v3: now superseded — see above.)*
> - The **server crash left no forensic trace** in any commit — treat as a fresh reproduction task.

---

## 0. TL;DR

Late Dec 2025 → Jan 11 2026, an attempt to port modern llama.cpp into llamafile got the **CPU `simple` binary to build and (per the planning notes) load a model**, then stalled. The work lived on the `upgrade` branch — 6 commits + the submodule pin `3cea47541` were disk-only, the applied patch state uncommitted. **All of that is now backed up (Phase 0 done this session).**

The respawn is substantially de-risked versus last time — and **the v2 gating blocker (GPU-under-cosmo) has largely evaporated**:

1. **llamafile reloaded (v0.10.0).** Vendoring is now a **git submodule + patch layer** with per-component `BUILD.mk`. The old flattening/`fix-includes` nightmare is transformed (not gone — `cosmocc mkdeps` still demands full-path includes — but no longer the dominant cost).
2. **GPU-under-cosmo is already solved on `main`.** v0.10.0 compiles ggml core *into* a self-contained Metal/CUDA/Vulkan dylib and loads it with `cosmo_dlopen` + a handful of `cosmo_dlsym` entry points — so the static-initializer function pointers resolve *inside* the dylib and never come back NULL. This is a fundamentally more robust mechanism than the `upgrade` branch's `ggml_metal_link` per-function trampoline (which was invented Dec/Jan to paper over the cross-module NULL problem on the pre-reload base). **We branch off `main`, inherit the working loader, and discard the trampoline.** The remaining GPU question is merely "does the v0.10.0 loader still build/load after the submodule bump" — a Phase-2 verification, not a gating spike.
3. **All three desired additions are pre-decoupled and portable:** jinja engine (absorbed by submodule bump past #18462), ik_llama `iqk/` (~40k LOC, MIT, zero inline asm, same per-ISA multi-TU shape as llamafile's `sgemm`), golem low-RAM (std-only header lib + one C-ABI hook).

**Recommended path:** branch off synced `main` (v0.10.0, working GPU loader) → bump the submodule to current master + re-apply the *forensically-useful* patches (absorbs jinja + 6 months upstream) → fix the app layer (reproduce the server crash) → confirm GPU still loads → layer on ik_llama kernels → layer on golem low-RAM → verify + merge. The old "prove GPU first" spike is no longer the spine; **CPU build + submodule bump is the spine, GPU rides along on the inherited v0.10.0 loader.**

---

## 1. State of the world (2026-06-28)

### 1.1 The fork (`/Users/ochafik/github/llamafile`)
- **Remotes:** `origin`=`ochafik/llamafile`; `upstream`=**`mozilla-ai/llamafile`** (project moved jart→Mozilla — correct); `llamacpp`=`ggerganov/llama.cpp`.
- **`main` synced this session:** `9509d9111`→`3917d4739` (== origin == upstream; v0.10.0 "reloaded" #867). Pure fast-forward, 42 commits, no force/rebase/push. Still checked out on `upgrade` @ `07b0a1b43`.
- **Worktrees (only 3 exist):**
  - **main checkout** [branch `upgrade`, HEAD `07b0a1b43`, 2026-01-12] — **the most advanced state; carries the applied patch state in its working tree.**
  - `llamafile-chat/` [branch `chat-support`/`peg-migration-squashed`, HEAD `e27693182`, 2025-12-27] — separate effort; submodule at **clean newer pin `01150c8d6` (b7482-135, post-split Metal layout)** → the one reusable newer-upstream base.
  - `llamafile-server/` [branch `feature/tool-call-support`, HEAD `9509d9111`, 2025-12-27] — abandonware; carries the **rejected flatten-and-rename** patch (1513 deletions) + a 72-byte stub `llama.cpp.a`. Not reusable.
  - The `llamafile-upgrade` worktree referenced in `UPGRADE_GUIDE.md` is **gone**; its product survives on `upgrade`.
  - **No worktree has a working binary** — `o/` dirs are empty; the Jan-7 `simple` build was cleaned out.
- **Disk-only backup dirs (untracked, not in any inventory until v3):** `llama.cpp_/` (127M), `stable-diffusion.cpp_/` (29M), `whisper.cpp_/` (5.2M), all dated **Jan 7** — flatten/rename-era snapshots taken before the submodule cutover. Ungitted; catalogue or delete (they are *not* in the critical path, but 161MB of silent disk state is a reproducibility smell).

### 1.2 The `upgrade` branch — the port effort (14 commits, 6 unpushed)
Recovered from the commit log (the CC sessions Dec 27→Jan 11 are purged):

| # | SHA | Date | Subject | Notable |
|---|-----|------|---------|---------|
| 1 | `860330e63` | 12-27 | update llama.cpp submodule to upstream master | pin `8b3befc0e`→`06705fdc` |
| 2–4 | `38841c942`,`593ae0bca`,`12097dd84` | 12-30 | wip upgrade; full-path includes for mkdeps; ggml header paths | the `mkdeps`/`fix-includes` fight |
| 5 | `823a870e1` | 12-30 | complete API migration, remove stub headers | 37 files; rewrites `simple.cpp`,`cuda.c`,`metal.c` |
| 6 | `b5dfcae79` | 01-08 | Metal GPU support for new structure | **+10040/-524, 81 files**; authors the full `llamafile/server/*` tool-call stack (`chat.cpp`,`chat-parser.cpp`,`peg-parser.cpp`,…) |
| 7–8 | `1fb9e9f79`,`fdf1ba2d5` | 01-08 | restore float.h; remove `-Illamafile`, full-path includes | |
| 9 | `f3f0f5d60` | 01-11 02:35 | complete upgrade with Metal backend fixes | server API migrations (KV `llama_memory_*`→`llama_kv_self_*`, `vendor/minja/`→`common/minja/`, unconditional jinja templating) — **unpushed** |
| 10 | `6fe55a833` | 01-11 02:52 | update llamafile_sgemm API for new signature — **unpushed** | |
| 11 | `635e1953e` | 01-11 13:13 | **wip:** disable Metal registration (cosmo_dlopen) — **unpushed** | *"Static struct fn pointers in dynamically loaded Metal dylib are NULL… TODO: port ggml_metal_link"* |
| 12 | `a720f9c8d` | 01-11 13:22 | implement `ggml_metal_link` pattern — **unpushed** | the trampoline (see §2.2) |
| 13 | `6a6eefbaf` | 01-11 13:22 | docs: add patches for ggml_metal_link — **unpushed** | `new-patches/*.patch` (+245) |
| 14 | `07b0a1b43` | 01-11 13:23 | update submodule with Metal API patches = **tip** — **unpushed** | submodule→`3cea47541` |

The **Jan-11 13:13→13:23 cadence** (`wip:` disable → implement → docs → pin, in 10 min) is a *commit-before-losing-it* snapshot, not a verified-working state.

> **v3 update — Phase 0 done:** the 6 formerly-disk-only commits are on `origin/upgrade`; the tip is preserved at tag `archive/christmas-upgrade-2026-06` → `07b0a1b43`; the submodule pin `3cea47541` is on `ochafik/llama.cpp-private` (tag+branch `archive/christmas-pin-3cea`); the applied state below is captured as patches + a tar + a thin bundle under `llama.cpp.patches/christmas-archive-2026-06/` (committed + pushed). The live working tree still carries the applied state for reference. Also untracked since v2: `llamafile/server/chat-parser-xml-toolcall.cpp`.

**Applied state in the working tree (NOT committed in-place — the original reproducibility hazard, now captured to patches):**
- `llama.cpp/ggml/src/ggml-metal/ggml-metal.m` **+213/-100** (the cosmo_dlopen / `ggml_metal_link` patch)
- `ggml/src/ggml-backend.cpp` +6/-1, `ggml/src/ggml-backend-impl.h` +1
- untracked: `cores.h/.cpp`, `ggml-barrier.cpp`, 12× `ggml-vector-*.c`, 8× `ggml-quants-*.c`, `tools/`
- orchestrator `llama.cpp.patches/{apply-patches-native.sh, UPGRADE_GUIDE.md, fix-includes.sh}` — **untracked**
- live debug block in `llamafile/metal.c`: `[MAIN DEBUG] dev=%p supports_op=%p get_buffer_type=%p buffer_from_host_ptr=%p` right after `ggml_backend_register(reg)` — instrumentation probing whether the iface pointers survived `cosmo_dlopen`.
- **Submodule pin `3cea47541`** = local-only ("feat: add ggml_metal_link and ggml_backend_api for cosmopolitan"), 3 ochafik commits on top of upstream `7242dd967` (#12621, early Jan 2026, ggml 0.9.x). ~6 months / ~2,280 build tags behind current master.

### 1.3 llama.cpp upstream delta (baseline → now)
Baseline `b7552` (2025-12-27) → master `b9833` (2026-06-28); **ggml 0.9.5 → 0.15.3.** Headlines:
- **minja → embedded jinja engine** (#18462, 2026-01-16). `vendor/minja/` deleted; `common/jinja/` (13 files) added. Recursive-descent, input-marking security. **No CLI break** (`--jinja` defaults OFF). *Impact: HIGH but mechanical.*
- **Autoparser** (#18675, 2026-03-06): PEG tool-call parser; `common/chat-parser*`/`chat-peg-parser*`/`chat-parsers/` split.
- **Server breaks:** `/api/*` **removed** (#22165); router mode; `--clear-idle`→`--cache-idle-slots`; `--agent`; CVE-2026-21869; WebUI separate artifact (`LLAMA_BUILD_UI`).
- **Core API:** `flash_attn` bool→enum; `kv_unified`; `defrag_thold` deprecated; vocab refactor; sampler-param refactor (#22233); backend-side sampling incl. MTP draft (#23287).
- **New archs:** GLM-5.2 DSA, DeepSeek V3.2 DSA, Cohere2-MoE, EAGLE3, DFlash, tensor parallelism (#19378), MTP variants.
- **Silent re-tests:** default ftype **Q5_1→Q8_0** (#20828); metadata→`LLM_KV` (#24802). `GGUF_VERSION` still 3.
- **Backends:** `ggml/src/ggml-cpu/` confirmed (with a `llamafile/` subdir — our upstream integration; #19709 powerpc FP16 MMA is llamafile-authored). New OpenVINO + WebGPU (gate OFF for cosmo). Metal changes touch exactly the `ggml_metal_link` surface we patch.
- **Build:** vendored deps bumped (`cpp-httplib` 0.47.0, LibreSSL 4.3.2, BoringSSL); `libmtmd` separately-linkable.

### 1.4 ik_llama.cpp kernels (`/Users/ochafik/github/ik_llama.cpp`)
Local `ab0f22b81` (May 8); upstream `3f40e73c3` (May 30). Deep fork pinned to a **pre-Aug-2024 base** (no `ggml/src/ggml-cpu/`). All optimized CPU kernels in **`ggml/src/iqk/` (~40k LOC, MIT, no inline asm, no glibc deps, pure intrinsics)**. ~50 new `GGML_TYPE_IQ*` enums. **Compile-time ISA gating** (`iqk_config.h`) — no runtime `cpu_detect()`; needs per-ISA multi-TU + dispatch (llamafile's `sgemm` shape). MoE via `iqk_mul_mat_moe` + `iqk_moe_fused_up_gate`.

### 1.5 golem low-RAM (`/Users/ochafik/github/golem-2b/`, worktree of `golem.cpp`)
golem is a **fork of ik_llama.cpp** (carries `iqk/` too). Engine code in **`src/golem/` (~12.8k LOC)**; **every header except `golem_hook.{h,cpp}` is header-only + std-only, zero ggml deps** — a closed world. Three `PoolMode`s: `MMAP_MADVISE` (Linux: mmap + `MADV_DONTNEED`, RSS bounded by **cgroup**), `EXPLICIT_PAGED` (macOS: **no mmap**, `pread` on `F_NOCACHE` bypass fd into a bounded arena). Per-OS split is deliberate — do not unify.

---

## 2. What the prior effort did & why it stalled (lessons)

### 2.1 The two parallel tracks (corrected)
- **wikifile** (Dec 26, branch `wikifile`, self-marked "Broken", unmerged): ZIM-Wikipedia augmentation + a streaming tool-call fight. Built a working ZIM reader (5/5 tests) + server endpoints, but **llamafile's `/v1/chat/completions` explicitly lists `tools`/`functions`/`tool_choice`/`function_call`/`parallel_tool_calls` as unsupported** and streams only `delta.content` — so native OpenAI tool streaming started from a deficit and the session fell back to a client-side pattern detector. Real lessons here: cosmocc `mkdeps` rejects `.c`-including-`.c`; C→C-symbol linkage needs `extern "C"` (in-session `::`-prefix was a stopgap); **an uncommitted submodule edit (`BUILD.mk`+`zim.a`) made it non-reproducible**.
- **the upgrade / port** (Dec 27→Jan 11, branch `upgrade`): the llama.cpp modernization. Sessions purged; see §1.2 for the recovered commit record.
- **separate minja fork** (`ochafik/minja`, Dec 20–25): PR curation (#11→#17 DSML merged, #12/#14/#15 CI+filters, #21 Windows-fix) + **thinking-mode capability detection** (commit `6925b09`, 5 flags, dual-pattern DeepSeek-R1 detector, GLM-4.7 `clear_thinking` captured). This is a *successful* side effort, not part of the port.

### 2.2 GPU-under-cosmo — SOLVED on `main`; the trampoline is superseded (v3)
> **Headline correction (v3, verified live).** The entire v2 framing of §2.2 — "the blocker is the trampoline; the cheapest kill is to build it and read `[MAIN DEBUG]`" — is **obsolete**. Synced `main` (v0.10.0) already ships a *working* GPU loader using a **different and structurally superior mechanism**, and the `upgrade` branch's trampoline was built on the *pre-reload* base before that mechanism existed.
>
> **`main`'s mechanism — self-contained dylib** (`llamafile/metal.c`, evidence): the GPU backend is compiled into a **self-contained dylib that includes ggml core**, loaded with `cosmo_dlopen()`; llamafile then `cosmo_dlsym`s only ~4 *entry-point* symbols (`ggml_backend_metal_init`, `ggml_backend_is_metal`, `ggml_backend_metal_reg`, `ggml_log_set`) and calls `ggml_backend_register(reg)`. Because the dylib contains ggml core, the static struct-initializer pointers (`.buffer_from_host_ptr = &…`) resolve to addresses *inside the dylib* — **the NULL-pointer failure mode is structurally impossible**, not patched around. `git grep ggml_metal_link` on `main` → **0 hits**. Companion commits: `c7d7e3e "vulkan dylibs"`, `0849f322c "Get CUDA and Metal GPU working in whisperfile"`.
>
> **Consequence:** do **not** carry `ggml_metal_link` forward. Branch off `main`, inherit its loader, and the only GPU work is verifying the loader still builds/loads after the submodule bump (Phase 2). R1 drops from High/Med to Low. The captured trampoline (`llama.cpp.patches/christmas-archive-2026-06/`) is kept for forensics only.

The reverse-engineered trampoline (below) is retained for the historical record / in case the self-contained-dylib path ever regresses.

Upstream `ggml-metal.m` installs function pointers in a static struct initializer:
```c
static const struct ggml_backend_device_i ggml_backend_metal_device_i = {
    .buffer_from_host_ptr = &ggml_backend_metal_buffer_from_host_ptr, ... };
```
Under `cosmo_dlopen` those member references resolve to **NULL** when the main binary later dereferences `dev->iface.buffer_from_host_ptr`.

**The fix (3 parts, all `#ifdef __COSMOPOLITAN__`):**
1. **Main binary** publishes a table: `extern "C" const struct ggml_backend_api *ggml_backend_api(void){ return &kGgmlBackendApi; }` (17+ function pointers).
2. **Dylib** (`ggml-metal.m`): `void ggml_metal_link(const struct ggml_backend_api *b){ g_backend=b; }` + `#define ggml_blck_size g_backend->ggml_blck_size` (×17) redirecting every `ggml_*` call through the live table, plus a `printf`/`exit` bridge through `g_backend->write`.
3. **Loader** (`llamafile/metal.c`, `LinkMetal()`): `ggml_metal.ggml_metal_link = cosmo_dlsym(lib,"ggml_metal_link"); … ggml_metal.ggml_metal_link(ggml_backend_api()); ggml_backend_register(reg);`

This is the genuine llamafile/cosmopolitan idiom (matches the CUDA bridge). **Conceptionally sound.** The open question — the *only* one that matters for Phase 1 — is whether, at runtime, `dev->iface.{supports_op,get_buffer_type,buffer_from_host_ptr}` come back **non-NULL** after `ggml_metal_link(ggml_backend_api())`.

### 2.3 Why it stalled (evidence-based)
1. **GPU-under-cosmo unverified.** `wip:` disable → implement in 9 min; working tree still has live debug instrumentation; **no test/log/"works" note anywhere**. Verdict: *unproven until a fresh build proves otherwise.*
2. **The `/v1/chat/completions` crash left zero forensic trace** in any commit (no disabled endpoint, no assert/abort/signal handler, no "FIXME crash"). It lived only in the purged sessions → **treat as a fresh reproduction task.**
3. **Patch layer not reproducible from a fresh clone** — 3 independent reasons: (a) applied state never committed; (b) `apply-patches-native.sh`/`UPGRADE_GUIDE.md` untracked; (c) the script knowingly skips 6 families of `[jart]` semantic patches ("Semantic changes from old patches need manual porting!").
4. **Never merged; 6 commits disk-only.**

### 2.4 Lessons (carry forward)
1. **~~Prove GPU-under-cosmo FIRST~~** *(v3: retracted)* — the prior stall was 85%-CPU-then-GPU-wall, which is why v2 made GPU the gate. But v0.10.0 already solved GPU-under-cosmo (self-contained dylib, §2.2), so the gate is gone. The carry-forward lesson is narrower: **branch off the latest `main`, not a stale base** — the trampoline existed only because the prior effort forked *before* the reload and never rebased forward. Re-basing onto current upstream would have surfaced the solved mechanism for free.
2. **Commit the applied state, always.** The patch layer must reproduce from a fresh clone; disk-only working trees get cleaned (the Jan-7 `simple` build is already gone).
3. **`cosmocc mkdeps` demands full-path includes + SRCS-enumerated headers** — a recurring tax (bit wikifile `zstddeclib.c`, then dominated Dec-30/Jan-8). Script it (`fix-includes.sh`), don't hand-edit.
4. **C→C++ symbol linkage under cosmocc needs `extern "C"`** — not `::`-prefix stopgaps.
5. **One branch, one worktree.** The vanished `llamafile-upgrade` worktree and the 3 scattered worktrees diluted the effort.
6. **Don't commit WIP as "complete."** The Jan-11 cadence made a snapshot look like a finish line.

---

## 3. The three additions — nature & strategy

### 3.1 (A) Upstream modernization — incl. the embedded jinja engine
**Mechanism:** bump the submodule past #18462 and wire `common/jinja/` into the build — absorbing jinja *and* 6 months of upstream in one act.
- Delete `vendor/minja/`; add `common/jinja/*` (13 files) + the `chat-parser*`/`chat-peg-parser*`/`chat-parsers/`/`json-partial*` split; reconcile `common/chat.cpp`; re-pin `cpp-httplib`/LibreSSL/BoringSSL; gate OFF `LLAMA_BUILD_UI`/openvino/webgpu/hexagon/sycl.
- App-layer fixups in `llamafile/`: `flash_attn_type`, sampler refactor, `/v1`-only server, `--cache-idle-slots`, `metal.c`/`cuda.c` signature changes.
- Re-test: default ftype Q8_0; custom chat templates (stricter engine — input marking).

### 3.2 (B) ik_llama optimized CPU kernels (source: `ik_llama.cpp` `iqk/`)
> **v3 correction (verified against `main`).** Two big things v2 got wrong:
> 1. **The per-ISA build plumbing is ALREADY DONE in v0.10.0.** `main` ships `llamafile/iqk_mul_mat_amd_avx2.cpp`, `_amd_zen4.cpp`, `_arm82.cpp` (thin `#include "iqk_mul_mat.inc"` per-ISA wrappers) + a **3061-line `llamafile/iqk_mul_mat.inc`** carrying the dispatcher + kernels, already adapted to llamafile include paths. The existing `.inc` switch already covers Q2_K..Q6_K, IQ4_XS, IQ3_S/XXS, IQ2_S/XS/XXS, Q4_0..Q8_0. So "Cost = build plumbing" describes *completed* work; Phase 4 starts non-zero. The remaining kernel work is narrow: **add the `iqk_quants` types (IQ2_K/IQ3_K/IQ4_K — confirmed absent from the `.inc`)** as new cases + their `block_iqN_k` structs (into an `iqk-common.h`-style include, not upstream `ggml-common.h`). `iqk_quantize.cpp` / flash-attn are correctly deferred (the `.inc` already avoids the `iqk_quantize.h` dependency).
> 2. **The real hazard is `GGML_TYPE_COUNT`, not the enum count.** Modern ggml already shares the 9 standard IQ imatrix types (IQ2_XXS..TQ2_0) at identical values — no graft needed there. But ik_llama's *new* types run sparsely up to `Q8_K_R8 = 399`, pushing `GGML_TYPE_COUNT` from **39 → 400** (the v2 "~50 enums" is 3-4× low — it ignores all `_R4`/`_R8`/`_R16` packed variants). That count is baked into compile-time static arrays: `type_traits[GGML_TYPE_COUNT]` in `ggml.c`, `type_traits_cpu[GGML_TYPE_COUNT]` in `ggml-cpu.c`, and per-type arrays in backends (Vulkan `mul_mat_l[GGML_TYPE_COUNT]`, etc.). Critically, **`gguf.cpp` rejects any tensor with `type >= GGML_TYPE_COUNT` as corrupt** — so an IQ2_K model fails at the GGUF reader *before any kernel runs*. This makes the type-table expansion a **hard prerequisite for all of Phase 4**, not a "graft enums first, expand later" step.

**Port-first shortlist** (add to the existing `.inc`): IQ2_K → IQ3_K → IQ4_K (DeepSeek/Qwen MoE) + their block structs + `iqk_moe_fused_up_gate`. For a minimal first cut, raise `GGML_TYPE_COUNT` only to **142** (cover IQ2_K/IQ3_K/IQ4_K) and defer the packed `_R*` variants (a second round of the same surgery).
**Prereq subphase (gate before any kernel work):** expand `GGML_TYPE_COUNT` + populate `type_traits`/`type_traits_cpu` sparse initializers + relax `gguf.cpp` validation. **Gate:** `ggml_type_name(GGML_TYPE_IQ2_K) == "iq2_k"` *and* an IQ2_K GGUF loads without a gguf-validation abort.
**Defer:** `iqk_quantize.cpp`, flash-attention (scheduler integration), Trellis/Bitnet/fused-delta-net/model-specific MoE glue, MLA, the `_R*` packed variants.

### 3.3 (C) golem low-RAM (source: `golem-2b` `src/golem/`)
> **v3 correction (verified) — the cosmo OS-guard trap.** golem's per-OS split is written `#if defined(__APPLE__) / #elif defined(__linux__) / #else`. **Under cosmocc BOTH macros are false** (`normalize.inc` `#undef __linux__`; `__APPLE__` never set by the cross-compiler) → the code falls through to the `#else` branch, which does a bare `dup(sfd)` with **no `F_NOCACHE` bypass flag** (`golem_hook.cpp:1256-1274`; same pattern `golem_engine.h:489-497`; `madvise_dontneed` `#ifdef __linux__` at :278 → permanent no-op). It **compiles cleanly, prefills the arena, produces byte-correct output, logs nothing** — but pread now goes through the macOS UBC and **RSS grows unbounded as unique experts load**, silently violating the one invariant golem exists for. The v2 phrase "~15-line `F_NOCACHE` setup" implies a direct copy; those lines are **not cosmo-portable as-is**. Fix: add `#elif defined(__COSMOPOLITAN__)` branches dispatching on runtime `IsXnu()`/`IsLinux()` (from `<libc/dce.h>`, already available) — `F_NOCACHE`/`O_DIRECT`/`MADV_DONTNEED` are all runtime-resolved cosmo constants (`-1`/`0` when unsupported), so the syscalls themselves pass through faithfully; only the compile-time *selection* is broken.
>
> **Also (verified):** golem only matters for llamafile's **external large-MoE** case (`-m big.gguf` on a Mac with RAM ≪ model). Bundled APE models are small dense (≤10GB) — mmap + OS cache suffices, golem adds nothing. And the bundled path mmaps the *binary itself* (`MAP_SHARED` on `prog`'s fd, `llamafile.c:219`) outside `ml.use_mmap`'s control — so `use_mmap=false` won't suppress it; the golem bypass fd must be opened on the same file. The six-item minimal surface below is otherwise **confirmed tight** — nothing in it is further deferrable without breaking bounded RSS.

**3-layer port:** (A) drop-in std-only — `golem_paged_io.h`, `golem_residency.h`, `golem_floorplan_policy.h`, `golem_kv_quant.h`, `golem_kv_cache.h`, `golem_kv_predictor.h`, `golem_expert_prefetch.h`, `golem_contig.h`; (B) std/POSIX + adapter — `golem_slot_pool.h`, `golem_scheduler.h`, `golem_kv_session.h`, `golem_kv_page_pool.h`; (C) rewrite `golem_hook.cpp`'s ggml poking for llamafile's loader (~6-function surface).
**Minimal first cut ("bounded RAM on macOS"):** Layer A + `slot_pool` + `scheduler` + thin hook + ~15-line `F_NOCACHE` setup + the **placeholder-`data` loader edit**. Skip §A KV paging / §B PolarQuant / session file until the base works.
**Critical gotchas:** macOS UBC defeats `F_NOCACHE` on mmap'd files → **no mmap + placeholder `data` ptr** (measure `vmmap` first); per-shard `(fd,offset)` keyed by tensor **name**; `slot_bytes`=MAX slab; two-phase prefetch runs `load()` **outside the lock**; ship per-OS (Linux = mmap+`MADV_DONTNEED`+cgroup; macOS = `F_NOCACHE`+no-mmap+placeholder-data).

---

## 4. Recommended plan — phased

> Principle (v3): **the spine is "branch off latest `main` → bump submodule → build CPU → fix forward"; GPU rides along on v0.10.0's inherited loader; then layer the two additive ports independently.** Binary gate per phase. The v2 "prove GPU first" gate is removed (§2.2).

**Phase 0 — Backup & foundation — ✅ DONE (2026-06-28)**
- ✅ 6 disk-only `upgrade` commits → `origin/upgrade`; tip preserved at tag `archive/christmas-upgrade-2026-06` (`07b0a1b43`).
- ✅ Local-only submodule pin `3cea47541` → `ochafik/llama.cpp-private` (tag + branch `archive/christmas-pin-3cea`; fork synced from ggml-org first) + thin bundle.
- ✅ Applied state captured: `superproject-applied.patch`, `submodule-applied-tracked.patch`, `submodule-untracked.tar.gz` under `llama.cpp.patches/christmas-archive-2026-06/` (committed + pushed; README has restore steps).
- **Remaining (cheap, do at Phase-2 start):** snapshot the `llamafile-chat` `01150c8d6` pin (reusable newer base); catalogue/delete the 161MB `*_/` disk backups; branch `upgrade-2026` off synced `main` (`3917d4739`). One branch, one worktree.

**Phase 1 — ~~GPU-under-cosmo~~ → folded into Phase 2 (v3)**
- The v2 gating spike is gone: v0.10.0 `main` already loads GPU correctly (self-contained dylib, §2.2). There is no separate "prove GPU" phase. GPU verification becomes a checkpoint *inside* Phase 2 ("does `main`'s loader still build/load after the submodule bump?"). CPU-only-v1 is no longer a fork to pre-agree — GPU is inherited, not gambled on.

**Phase 2 — Branch off `main` + submodule bump + patch re-application (absorbs jinja + 6mo + working GPU)**
- Branch `upgrade-2026` off synced `main` (`3917d4739`, v0.10.0, **carries the working GPU loader**). Bump its submodule from `d6d899580`→current master `b9833`.
- Re-apply only the *still-relevant* patches via `apply-patches-native.sh`; **drop the `ggml_metal_link`/`ggml_backend_api` cosmo trampoline patches** (superseded by the inherited self-contained-dylib loader — graft from `upgrade` only what `main` lacks). Fold `common/jinja/*`+`chat-*` into the build. Re-pin `cpp-httplib`/LibreSSL/BoringSSL; gate OFF `LLAMA_BUILD_UI`/openvino/webgpu/hexagon/sycl.
- **GPU checkpoint (inside this phase):** build the Metal dylib against the bumped submodule; confirm `ggml_backend_metal_reg` resolves and one Metal-assisted inference runs. If `main`'s loader needs adjustment for new ggml Metal changes (#1.3 notes Metal touched the loader surface), that's a bounded fix on a *working* base — not a from-scratch spike.
- **Exit gate:** `o/llamafile/simple` builds on CPU **and** a Metal-assisted run succeeds; `make setup` reproducible from a fresh clone.

**Phase 3 — App-layer fixups + server crash**
- Fix `metal.c`/`cuda.c`/`server/`/`chatbot_*` against the new API. **Reproduce the `/v1/chat/completions` crash** fresh and fix it (no prior trace exists). Verify `--jinja` templates render.
- **Exit gate:** server serves `/v1/chat/completions` for a tool-calling template end-to-end.

**Phase 4 — ik_llama kernels (CPU perf)** — *starts non-zero; per-ISA plumbing already in v0.10.0 (§3.2)*
- **4a (prereq, gated):** expand `GGML_TYPE_COUNT` (39→142 for the minimal IQ2_K/IQ3_K/IQ4_K cut) + populate `type_traits`/`type_traits_cpu` sparse initializers + relax `gguf.cpp` `type >= GGML_TYPE_COUNT` validation. **Gate:** `ggml_type_name(GGML_TYPE_IQ2_K) == "iq2_k"` and an IQ2_K GGUF loads without abort.
- **4b:** add IQ2_K/IQ3_K/IQ4_K cases + `block_iqN_k` structs to the existing `iqk_mul_mat.inc`; (defer `_R*` packed variants to a second round of 4a+4b).
- **Exit gate:** `llama-bench` shows ik GEMM active + faster than stock on Q4_K/IQ2_K; **bit-exact** vs upstream ik_llama.

**Phase 5 — golem low-RAM (big models on small RAM)**
- **First, before any porting:** convert golem's `#if defined(__APPLE__)/#elif defined(__linux__)` OS guards to `#elif defined(__COSMOPOLITAN__)` + runtime `IsXnu()`/`IsLinux()` (`golem_hook.cpp:1256-1274`, `golem_engine.h:489-497`, `madvise_dontneed:278`) — else bounded RSS is silently defeated (§3.3). Open the golem bypass fd on the model file llamafile actually uses (binary fd for bundled; external fd for `-m`).
- Minimal macOS bounded-RSS cut → Linux mmap+DONTNEED+cgroup → (optional) §A KV paging + §B PolarQuant.
- **Exit gate:** a 262GB-class MoE runs in bounded RSS (~30–40GB) on a 96GB Mac, no swap, byte-coherent — and crucially, **RSS stays bounded under sustained decode** (measure `vmmap` file-backed-vs-anon *after* many unique experts have cycled, not just post-load — the cosmo-guard bug shows up only as the cache fills).

**Phase 6 — Verify, parity, merge**
- Fresh-clone reproducibility; token-for-token parity vs upstream + ik_llama; low-RAM RSS measurement; APE cross-arch smoke (x86_64+arm64). Merge `upgrade-2026`→`main`.

---

## 5. Risk register

| # | Risk | L | I | Mitigation |
|---|---|---|---|---|
| R1 | ~~GPU-under-cosmo `dev->iface` NULL~~ **(v3: largely retired)** — risk is now narrowly "v0.10.0's self-contained-dylib loader needs adjustment for new ggml Metal changes after the bump" | Low | Med | Inherit `main`'s working loader; verify at the Phase-2 GPU checkpoint; bounded fix on a working base |
| R2 | Patch layer not reproducible from fresh clone | High | High | Phase-0 capture the applied state to a committed patch; fresh-clone test each phase |
| R3 | **`GGML_TYPE_COUNT` 39→400 surgery** (v3: re-rated, was "header graft") — static `type_traits[]`/`type_traits_cpu[]` arrays + `gguf.cpp` validation reject ik types until expanded; blocks ALL of Phase 4 | Med | **High** | Phase-4a prereq subphase with its own gate; minimal cut raises COUNT to 142 only; `gguf.cpp` guard relaxed first |
| R4 | ~~Per-ISA multi-TU build plumbing for iqk~~ **(v3: mostly DONE in v0.10.0)** — `iqk_mul_mat_amd_avx2/zen4/arm82.cpp` + `iqk_mul_mat.inc` already exist | Low | Low | Add new type cases to the existing `.inc`; no new fat-binary machinery needed |
| R5 | `/v1/chat/completions` crash reintroduces after autoparser | Med | Med | Port server against current master; add a tool-call template to the test suite |
| R6 | golem placeholder-`data` edit fights llamafile's loader (incl. bundled-model binary mmap outside `ml.use_mmap`) | Med | Med | Confine to EXPLICIT_PAGED behind a flag; open bypass fd on the file llamafile uses; `vmmap` immediately |
| R10 | **golem cosmo OS-guard trap** (v3, verified): `#if __APPLE__/__linux__` both false under cosmocc → bare `dup()` no-`F_NOCACHE` `#else` branch → bounded RSS silently defeated, byte-correct output, no error | High | High | Convert to `#elif __COSMOPOLITAN__` + `IsXnu()`/`IsLinux()` runtime dispatch before porting; Phase-5 gate measures RSS *after sustained decode*, not just post-load |
| R7 | Scope creep (Trellis/Bitnet/MLA/full TP) | Med | Med | Strict port-first/defer lists |
| R8 | ~~Losing the 6 unpushed commits + `3cea47541` (disk-only)~~ **(v3: CLOSED 2026-06-28)** | — | — | Done: `origin/upgrade` + tag; pin on `ochafik/llama.cpp-private` + bundle; applied state in `christmas-archive-2026-06/` |
| R9 | **Stale-base drift** (the root cause of the trampoline detour): re-doing work upstream already solved by forking off an old base and not rebasing | Med | Med | Always branch off latest `main`; before hand-rolling a cosmo workaround, `git grep`/`git log` upstream for an existing mechanism (the §2.2 lesson) |

---

## 6. Verification strategy
- **Per-phase binary gates** — no phase starts before the prior gate is green.
- **Parity:** token-for-token vs upstream llama.cpp (CPU) and ik_llama (ik kernels) on Q4_K/IQ2_K/IQ3_K across ≥2 archs.
- **Reproducibility:** `make setup && make` from a fresh clone each phase; no disk-only artifacts in the critical path.
- **Low-RAM:** `vmmap`/smaps file-backed-vs-anon + RSS cap under a representative MoE.
- **APE:** cross-arch smoke (x86_64+arm64) before merge.

---

## 7. Open decisions
**Resolved in v3 (by the §2.2 finding + Phase-0 completion):**
1. ~~Phase-1 first move (build trampoline / read `[MAIN DEBUG]`)~~ → **moot.** GPU is solved on `main`; no GPU spike. The `[MAIN DEBUG]` probe tests a dead mechanism.
2. ~~v1 GPU scope / CPU-only-v1~~ → **resolved: ship GPU.** It's inherited from v0.10.0, not a risk to defer. CPU-only is no longer a meaningful fork.
5. ~~Salvage strategy (cherry-pick trampoline onto v0.10.0)~~ → **resolved: branch off `main`, discard the trampoline.** Graft from `upgrade` only the non-GPU work that `main` lacks (app-layer migrations, ported per-ISA sources). Don't carry `ggml_metal_link`.

**Still open:**
3. **ik_llama source:** `ik_llama.cpp` `iqk/` (authoritative, MIT) vs `golem.cpp`'s copy. *Recommend ik_llama.cpp.*
4. **golem scope:** minimal bounded-RSS vs full (KV paging + PolarQuant + 1M-context). *Recommend minimal first.*
6. **Submodule target:** current master `b9833` (bleeding) vs a tagged release. *Lean: a recent tagged release for the first green build, then forward to master — limits the "which of 6 months of changes broke it" search space (see R-highest, §5 discussion).*

---

## 8. Appendix — research artifacts
- **v3 verification provenance (2026-06-28):** the GPU/Metal correction was verified live against `main` (`llamafile/metal.c`, `git grep ggml_metal_link`); §3.2 ik_llama and §3.3 golem corrections came from two independent Explore subagents (ik_llama: `GGML_TYPE_COUNT` 39→400 hazard + existing per-ISA `.inc`; golem: the cosmo `#if __APPLE__/__linux__` dead-branch trap). Phase-0 backup artifacts live under `llama.cpp.patches/christmas-archive-2026-06/` (committed). A parallel clolo (Opus-4.8) consult was set up (`CLolo_BRIEF.md`/`CLolo_TASK.md`) but hit a usage limit before writing `CLolo_REPORT.md`; its 3 subagents finished and its one decisive early finding (the v0.10.0 self-contained-dylib loader) is the §2.2 headline, independently confirmed here.
- CC session archaeology v2 (real transcripts): `/tmp/llamafile-sessions-report-v2.md`; per-track detail `/tmp/{llamafile,minja}-track-findings.md`; prompt narrative `~/.claude_/history.jsonl`.
- `upgrade` branch forensics (lost-session recovery): `/tmp/upgrade-branch-forensics.md`.
- Worktree inventory: `/tmp/llamafile-worktree-inventory.md`.
- Fork state + sync: `/tmp/llamafile-fork-state.md`.
- llama.cpp Δ: `/tmp/llamaccp-changes-report.md`.
- ik_llama kernels: `/tmp/ikllama-kernels-report.md`.
- golem low-RAM: `/tmp/golem-lowram-report.md`.
- In-repo prior migration logs: `docs/upgrade/00{1..8}-*.md`, `docs/PROGRESS_LOG.md`, `llama.cpp.patches/UPGRADE_GUIDE.md`.
