#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘

PKGS += TESTS

include tests/sgemm/BUILD.mk
include tests/strsm/BUILD.mk

# ==============================================================================
# Include paths (reuse llamafile includes)
# ==============================================================================

TESTS_CPPFLAGS := $(LLAMAFILE_INCLUDES)

# ==============================================================================
# Test: extract_data_uris_test
# ==============================================================================

# Dependencies for extract_data_uris test:
#   - extract_data_uris.o: contains extract_data_uris function (isolated)
#   - datauri.o: DataUri class for parsing data URIs
#   - image.o: is_image function for validating images
#   - string.o: lf::startscasewith helper
#   - xterm.o: terminal utilities (required by image.o)
#   - stb.a: stb_image for image validation

EXTRACT_DATA_URIS_TEST_DEPS := \
	o/$(MODE)/llamafile/extract_data_uris.o \
	o/$(MODE)/llamafile/datauri.o \
	o/$(MODE)/llamafile/image.o \
	o/$(MODE)/llamafile/string.o \
	o/$(MODE)/llamafile/xterm.o \
	o/$(MODE)/third_party/stb/stb.a \
	o/$(MODE)/llama.cpp/common/build-info.cpp.o \
	o/$(MODE)/llama.cpp/common/jinja/caps.cpp.o \
	o/$(MODE)/llama.cpp/common/jinja/lexer.cpp.o \
	o/$(MODE)/llama.cpp/common/jinja/parser.cpp.o \
	o/$(MODE)/llama.cpp/common/jinja/runtime.cpp.o \
	o/$(MODE)/llama.cpp/common/jinja/string.cpp.o \
	o/$(MODE)/llama.cpp/common/jinja/value.cpp.o

o/$(MODE)/tests/extract_data_uris_test.o: tests/extract_data_uris_test.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TESTS_CPPFLAGS) -c -o $@ $<

o/$(MODE)/tests/extract_data_uris_test: \
		o/$(MODE)/tests/extract_data_uris_test.o \
		$(EXTRACT_DATA_URIS_TEST_DEPS)
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ==============================================================================
# Test: fa_helpers_test (issue #975 numerical equivalence)
# ==============================================================================
#
# Compares llamafile_fa_vec_dot_f16 / llamafile_fa_fp16_to_fp32_row
# against upstream ggml_vec_dot_f16 / ggml_fp16_to_fp32_row on the
# same random + edge-case inputs. Catches numerical regressions in
# the alternative AVX-512F implementations we ship via sgemm.cpp's
# dispatch. On CPUs without AVX-512F the helpers report unsupported
# and the corresponding assertions are skipped.

FA_HELPERS_TEST_DEPS := \
	o/$(MODE)/llamafile/sgemm.o \
	o/$(MODE)/llamafile/llamafile.o \
	o/$(MODE)/llamafile/fa_helpers_amd_avx512f.o \
	o/$(MODE)/llamafile/fa_helpers_unsupported.o \
	o/$(MODE)/llamafile/fa_simd_gemm_amd_avx512f.o \
	o/$(MODE)/llamafile/tinyblas_cpu_sgemm_amd_avx.o \
	o/$(MODE)/llamafile/tinyblas_cpu_sgemm_amd_fma.o \
	o/$(MODE)/llamafile/tinyblas_cpu_sgemm_amd_avx2.o \
	o/$(MODE)/llamafile/tinyblas_cpu_sgemm_amd_avxvnni.o \
	o/$(MODE)/llamafile/tinyblas_cpu_sgemm_amd_avx512f.o \
	o/$(MODE)/llamafile/tinyblas_cpu_sgemm_amd_zen4.o \
	o/$(MODE)/llamafile/tinyblas_cpu_sgemm_arm80.o \
	o/$(MODE)/llamafile/tinyblas_cpu_sgemm_arm82.o \
	o/$(MODE)/llamafile/tinyblas_cpu_unsupported.o \
	o/$(MODE)/llamafile/tinyblas_cpu_mixmul_amd_avx.o \
	o/$(MODE)/llamafile/tinyblas_cpu_mixmul_amd_fma.o \
	o/$(MODE)/llamafile/tinyblas_cpu_mixmul_amd_avx2.o \
	o/$(MODE)/llamafile/tinyblas_cpu_mixmul_amd_avxvnni.o \
	o/$(MODE)/llamafile/tinyblas_cpu_mixmul_amd_avx512f.o \
	o/$(MODE)/llamafile/tinyblas_cpu_mixmul_amd_zen4.o \
	o/$(MODE)/llamafile/tinyblas_cpu_mixmul_arm80.o \
	o/$(MODE)/llamafile/tinyblas_cpu_mixmul_arm82.o \
	o/$(MODE)/llamafile/iqk_mul_mat_amd_avx2.o \
	o/$(MODE)/llamafile/iqk_mul_mat_amd_zen4.o \
	o/$(MODE)/llamafile/iqk_mul_mat_arm82.o \
	o/$(MODE)/llamafile/iqk_quantize_k.o \
	o/$(MODE)/llama.cpp/llama.cpp.a

