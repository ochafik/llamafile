# llamafile: embeddable vector / semantic-search engine — evaluation (2026-06-29)

> The "real search" complement to the title-prefix Wikipedia lookup (doc 01) and the
> SQLite Wikidata entity store (doc 07). Goal: power **semantic search** over embedded
> Wikipedia article text and **fuzzy label→Qid** for Wikidata, inside the **single portable
> cosmocc/APE binary**.
> Method: web research (2025–2026, cited, accessed 2026-06-29) + hands-on inspection of the
> repo's vendored SQLite build and the user's prior-art pipeline at `~/github/ai/graphs/`.
> EVIDENCE is marked; the rest is INFERENCE. The make-or-break filter is **cosmocc-buildability**.

---

## 0. TL;DR (recommendation + biggest risk)

- **The missing piece is only the vector index.** llamafile already runs embedding models
  (`--embedding`, OpenAI-compatible `/v1/embeddings`), and the user's prior art already drives
  exactly this stack (`embed_parquet_llama_server.py`: nomic-embed-text-v1.5 GGUF via
  `llama-server --embeddings`). So the engine question = *"what ANN/index format embeds cleanly
  in the APE and holds the corpus?"* — not *"how do we embed?"*

- **Decisive axis = ANN-indexed vs brute-force (linear scan).** Brute-force is fine to
  ~10^5–10^6 vectors (sub-100 ms with binary quant), useless at full-Wikidata scale (8–113 M).
  The two SQLite vector extensions are **brute-force only**; real ANN means a header-only HNSW
  (usearch / hnswlib).

- **v1 PRIMARY (lowest cosmocc risk): `asg017/sqlite-vec`, compiled as a *static* extension
  into the SQLite that llamafile already vendors and builds under cosmocc** (`third_party/sqlite`,
  already `-DSQLITE_CORE -DSQLITE_ENABLE_FTS5`). Pure C, single-file amalgamation, MIT/Apache-2.0.
  Use **binary (bit) quantization + float rescore** → 24 MB index for 250 k Simple-wiki vecs.
  Brute-force, but sufficient to ~1–5 M vectors, and it **unifies exact-title + FTS5 + semantic
  in one `.db`** (the hybrid the user wants). This is the v1 because it is the *least new
  machinery* on top of what already compiles.

- **SCALE PATH (v2, when N ≳ 5–10 M — full English-Wikipedia chunks or full Wikidata labels):
  `unum-cloud/usearch` as a standalone, `mmap`-viewable HNSW `.usearch` index** bundled in the
  APE zip (or shipped external via a `--wikidata-vectors`/`--zim` mirror). Real ANN (sub-linear),
  Apache-2.0, header-only C++11 with a C API, `b1`/`i8` quantization, billion-scale. Build it
  offline; at query time `index.view()` mmaps it; ids → SQLite/ZIM for payload. **hnswlib is the
  fallback** if usearch's C++/intrinsics fight cosmocc.

- **Biggest risk:** for v1, that `sqlite-vec.c`'s SIMD paths or a stray C99/`SQLITE_CORE`
  registration detail need a small patch under cosmocc `-mgcc` (LOW — it's a single pure-C file,
  already built statically against `vendor/sqlite3.c` upstream). For the scale path, that
  **usearch's C++ (libcxx exceptions + SimSIMD intrinsics) needs taming under cosmocc** —
  mitigated by `USEARCH_USE_SIMSIMD=0` and the C wrapper; hnswlib is the lower-risk C++ fallback.
  The user already proved DuckDB-VSS is *not* the answer (experimental persistence + a logged
  bug). **The engine is solved; the work is the offline embed+quantize+pack pipeline (which the
  user has already largely built).**

---

## 1. The user already has the pipeline — reuse, don't reinvent (EVIDENCE)

Inspected `~/github/ai/graphs/` (Nov 2024 – Oct 2025). What they tried, what worked, what broke:

| Artifact | What it does | Verdict for llamafile |
|---|---|---|
| `embed_parquet_llama_server.py` (Oct 2025, 22 KB) | Embeds a parquet corpus by POSTing to **`llama-server --embeddings`** `/v1/embeddings` (nomic-embed-text-v1.5 Q8_0 GGUF), fanned out `-np 24` across several servers + caddy. | **This IS the llamafile `--embedding` integration.** The offline-embed step is already written and tuned. |
| `embed_wikidata.py` | Extracts `id + label\ndescription` from `latest-all.json.gz`, embeds, writes parquet. **~8 h full dump on M1 Ultra.** | The corpus-prep step. Note: cost is ~hours, single-pass → precompute offline, ship the index. |
| `build_annoy.py` | Spotify **annoy** `on_disk_build` (mmap), 768-dim, `angular`/cosine, `n_trees=100`. **8 M × 768 (40.2 GB parquet) → 40 min to index on M2 Max.** | Real, working ANN-on-disk at 8 M scale. annoy works but is in maintenance mode + lower recall than HNSW → prefer usearch/hnswlib. Confirms 8 M is their actual Wikidata-text count. |
| `create_hnswlib.sql`, `bug_vss.sql`, `search_ddb.py` | **DuckDB-VSS** HNSW (cosine), `SET hnsw_enable_experimental_persistence=true`; `bug_vss.sql` is a repro. | **DuckDB-VSS hit a wall** (persistence is experimental/fragile; user logged a bug). Dead end for an embedded single binary anyway (DuckDB = huge C++, not cosmocc-friendly). |
| `reduce_dimensionality.py` | LSI/topic reduction → 128 dims (gensim). | They already considered shrinking dims. Cleaner route = nomic-v1.5 **Matryoshka** truncation (768→512/256/128) + quantization. |
| `embed_duckdb.py`, `server/embed_duckdb_sqlite.py`, `milvus_bulk_import.py` | DuckDB embed bridge; a DuckDB→SQLite bridge; a Milvus bulk-import stub. | They've already migrated toward SQLite as the store and away from Milvus (server, heavy). Confirms SQLite-as-store direction (doc 07). |
| `test_oxidata/` | Oxigraph (Rust RDF/SPARQL) experiments. | Relevant to the SPARQL question (doc 07): Oxigraph = Rust → not cosmocc-embeddable; keep as the *external* SPARQL bridge, not in-APE. |

**Embedding model decided by prior art:** `nomic-embed-text-v1.5`, **768-dim**, Q8_0 GGUF.
EVIDENCE. (It supports Matryoshka truncation and task prefixes `search_query:` / `search_document:`
— use them; they materially change recall.) Use the same model in llamafile so the **offline
corpus embeddings and the in-process query embedding are identical**.

**Gotcha already hit (EVIDENCE, from the pm2 logs in the file):**
`GGML_ASSERT((causal_attn || n_ubatch >= n_tokens_all))` — the embedding path is non-causal, so
**`--ubatch-size` must be ≥ the longest input**, or inputs must be truncated. llamafile's embedding
mode must set `-ub`/`-b` high (they used `-b 8192 -ub 8192 -c 8192`) and/or truncate. Carry this
into the llamafile `--embedding` wiring.

---

## 2. The end-to-end stack (the thing to build)

```
OFFLINE (build host, once per corpus dump)
  ZIM / Wikidata dump
     │  extract {id, text}  (id = ZIM article / Qid)        ← embed_wikidata.py exists
     ▼
  llamafile --embedding  (nomic-embed-text-v1.5, search_document: prefix)
     │  768-d float32 per item                              ← embed_parquet_llama_server.py exists
     ▼
  QUANTIZE  (binary bit / int8 — Matryoshka-truncate first if wanted)
     │
     ▼
  BUILD INDEX
     ├─ v1:  sqlite-vec  → vec0 virtual table inside the SAME .db as FTS5 + entity store
     └─ v2:  usearch     → standalone  corpus.usearch  (mmap), ids parallel-array → SQLite/ZIM
     │
     ▼
  PACK: ship the .db / .usearch  external (--wikidata / --zim mirror, doc 01/07)
        or zipalign-append into the APE zip (like the model/ZIM) for small corpora

RUNTIME (the one APE binary)
  query string
     │ "search_query:" prefix
     ▼
  llamafile --embedding  → 768-d query vector  (already in-process)
     │
     ▼
  ANN/brute-force search over the bundled index → top-k ids
     │
     ▼
  fetch payload:  id → ZIM cluster (article text)  /  Qid → SQLite entity store (claims)
     │
     ▼
  (hybrid) fuse with exact-title + FTS5 BM25 results  → return to model as tool result
```