o/$(MODE)/tests/fa_helpers_test.o: tests/fa_helpers_test.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TESTS_CPPFLAGS) -fopenmp -c -o $@ $<

o/$(MODE)/tests/fa_helpers_test: \
		o/$(MODE)/tests/fa_helpers_test.o \
		$(FA_HELPERS_TEST_DEPS)
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -fopenmp -o $@ $^ $(LDLIBS)

# ==============================================================================
# Test: gpu_backend_test (issue #988 device-count gate / fallback)
# ==============================================================================
#
# Exercises the shared GPU backend probe core. The test injects stub entry
# points into a GpuBackend and provides its own doubles for gpu_backend.c's
# externs, so it links against gpu_backend.o alone — no DSO, no GPU, no
# llamafile.o/llama.cpp.a.

GPU_BACKEND_TEST_DEPS := \
	o/$(MODE)/llamafile/gpu_backend.o

o/$(MODE)/tests/gpu_backend_test.o: tests/gpu_backend_test.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TESTS_CPPFLAGS) -c -o $@ $<

o/$(MODE)/tests/gpu_backend_test: \
		o/$(MODE)/tests/gpu_backend_test.o \
		$(GPU_BACKEND_TEST_DEPS)
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ==============================================================================
# Test: zim_reader_test (llamafile/zim ZIM reader, against tiny committed ZIMs)
# ==============================================================================
#
# Opens the two committed fixture archives (tests/fixtures/small_nons.zim v6 and
# small_withns.zim v5) and exercises open/metadata/get-by-path/title-search/
# redirect-resolution and the streaming-zstd cluster path. The fixture path
# defaults to "tests/fixtures" (correct when run from the repo root by make).

ZIM_READER_TEST_DEPS := \
	o/$(MODE)/llamafile/zim/zim.a

o/$(MODE)/tests/zim_reader_test.o: tests/zim_reader_test.c
	@mkdir -p $(@D)
	$(CC) $(CCFLAGS) $(CPPFLAGS) $(TESTS_CPPFLAGS) -iquote llamafile/zim -c -o $@ $<

o/$(MODE)/tests/zim_reader_test: \
		o/$(MODE)/tests/zim_reader_test.o \
		$(ZIM_READER_TEST_DEPS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ==============================================================================
# Test: zim_xapian_test (Xapian Glass BM25 reader, against the real ZIM)
# ==============================================================================
#
# Drives the llamafile/zim/zim_xapian.c full-text reader over the real
# Simple-English Wikipedia ZIM (X/fulltext/xapian Glass index): BM25 ranking of
# tokyo/japan/einstein/"TNT inventor", doclen normalization, and the title
# index. The ~1 GB ZIM is not committed, so the test SKIPS (exit 0) when absent.

ZIM_XAPIAN_TEST_DEPS := \
	o/$(MODE)/llamafile/zim/zim.a

o/$(MODE)/tests/zim_xapian_test.o: tests/zim_xapian_test.c
	@mkdir -p $(@D)
	$(CC) $(CCFLAGS) $(CPPFLAGS) $(TESTS_CPPFLAGS) -iquote llamafile/zim -c -o $@ $<

o/$(MODE)/tests/zim_xapian_test: \
		o/$(MODE)/tests/zim_xapian_test.o \
		$(ZIM_XAPIAN_TEST_DEPS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ==============================================================================
# Test: wikidata_test (llamafile/wikidata reader; self-built SQLite+FTS5 fixture)
# ==============================================================================
#
# Builds a tiny in-process SQLite+FTS5 store with the vendored sqlite, then
# drives the wikidata reader over it (FTS5 search incl. OR'd boolean query,
# entity fetch, claim decode, and unit/entity P->Q resolution). No fixture is
# committed — it is created in a temp file and unlinked. Links the production
# wikidata.o plus the vendored sqlite3.o.

WIKIDATA_TEST_DEPS := \
	o/$(MODE)/llamafile/wikidata.o \
	o/$(MODE)/third_party/sqlite/sqlite3.o

o/$(MODE)/tests/wikidata_test.o: tests/wikidata_test.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TESTS_CPPFLAGS) -c -o $@ $<

o/$(MODE)/tests/wikidata_test: \
		o/$(MODE)/tests/wikidata_test.o \
		$(WIKIDATA_TEST_DEPS)
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ==============================================================================
# Test: path_jail_test (llamafile/path_jail.h — server-tools jail canonicalizer)
# ==============================================================================
#
# Header-only: exercises lf::jail_resolve (the llamafile-owned extraction of the
# server-tools `jail_resolve` security check) over a temp tree with an escaping
# symlink. No external deps.

o/$(MODE)/tests/path_jail_test.o: tests/path_jail_test.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TESTS_CPPFLAGS) -c -o $@ $<

o/$(MODE)/tests/path_jail_test: \
		o/$(MODE)/tests/path_jail_test.o
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ==============================================================================
# Test: agent_runtime_test (interactive multi-agent runtime core)
# ==============================================================================
#
# Exercises the Router/mailbox/Scheduler/guard machinery in agent_runtime.cpp
# with a fake (no-model) TurnFn: delivery by id+name, mailbox FIFO + thread
# safety, runs-on-input / park-on-await / resume-on-message, and the spawn-depth
# / live-agent / message-rate runaway guards. Links the core object only.

o/$(MODE)/tests/agent_runtime_test.o: tests/agent_runtime_test.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TESTS_CPPFLAGS) -c -o $@ $<

o/$(MODE)/tests/agent_runtime_test: \
		o/$(MODE)/tests/agent_runtime_test.o \
		o/$(MODE)/llamafile/agent_runtime.o
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ==============================================================================
# Test: agent_runtime_sched_test (Phase 2 scheduling tools: wait/poll_until/
# schedule + the poll_until predicate language)
# ==============================================================================
#
# Exercises the timer-park machinery the scheduling tools ride on: wait() parks
# without holding a worker and resumes after N; poll_until() re-checks on a timer
# until a stub condition flips (and times out when it never does); schedule()
# delivers a delayed message that wakes a parked target; the max-jobs cap trips.
# Plus pure unit tests of agent_predicate.h. Links the core object only.

o/$(MODE)/tests/agent_runtime_sched_test.o: tests/agent_runtime_sched_test.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TESTS_CPPFLAGS) -c -o $@ $<

o/$(MODE)/tests/agent_runtime_sched_test: \
		o/$(MODE)/tests/agent_runtime_sched_test.o \
		o/$(MODE)/llamafile/agent_runtime.o
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Scripted mesh run (ddoc-09 9.1 integration gate, no model): drives the real
# Runtime end-to-end and writes/prints a real trace.jsonl (tree + concurrency +
# token totals). Links the core object only.
o/$(MODE)/tests/agent_runtime_mesh_demo.o: tests/agent_runtime_mesh_demo.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TESTS_CPPFLAGS) -c -o $@ $<

o/$(MODE)/tests/agent_runtime_mesh_demo: \
		o/$(MODE)/tests/agent_runtime_mesh_demo.o \
		o/$(MODE)/llamafile/agent_runtime.o
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ==============================================================================
# Test: agent_session_test (Phase 4 persisted/pausable/resumable sessions)
# ==============================================================================
#
# Exercises Runtime export/import roundtrip, the SessionManager lifecycle
# (create/list/pause/resume/stop) incl. a simulated full process restart from
# disk, the KV-fingerprint guard (match restores / tamper falls back), and the
# bounded-sessions eviction. Model-free: fake TurnFn + injected KV hooks. Links
# the runtime core + the session manager objects.
o/$(MODE)/tests/agent_session_test.o: tests/agent_session_test.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TESTS_CPPFLAGS) -c -o $@ $<

o/$(MODE)/tests/agent_session_test: \
		o/$(MODE)/tests/agent_session_test.o \
		o/$(MODE)/llamafile/agent_runtime.o \
		o/$(MODE)/llamafile/agent_session.o
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ==============================================================================
# Test: code_run_wrapper_test (Phase 3 headless-CDP code interpreter, ddoc 09 §4)
# ==============================================================================
#
# Pure unit test of the code_run_js wrapping helpers (build_code_eval_wrapper /
# format_code_result in browser_tool.h): JSON-literal embedding (injection
# safety), timeout inlining, console hooks, and MCP result shaping/truncation.
# Header-only — no httplib/CDP/model dependency. The live CDP eval path is
# browser-gated and verified separately against Chrome.
o/$(MODE)/tests/code_run_wrapper_test.o: tests/code_run_wrapper_test.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TESTS_CPPFLAGS) -c -o $@ $<

o/$(MODE)/tests/code_run_wrapper_test: \
		o/$(MODE)/tests/code_run_wrapper_test.o
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ==============================================================================
# Phony targets
# ==============================================================================

.PHONY: o/$(MODE)/tests
o/$(MODE)/tests: \
	o/$(MODE)/tests/extract_data_uris_test.runs \
	o/$(MODE)/tests/fa_helpers_test.runs \
	o/$(MODE)/tests/gpu_backend_test.runs \
	o/$(MODE)/tests/zim_reader_test.runs \
	o/$(MODE)/tests/zim_xapian_test.runs \
	o/$(MODE)/tests/wikidata_test.runs \
	o/$(MODE)/tests/path_jail_test.runs \
	o/$(MODE)/tests/agent_runtime_test.runs \
	o/$(MODE)/tests/agent_runtime_sched_test.runs \
	o/$(MODE)/tests/agent_runtime_mesh_demo.runs \
	o/$(MODE)/tests/agent_session_test.runs \
	o/$(MODE)/tests/code_run_wrapper_test.runs