**Index-size budget (768-dim, nomic; EVIDENCE for raw math, INFERENCE for HNSW overhead):**

| Corpus | N vecs | f32 | int8 | **binary (1-bit)** | + HNSW graph (≈M=16) |
|---|---|---|---|---|---|
| Simple-wiki, 1 vec/article | 250 k | 768 MB | 192 MB | **24 MB** | +~40 MB |
| Simple-wiki, ~4 chunks/article | ~1 M | 3 GB | 768 MB | **96 MB** | +~150 MB |
| Wikidata (user's actual entity-texts) | 8 M | 24.6 GB | 6.1 GB | **768 MB** | +~1.5 GB |
| Full Wikidata labels | ~100–113 M | ~310 GB | ~77 GB | **~10 GB** | +~15–30 GB |

Takeaways: **binary quantization is mandatory** for anything Wikidata-scale (32× shrink; do a
float **rescore** of the top candidates to recover recall — sqlite-vec and usearch both support
this pattern). Simple-wiki **fits in the APE zip** (24–96 MB). Wikidata-scale should ship as an
**external file** (`--wikidata-vectors`, mirroring doc 07's `--wikidata`), not embedded.

---

## 3. Candidate matrix — ANN vs brute-force, and cosmocc verdict (the core table)

| Candidate | Index type | Algo | Practical max N (interactive) | Recall knobs | Lang / deps | License | **cosmocc verdict** |
|---|---|---|---|---|---|---|---|
| **sqlite-vec** (asg017) | **BRUTE-FORCE** | linear KNN over `vec0`; IVF/DiskANN exist but **experimental, off by default** | ~1–5 M (binary), ~few×10^5 (f32) | binary/int8 quant + float **rescore** | **pure C**, no deps, single-file amalgamation | **MIT / Apache-2.0** | **GREEN — best.** SQLite already vendored+built under cosmocc; add one C file + static `sqlite3_vec_init` |
| **sqlite-vector** (sqliteai) | **BRUTE-FORCE** | `vector_full_scan` (exact) / `vector_quantize_scan` (TurboQuant SIMD 2/3/4-bit). **No graph/tree index** | ~1–5 M (quantized) | TurboQuant 2/3/4-bit SIMD scan | **pure C** (99%), Makefile | **Elastic License 2.0** (OSS grant) | **YELLOW — builds, but license.** Pure C is fine; **Elastic-2.0 is not OSI-approved** → contaminates an Apache-2.0 binary's downstream commercial use |
| **usearch** (unum) | **ANN** | **HNSW**, mmap `view()` | **10^7–10^9** (sub-linear) | `ef`/`M`; `f16`/`i8`/`b1` quant | header-only **C++11** + C API; SimSIMD **optional** | **Apache-2.0** | **YELLOW→GREEN — the scale answer.** C++ under cosmocc + SimSIMD intrinsics = the risk; mitigate `USEARCH_USE_SIMSIMD=0`, use C wrapper |
| **hnswlib** (nmslib) | **ANN** | **HNSW** | 10^7–10^8 | `ef`/`M` | header-only **C++11**, no deps | **Apache-2.0** | **YELLOW — fallback ANN.** Simplest C++ HNSW; but **mmap is weak** (loads full index to RAM) → worse for big bundled indexes |
| annoy (spotify) | **ANN** | random-projection forests, **mmap `on_disk`** | 10^7–10^8 | `n_trees`, `search_k` | C++, mmap | Apache-2.0 | **YELLOW — works (user proved 8 M), but** maintenance-mode, lower recall than HNSW |
| sqlite-vss (asg017) | ANN | faiss IVF | — | — | **C++ + faiss** (BLAS/OpenMP) | MIT | **RED — deprecated** by its own author in favor of sqlite-vec; faiss won't cosmocc |
| faiss (Meta) | ANN | many | 10^8+ | many | **C++ + BLAS + OpenMP** | MIT | **RED — heavy deps**, not cosmocc-targetable |
| DuckDB-VSS | ANN | HNSW | 10^7 | `ef`/`M` | **DuckDB (huge C++)**; persistence **experimental** | MIT | **RED — user hit a bug** (`bug_vss.sql`), persistence flagged-experimental, DuckDB not cosmocc-embeddable |
| Milvus-lite | ANN | HNSW/IVF | 10^7 | many | Python/C++ **server** | Apache-2.0 | **RED — server/heavy**, not single-binary |
| LanceDB | ANN | IVF-PQ/HNSW | 10^8 | many | **Rust** | Apache-2.0 | **RED — Rust**, no cosmopolitan target |
| ScaNN (Google) | ANN | aniso PQ | 10^8 | many | **C++ + TF + Bazel** | Apache-2.0 | **RED — heavy build** |

> **Brute-force ⇒ small-corpus only.** The two SQLite extensions are linear-scan. They are great
> for Simple-wiki (250 k–1 M) and bounded Wikidata-label subsets, and *unbeatable on integration
> simplicity*, but they are **not** the answer for full English-Wikipedia chunks or 100 M Wikidata
> labels. For that you need real HNSW (usearch/hnswlib). This is the axis that splits v1 from v2.

---

## 4. The two SQLite extensions — verified, not marketing (EVIDENCE)

### 4a. `asg017/sqlite-vec` — brute-force, pure C, best license

- **EVIDENCE** ([github.com/asg017/sqlite-vec](https://github.com/asg017/sqlite-vec),
  [tracking issue #25](https://github.com/asg017/sqlite-vec/issues/25),
  [compiling docs](https://alexgarcia.xyz/sqlite-vec/compiling.html), accessed 2026-06-29):
  pure C, no deps, MIT/Apache-2.0; stores **float / int8 / binary** vectors in a `vec0` virtual
  table; **issue #25 states "sqlite-vec as of v0.1.0 will be brute-force search only, which slows
  down on large datasets."** ANN indexes (`rescore`, `ivf` (experimental, *not enabled*),
  `diskann`) are present in alpha but **not the stable default**. Latest ~v0.1.9 (Mar 2026),
  still pre-v1 ("expect breaking changes"). Sponsored by Mozilla Builders, Fly, Turso.
- **EVIDENCE — buildability**: ships a **single-file amalgamation** (`sqlite-vec.c` + `.h` in the
  Releases). Its own Makefile already compiles it **statically against `vendor/sqlite3.c` with
  `-DSQLITE_CORE -DSQLITE_VEC_STATIC`** — i.e. exactly the static-extension mode llamafile needs.
- **VERDICT: brute-force, GREEN buildability.** Query is **O(N)**; with binary vectors + rescore
  it's fast to ~1–5 M. The cleanest fit because llamafile **already vendors and builds SQLite
  under cosmocc** (`third_party/sqlite/sqlite3.c`, see §5).

### 4b. `sqliteai/sqlite-vector` — the user-flagged one: "no index, a bit sus" → CONFIRMED brute-force

- **EVIDENCE** ([github.com/sqliteai/sqlite-vector](https://github.com/sqliteai/sqlite-vector),
  [deepwiki](https://deepwiki.com/sqliteai/sqlite-vector),
  [LICENSE](https://github.com/sqliteai/sqlite-vector/blob/main/LICENSE.md), accessed 2026-06-29):
  pure C (99%), Makefile; markets "**Zero preindexing needed**", "no virtual tables — store
  vectors as BLOBs". It exposes `vector_full_scan` (exact) and `vector_quantize_scan` (approximate
  via **TurboQuant** 2/3/4-bit SIMD). **There is no HNSW/IVF/DiskANN graph or tree.** v1.0.0 (May
  2026), actively maintained (~217 commits, ~987★).
- **VERDICT on the user's suspicion: correct.** "No index" = **brute-force full scan, O(N)**.
  TurboQuant only lowers the *per-vector* cost (SIMD over compressed codes); it still **scans
  every row**. So same scale class as sqlite-vec (~1–5 M), *not* sub-linear. The marketing
  ("ultra-efficient", "semantic retrieval") describes a fast linear scan, not an ANN index.
- **License is the disqualifier vs sqlite-vec**: **Elastic License 2.0** (with a grant for use
  *inside* OSI-approved OSS). llamafile is Apache-2.0 (OSI-approved), so the grant *technically*
  lets llamafile bundle it — but Elastic-2.0 is **not OSI-approved**, forbids offering the software
  as a managed service / circumventing license keys, and would **muddy the licensing of the
  combined APE for llamafile's commercial downstream users**. sqlite-vec (MIT/Apache-2.0) is the
  same architecture (pure-C brute-force) with a clean license → **prefer sqlite-vec**.

---

## 5. Question 1 answered: SQLite-extension vs standalone-index under cosmocc

**EVIDENCE (repo inspection):** llamafile already vendors SQLite at
`third_party/sqlite/sqlite3.c` and builds it under cosmocc with (`third_party/sqlite/BUILD.mk`):

```
-mgcc -DSQLITE_CORE -DSQLITE_OS_UNIX
-DSQLITE_ENABLE_FTS5   -DSQLITE_ENABLE_RTREE   -DSQLITE_ENABLE_MATH_FUNCTIONS
-DSQLITE_ENABLE_DBSTAT_VTAB -DSQLITE_ENABLE_GEOPOLY ...
```

This changes the answer to the user's hunch. **Compiling a vector *extension* is NOT
meaningfully harder than mmap'ing a standalone index — *for sqlite-vec specifically* — because:**

1. SQLite is **already compiled under cosmocc** and works; **FTS5 is already on** (the hybrid
   keyword path is free).
2. `sqlite-vec` is **one pure-C amalgamation file**. With `-DSQLITE_CORE` you drop it into the
   build and call `sqlite3_vec_init` from a static auto-extension list (no `dlopen`, no loadable
   `.so` — which is the part that's actually hard/forbidden in a static APE). Its upstream Makefile
   already does this exact static link against `vendor/sqlite3.c`.
3. No second index format, no parallel id-mapping layer, no separate file lifecycle — vectors live
   **next to FTS5 and the Wikidata entity store in one `.db`**.

So **the lower-complexity v1 under cosmocc is the SQLite extension route (sqlite-vec)** — *as long
as you accept brute-force scale*. The standalone-index route (usearch/hnswlib) is **only worth its
extra cost when you need real ANN** (N ≳ 5–10 M). At that point the standalone index is *also* not
that hard — usearch is header-only with a C API and an mmap `view()` — but it adds: a C++ TU under
cosmocc, SimSIMD handling, and a separate id→payload mapping. Net:

- **Brute-force is enough (Simple-wiki, bounded Wikidata) → SQLite extension (sqlite-vec) wins on
  simplicity.**
- **Need ANN at scale (full Wikipedia chunks, full Wikidata) → standalone usearch HNSW; the SQLite
  extensions cannot get you there regardless of build effort.**

(RTREE being enabled is a red herring: it's a low-dim spatial R-tree, useless for 768-d embeddings.)

---

## 6. Question 3 answered: v1 recommendation + the crossover

**Progressive, two-tier — pick by the *first* corpus you ship:**

### v1 (ship first) — `sqlite-vec` static extension, binary-quantized, hybrid with FTS5
- **Why:** lowest cosmocc risk (pure C, SQLite already builds + FTS5 on), MIT/Apache-2.0, one
  `.db` for exact-title + FTS5 + semantic, reuses the user's nomic-v1.5 embed pipeline verbatim.
- **Index format:** `vec0` virtual table, **binary (bit) vectors + float rescore** (24 MB for
  250 k Simple-wiki → bundle in the APE zip; ship Wikidata-scale `.db` external via a
  `--wikidata`/`--zim`-style flag).
- **Covers:** Simple-wiki semantic search (the actual near-term ask), Wikidata fuzzy label→Qid for
  bounded label sets (or as a rescorer behind FTS5/trigram).
- **Quantize?** Yes — **binary + rescore** by default (32× smaller, brute-force binary KNN is
  extremely fast); keep int8 as the higher-recall option. Optionally Matryoshka-truncate nomic
  768→256 first.
- **Biggest build risk:** a SIMD/`SQLITE_CORE` nit in `sqlite-vec.c` under cosmocc `-mgcc` (LOW;
  scalar fallback exists; it already builds statically against the SQLite amalgamation upstream).

### v2 / scale path — `usearch` standalone mmap HNSW
- **Trigger:** N ≳ 5–10 M (full English-Wikipedia chunks, or full ~100 M Wikidata labels) where
  brute-force O(N) per query stops being interactive.
- **Index format:** offline-built `corpus.usearch`, **`mmap` via `index.view()`** (no full-RAM
  load), `b1`/`i8` quantized; a parallel id array (or SQLite rowid table) maps HNSW slot → ZIM
  article / Qid. Ship external (10 GB binary at 100 M — not zip-embeddable).
- **cosmocc plan:** compile the C wrapper (`c/usearch.h` + `lib.cpp`) as **one C++ TU with
  `USEARCH_USE_SIMSIMD=0`** (drops the intrinsics dependency; cosmocc's libcxx supplies the C++
  runtime). **Validate exceptions/libcxx link early** — that's the make-or-break test.
- **Fallback:** **hnswlib** (header-only, Apache-2.0, simpler C++) if usearch's C++ resists
  cosmocc; accept weaker mmap (loads index to RAM — fine for ≤ a few GB). annoy (`on_disk` mmap,
  user-proven at 8 M) is a third fallback but lower recall.

**One-line crossover:** *brute-force SQLite (sqlite-vec) up to a few million vectors; real HNSW
(usearch) beyond.* The make-or-break filter (cosmocc-buildability) favors sqlite-vec for v1; the
scale ambition (Wikidata 100 M) forces usearch for v2.

---

## 7. Question 4 answered: hybrid composition (exact + FTS + semantic)

Both tiers compose with the existing keyword paths. The model should get a **fused** result:

```
query
 ├─ exact:  title binary-search (ZIM, doc 01) / Qid+label exact (SQLite, doc 07)
 ├─ lexical: FTS5 BM25 over titles/labels/aliases  (FTS5 ALREADY compiled in — free)
 └─ semantic: ANN/brute-force over embeddings (this doc)
        │
        ▼  Reciprocal-Rank-Fusion (RRF) or weighted merge → dedup by id → top-k
```

- **v1 makes this trivial:** sqlite-vec's `vec0` lives in the **same SQLite db** as FTS5 and the
  Wikidata entity store → a single SQL query can `JOIN`/`UNION` lexical + vector candidates and
  RRF-rank them. No cross-store glue.
- **v2:** usearch returns ids; fetch FTS5/exact candidates from SQLite, fuse in C. A little more
  glue, but the payoff is sub-linear scale.
- **Recommended default:** exact-title → if thin, FTS5 BM25 + semantic, RRF-fused. Semantic
  recovers the cases title-prefix and BM25 miss (paraphrase, synonymy) — the actual reason to add
  vectors. For Wikidata fuzzy label→Qid, run FTS5/trigram first and use vectors as a **rescorer**
  (keeps N small enough that even brute-force is instant).

---

## 8. cosmocc-buildability verdict per candidate (summary)

| Candidate | Verdict | One-line reason |
|---|---|---|
| **sqlite-vec** | **GREEN** | pure C amalgamation; SQLite already vendored+built under cosmocc; static `SQLITE_CORE` register |
| **sqlite-vector** (sqliteai) | **YELLOW (license)** | pure C builds fine, but Elastic-2.0 (non-OSI) contaminates the Apache-2.0 binary |
| **usearch** | **YELLOW→GREEN** | header-only C++ + C API; real HNSW + mmap; risk = libcxx/SimSIMD under cosmocc → `USEARCH_USE_SIMSIMD=0` |
| **hnswlib** | **YELLOW** | header-only C++ HNSW, clean; weak mmap (loads to RAM); ANN fallback |
| annoy | **YELLOW** | C++ mmap `on_disk`, user-proven at 8 M; maintenance-mode, lower recall |
| sqlite-vss | **RED** | deprecated; faiss/C++ won't cosmocc |
| faiss | **RED** | BLAS + OpenMP heavy C++ |
| DuckDB-VSS | **RED** | huge C++; persistence experimental; user hit a bug |
| Milvus-lite | **RED** | server/heavy |
| LanceDB | **RED** | Rust — no cosmopolitan target |
| ScaNN | **RED** | TF + Bazel heavy |

---

## 9. Open items / next steps

1. **Spike the make-or-break test:** compile `sqlite-vec.c` (amalgamation) into the vendored
   SQLite under cosmocc `-mgcc -DSQLITE_CORE`, register via auto-extension, run a `vec0` binary
   KNN. If green, v1 is unblocked.
2. **Separately** spike usearch's C wrapper under cosmocc with `USEARCH_USE_SIMSIMD=0` to de-risk
   v2 early (exceptions/libcxx link is the gate). Fall back to hnswlib if it fights.
3. Decide v1 corpus: Simple-wiki (250 k, zip-embeddable) is the obvious first target → semantic
   search over article text, the "real search" answer doc 01 lacks.
4. Reuse `embed_parquet_llama_server.py` + `embed_wikidata.py` for offline embed; add a quantize +
   pack step (binary + rescore). Use nomic-v1.5 with `search_document:`/`search_query:` prefixes
   and set `-ub` ≥ longest input (the logged non-causal assert).
5. Delivery mirrors doc 01/07: bundle small indexes in the APE zip (zipalign-append, like the
   model/ZIM); ship Wikidata-scale indexes external via a `--wikidata-vectors` flag.

---

### Sources (accessed 2026-06-29)
- sqlite-vec: <https://github.com/asg017/sqlite-vec> · ANN issue <https://github.com/asg017/sqlite-vec/issues/25> · compiling <https://alexgarcia.xyz/sqlite-vec/compiling.html> · v0.1.0 post <https://alexgarcia.xyz/blog/2024/sqlite-vec-stable-release/index.html>
- sqlite-vector (sqliteai): <https://github.com/sqliteai/sqlite-vector> · <https://deepwiki.com/sqliteai/sqlite-vector> · license <https://github.com/sqliteai/sqlite-vector/blob/main/LICENSE.md> · <https://www.sqlite.ai/sqlite-vector>
- sqlite-vss (deprecated): <https://github.com/asg017/sqlite-vss>
- usearch: <https://github.com/unum-cloud/usearch> · docs <https://unum-cloud.github.io/USearch/> · build/SimSIMD <https://github.com/unum-cloud/USearch/blob/main/CONTRIBUTING.md>
- hnswlib: <https://github.com/nmslib/hnswlib>
- annoy: <https://github.com/spotify/annoy>
- faiss: <https://github.com/facebookresearch/faiss>
- DuckDB-VSS: <https://duckdb.org/docs/stable/core_extensions/vss> · <https://github.com/duckdb/duckdb-vss>
- cosmocc / C++ support: <https://github.com/jart/cosmopolitan/blob/master/tool/cosmocc/README.md> · STL issue <https://github.com/jart/cosmopolitan/issues/27>
- Repo inspection: `third_party/sqlite/BUILD.mk` (SQLITE_CORE + FTS5 + RTREE), `llamafile/zim/`
- User prior art: `~/github/ai/graphs/` (`embed_parquet_llama_server.py`, `embed_wikidata.py`, `build_annoy.py`, `create_hnswlib.sql`, `bug_vss.sql`, `search_ddb.py`, `reduce_dimensionality.py`)
</content>
</invoke>
