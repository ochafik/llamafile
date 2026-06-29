# llamafile: Wikidata + knowledge-graph/agent-memory engines — design (2026-06-29)

> The structured-knowledge-graph complement to the embedded Wikipedia ZIM (doc 01).
> Wikipedia ZIM = prose/context; Wikidata = exact facts/relations (e.g. *height of the
> Eiffel Tower* → property **P2048 = 330 m**, structured, machine-checkable).
> Status: **design locked toward a lookup-first v1; SPARQL/graph-query deferred to an external bridge.**
> Method: web research (2025–2026, cited) + direct inspection of the user's existing
> `/Volumes/AI Models at Home/wikidata` corpus. EVIDENCE is marked; the rest is INFERENCE.

## 0a. Two distinct use cases — keep them separate

This doc answers **two** questions that share "graph" vocabulary but have opposite shapes:

- **UC1 — query static Wikidata offline (the original ask).** A huge, read-only, pre-built
  fact graph. Want: resolve entity, fetch claims, maybe triple-pattern/SPARQL. Write-once,
  read-many, ~100 M entities. **Best fit: a compact read-only store (SQLite entity store v1;
  HDT/QLever for RDF/SPARQL).** Engines optimized for *ingest/mutation* (Kùzu, Graphiti,
  Memgraph) are the *wrong tool* here — you don't need a transactional writer to read a frozen
  dump.
- **UC2 — the agent's own dynamic knowledge graph / long-term memory.** Small, mutable,
  grows across sessions; facts the agents *accumulate* (ties into the multiagent platform doc
  02 + compaction/memory doc 05). Want: incremental writes, temporal validity, hybrid
  retrieval. **Best fit: an embedded property-graph DB (Kùzu-class) and/or the Graphiti
  *pattern*** (LLM-extracted temporal triples) — NOT a Wikidata dump engine.

The engine survey below tags every candidate UC1 / UC2 / both. **Recommendation per use case
is in §7.** The one-line answer: **UC1 → SQLite entity store (HDT/QLever-bridge for SPARQL);
UC2 → a small embedded property-graph store, with the Graphiti temporal-triple pattern
re-implemented in-process if/when agent memory is built.**

---

## 0. TL;DR (recommendation + biggest risk)

- **Two use cases, two answers (see §0a):** **UC1 (query static Wikidata) → SQLite entity
  store in-binary** + **external QLever/Oxigraph over MCP** for SPARQL. **UC2 (the agent's own
  dynamic KG / long-term memory) → the Graphiti *pattern* (LLM-extracted bitemporal triples)
  over an embeddable store** — *not* a Wikidata engine, and a separate, later workstream.
- **v1 = external `--wikidata <path>` (mirror `--zim`)** pointing at a **compact SQLite
  entity store** built from the data the user *already has*: `Qid → {label, description,
  aliases, packed best-rank claims}`, plus an FTS5 label/alias index. Pure C, SQLite is
  **already in cosmopolitan** → trivially embeddable in the APE, mmap/`pread`-friendly,
  no server, no JVM, no Rust. **Lookup + triple-pattern only, not SPARQL.**
- **Tools:** `wikidata_search(label)→Qids`, `wikidata_entity(Qid)→labels+resolved claims`,
  `wikidata_property(Qid,Pid)→values`. Mirror as a CLI subcommand (`llamafile wikidata
  get Q243 / search "Eiffel Tower"`) that short-circuits before model init, exactly like
  the planned `wikipedia` CLI.
- **SPARQL / arbitrary graph queries: DEFER.** No production SPARQL engine compiles cleanly
  under cosmocc (QLever = heavy C++ + 149 GB index + 2 TB build; Oxigraph = Rust, won't
  target cosmopolitan). If a user needs SPARQL, bridge to an **external QLever or Oxigraph
  over the MCP host** that doc 01 already builds. Optionally ship **HDT (hdt-cpp)** as an
  in-APE *triple-pattern* engine later — but it adds a C++ dep for capability the SQLite
  store already covers for the LLM-fact use case.
- **Biggest risk: rank/units/value-typing fidelity in the packed claims, NOT the engine.**
  The engine question is *solved* (SQLite). The hard part is producing a *correct, minimal*
  claims encoding — keeping **best-rank ("truthy")** statements (the user's current parse
  drops rank, so it stores 300 **and** 324 **and** 330 m for the Eiffel Tower with no way to
  pick "the" value), resolving units (stored as raw entity URIs), and typing values
  (entity vs quantity vs time vs string) so the tool returns *"330 metre"* not
  *"+330 x http://www.wikidata.org/entity/Q11573"*. This is a data-pipeline problem, fully
  in our control, and is the v1 work item.

---

## 1. What the user already has (EVIDENCE — inspected 2026-06-29)

`/Volumes/AI Models at Home/wikidata` (≈1.0 TB total). This is **not a blank slate** — it is
a half-built Wikidata pipeline with **both** halves we'd want (structured claims + semantic
search), just not in an APE-embeddable engine. Contents:

| File / dir | Size | What it is |
|---|---|---|
| `latest-all.json.gz` | **139.7 GB** | Full Wikidata JSON dump (gzip), dated Nov 2024 — the raw source of everything below. |
| `wikidata.ddb` | **30.6 GB** | **DuckDB**, one table `wikidata(type, id, label, description, aliases VARCHAR[], claims VARCHAR[][][])`, **112,983,982 rows** (112,971,736 items + 12,246 properties). label/description are **English**. **This is the structured entity store, already parsed.** |
| `wikidata.entity_texts.parquet` | 4.3 GB | `id, text` where text = `"label\ndescription"`, **82.2 M rows** — the corpus that was embedded. |
| `wikidata.embeddings-bge-micro.parquet` | 132 GB | per-entity bge-micro vectors. |
| `wikidata.embeddings-nomic-text-v1.5*.parquet` | 29 GB ×2 | per-entity nomic-embed-text-v1.5 vectors. |
| `wikidata.embeddings.parquet` / `.ddb` | 40 GB / 68 GB | embeddings in parquet / DuckDB. |
| `wikidata.embeddings.ann` | 38 GB | ANN index (Annoy/HNSW-style) for vector search. |
| `embeddings_reduction.pkl` + `*.embeddings_reduced.parquet` | 0.8 MB / 74 MB | a dimensionality reducer (PCA/UMAP) + reduced vectors. |
| `milvus/` | small (volume) | a **Milvus** vector-DB volume (etcd + rocksdb meta + mmap chunk mgr). |
| `sample100.json.gz` | 2.6 MB | 100-entity slice of the JSON dump (handy fixture). |

**Two key takeaways from inspection:**

1. **The structured store we want is essentially already built** — `wikidata.ddb` is exactly
   the "compact entity store (Qid → packed claims) for pure lookup without SPARQL" that this
   doc concludes is the right v1 shape. We do **not** need to re-parse 140 GB of JSON; we
   re-export from the DuckDB (or refresh from a newer JSON dump). EVIDENCE — the schema and a
   real row:
   - `Q243 → "Eiffel Tower" / "tower located on the Champ de Mars in Paris, France"`,
     aliases `["Tour Eiffel","tour Eiffel"]`.
   - `claims` is a **3-level array**: `[ statement ][ pair ][ {key,value} ]`. The **first
     pair** of each statement is the main `[property, value]`; subsequent pairs are
     **qualifiers**. Example, the height statements of Q243:
     ```
     [[P2048, +330 x http://www.wikidata.org/entity/Q11573], [P518, Q26970842], [P580, +2022-03-15...], ...]
     [[P2048, +324 x http://www.wikidata.org/entity/Q11573], [P518, Q26970842], [P580, +1950-...], [P582, ...]]
     [[P2048, +300 x http://www.wikidata.org/entity/Q11573], [P518, Q24192182]]
     ```
     So: **the 330 m fact is present and exact** (P2048, unit Q11573 = metre), with qualifiers
     (P518 "applies to part", P580/P582 start/end time) preserved — but **references and rank
     are dropped**, and the **unit is a raw entity URI** and the **value type is implicit**
     (parse the `+NNN x <uri>` to know it's a quantity). This is the encoding the v1 pipeline
     must clean up (see §6 risk).

2. **The user also already built semantic entity search** (entity_texts → bge/nomic
   embeddings → ANN/Milvus). That answers fuzzy *label→Qid* far better than string match —
   but it needs an **embedding model + vector index at query time**, which is too heavy for an
   APE v1 (see §4 search tiers). Treat it as an **optional external** retriever, not the v1
   in-binary path.

**Performance probe (EVIDENCE, on the external SSD via DuckDB):** point lookup by `id`
≈ 0.03 s; full-scan label match (`WHERE label='Eiffel Tower'`, no index) ≈ 0.96 s returning
many homonyms. → a search tool **needs a label/alias index** (DuckDB has none here; the v1
SQLite store adds FTS5).

> Note: `Q42` returned an **empty** label in this dump — the English label column is not
> populated for every row. The re-export must fall back (en → mul → any) per Wikidata's
> label-language fallback, or some entities will be nameless.

---

## 2. Wikidata data model + dump formats + sizes (EVIDENCE, 2025–2026)

**Data model.** Entities are **items (Q…)** and **properties (P…)** (plus **lexemes L…**,
out of scope). Each entity has **labels / descriptions / aliases** in ~hundreds of languages,
and **statements**: `property → value` with optional **qualifiers** (e.g. point-in-time,
applies-to-part), **references** (provenance), and a **rank** (preferred / normal /
deprecated). Values are typed: another entity (`Q…`), quantity (+amount + unit-entity +
bounds), time (precision-tagged), string, monolingual text, coordinate, URL, etc. "Truthy"
= the best-rank statement of each (subject, property) flattened to a single direct triple,
**dropping qualifiers, references, and rank**.

**Dump options + current sizes** (dumps.wikimedia.org/wikidatawiki/entities/, and Meta
"Data dumps/Dumps sizes and growth"):

| Dump | Format | Compressed | Uncompressed | Has qualifiers/refs? |
|---|---|---|---|---|
| `latest-all.json.bz2` | full JSON | **≈101 GB** | ≈2 TB (grows; "500 GB" figure is dated) | yes (everything) |
| `latest-all.nt.bz2` | full RDF N-Triples | **≈192 GB** | very large | yes (reified) |
| `latest-truthy.nt.bz2` | truthy RDF N-Triples | **≈40 GB**, **≈8 B triples** | ≈100 GB+ | **no** (direct best-rank only) |
| `latest-all.ttl.bz2` | full RDF Turtle | similar to .nt | — | yes |

(Sources: [Wikidata:Database download](https://www.wikidata.org/wiki/Wikidata:Database_download);
[dumps.wikimedia.org](https://dumps.wikimedia.org/wikidatawiki/entities/);
[Data dumps/sizes & growth](https://meta.wikimedia.org/wiki/Data_dumps/Dumps_sizes_and_growth);
truthy ≈8 B triples / ≈40 GB per [HF CleverThis/wikidata-truthy](https://huggingface.co/datasets/CleverThis/wikidata-truthy).)

⚠️ **Freshness risk (EVIDENCE):** the N-Triples RDF dumps have been **broken / empty since
~25 Jul 2025** ([phabricator T403882](https://phabricator.wikimedia.org/T403882)). A
truthy-RDF-first plan inherits that instability. The **JSON dump is the reliable source** —
and conveniently the user's pipeline (and `wikidata.ddb`) is already JSON-derived.

**What subset is actually useful for an LLM fact tool?** Drop ruthlessly:
- **Languages → English (+`mul`/fallback) only** for labels/descriptions/aliases. Removes the
  bulk: the user's *all-language* `wikidata.ddb` is 30.6 GB; the *English-text* projection
  (`entity_texts.parquet`) is **4.3 GB** for the label/desc text of 82 M entities — a ~7×
  reduction from just dropping non-English text.
- **References → drop entirely** (provenance is noise for a fact tool).
- **Rank → keep best-rank ("truthy") only**, discard deprecated/non-best (fixes the
  multi-value height ambiguity in §1).
- **Qualifiers → keep** (cheap, and time/unit qualifiers are exactly what makes a fact
  precise) — or drop in a "lite" tier.
- **Lexemes → drop.**

INFERENCE on size: an English-labels + best-rank-claims + qualifiers SQLite (re-exported from
`wikidata.ddb`) is plausibly **~8–15 GB**; an English-labels + truthy-only (no qualifiers,
entity/quantity values) tier is plausibly **~4–8 GB**. Both are external-path artifacts. A
**curated top-N bundle** (e.g. the ~1–5 M entities with Wikipedia sitelinks / highest
pageviews + their labels + truthy claims) is plausibly **~0.5–2 GB** → small enough to embed
in the APE via zipalign, like a small ZIM.

---

## 3. Engine survey — cosmocc/APE fit (EVIDENCE + verdicts)

Evaluation axes: pure C/C++ (no JVM / no Rust-FFI), mmap-friendly, single-binary/no-server,
permissive license, builds under cosmocc; query language; Wikidata-load feasibility + index
size. **UC** column = use case the engine actually serves (UC1 static-Wikidata / UC2
agent-memory). The cosmocc lens is central: the APE links **C/C++ statically**; **Rust** does
not target cosmopolitan, and **JVM** is a non-starter.

### 3a. RDF / SPARQL engines (UC1)

| Engine | Lang / deps | Query | Wikidata index size | UC | APE/cosmocc verdict |
|---|---|---|---|---|---|
| **SQLite (+FTS5)** | **C, in cosmopolitan already** | key lookup + joins + FTS; **no SPARQL** | re-export ~4–15 GB | UC1 | ✅ **BEST UC1 FIT.** Zero new deps, mmap, single file, public-domain, proven in APE. Covers resolve/claims/property/label-search. |
| **HDT / hdt-cpp** | C++ (serd) | **triple-pattern** only | ~100 GB RDF → **~10 GB**; truthy ≈ **15–25 GB** | UC1 | ⚠️ Later. mmap, compact, read-only. Adds C++/RDF-parser dep + **LGPL** (verify) for capability SQLite already gives the LLM. SPARQL needs an extra layer. |
| **QLever** | C++ (heavy: ICU, abseil) | **full SPARQL 1.1** | truthy ~8 B → **149 GB**, **3.1 h load**, 32–64 GB RAM / 2 TB to build; ~2.3 s query | UC1 | ❌ embed (deps/index/build). ✅ **external SPARQL server bridged via MCP** — the recommended SPARQL path. |
| **Oxigraph** | **Rust**, RocksDB | **full SPARQL 1.1** | RocksDB tens–>100 GB | UC1 | ❌ in-APE (Rust + RocksDB). ✅ clean **external bridge** (pyoxigraph / HTTP). |
| **RDFox** | C++, **commercial/proprietary** | SPARQL + Datalog (in-memory) | RAM-resident | UC1 | ❌ license + RAM-resident. External only. |
| **Blazegraph / Jena / GraphDB / Virtuoso** | **JVM** / commercial | full SPARQL | large | UC1 | ❌ APE (JVM). External only. |

### 3b. Property-graph / embedded-graph engines (UC1 read + UC2 write)

| Engine | Lang / deps | Query | Wikidata-load | UC | APE/cosmocc verdict |
|---|---|---|---|---|---|
| **Kùzu (KuzuDB)** | **C++**, **single-file** since 0.11, columnar/vectorized, mmap; vector + FTS built-in | **Cypher** (GQL-ish) | scales to 100s M nodes / B edges on one node; bulk `COPY FROM` parquet/csv | **both** | ⚠️ **The strongest *technical* APE fit — and the biggest catch (see 3c).** Embedded in-process, MIT, single file, C++ → *could* link into the APE. **BUT upstream ABANDONED Oct 2025.** Best agent-memory engine (UC2) by design; usable for UC1 if loaded from a dump. |
| **bighorn** (Kineviz fork of Kùzu) | C++ | Cypher | as Kùzu | both | ⚠️ Live fork of the abandoned Kùzu; inherits the embeddability but a young/uncertain maintenance base. |
| **Vela fork of Kùzu** | C++, MIT | Cypher + concurrent multi-writer, vector/FTS | as Kùzu | UC2 | ⚠️ MIT fork explicitly aimed at **AI-agent memory** (multi-writer). Same embeddability; same fork-risk. |
| **DuckDB (+ `duckpgq` PGQ / RDF ext)** | C++ amalgamation | SQL/**PGQ** property-graph; no native SPARQL | the user's store = 30.6 GB | UC1 | ⚠️ User's data lives here, but DuckDB is a heavy C++ build that does **not** drop cleanly into cosmocc. **Use DuckDB as the offline ETL tool**, not the APE runtime. |
| **Memgraph** | C++, **server**, in-memory, **BSL license** | Cypher | RAM-resident | UC2 | ❌ server + in-memory + BSL. Not embeddable. |
| **TerminusDB** | Rust/Prolog, **server** | WOQL/GraphQL | server | UC2 | ❌ Rust + server. |
| **Neo4j (embedded)** | **JVM** | Cypher | large | UC2 | ❌ JVM. (This is Graphiti's default backend, run as a *server*.) |
| **FalkorDB / FalkorDBLite** | C (Redis module / lib) | Cypher (GraphBLAS) | medium | UC2 | ⚠️ C core (sparse-matrix), but ships as a Redis module / Python lib; not a clean static cosmocc link. Possible far-future UC2. |
| **GraphLite / Grafeo / pgGraph** | Rust / Rust / Postgres-ext | GQL / — / SQL | new/small | UC2 | ❌ Rust or Postgres-bound; immature (late-2025 launches). Watch, don't adopt. |
| **Custom packed C entity store** | C | Qid→claims lookup; label index | smallest | UC1 | ✅ Fallback if SQLite per-row overhead bites at 113 M rows (sorted `Qid→offset` + packed blob + mmap). SQLite gives this for free first. |

### 3c. The KuzuDB catch (EVIDENCE — prioritized assessment)

Kùzu is, on paper, the *ideal* single-binary graph engine: **embedded in-process, written in
C++, MIT-licensed, single-file storage (since 0.11.0, Jul 2025), columnar + vectorized, with
built-in vector search and FTS, and Cypher**. It is purpose-built for exactly UC2 (AI-agent
memory) and can bulk-load a dump for UC1. **The disqualifier for *depending on it* is
maintenance, not technology:**
- **Upstream abandoned 2025-10-10** — the `kuzudb/kuzu` GitHub repo was archived without
  warning; final release **0.11.3**. A later EC filing indicates **Apple acqui-hired the
  team** ([The Register 2025-10-14](https://www.theregister.com/2025/10/14/kuzudb_abandoned/);
  [HN 45560036](https://news.ycombinator.com/item?id=45560036)).
- **Forks exist** but are young: **bighorn** (Kineviz) and a **Vela Partners** MIT fork aimed
  at agent memory ([Vela fork](https://github.com/Vela-Engineering/kuzu)). The single-file
  format change in 0.11 created migration friction.
- **cosmocc buildability is unproven** — Kùzu's CMake build pulls non-trivial third-party
  C++; "links statically in principle" ≠ "compiles under cosmocc." Would need a P-style spike.

**Verdict:** do **not** put Kùzu on the v1 critical path. For **UC1 it's overkill** (we only
read a frozen dump — SQLite wins). For **UC2 it's the most interesting candidate** but carries
**abandonment + unproven-cosmocc-build** risk; if agent-memory is built, prototype the
**Vela/bighorn fork under cosmocc** before committing, and keep a SQLite-graph fallback.

### 3d. Agent-memory frameworks (UC2) — Graphiti, and "Graphify"

- **Graphiti** (`getzep/graphiti`) — **Apache-2.0 Python** framework for **temporal
  knowledge-graph agent memory**. It ingests conversational **episodes**, uses an **LLM to
  extract entities/relations**, stores them in a **bi-temporal** graph (valid-time
  `t_valid`/`t_invalid` **and** ingestion-time), and serves **hybrid retrieval (semantic +
  BM25 + graph traversal)**. Backends: **Neo4j 5.26+, FalkorDB, Kùzu 0.11.2+, Neptune**
  ([github.com/getzep/graphiti](https://github.com/getzep/graphiti);
  [Zep docs](https://help.getzep.com/graphiti/getting-started/overview);
  [Neo4j blog](https://neo4j.com/blog/developer/graphiti-knowledge-graph-memory/)).
  **Fit:** squarely **UC2 (the agent's own memory), NOT static-Wikidata querying.** It is
  **Python + a graph-DB backend** → **not APE-embeddable as-is** (no Python runtime in the
  binary; needs Neo4j/Kùzu underneath). **What's portable is the *pattern*, not the package:**
  if llamafile builds agent long-term memory (doc 02/05), re-implement Graphiti's idea —
  *LLM-extract subject/predicate/object triples from turns, store with bitemporal validity,
  retrieve by hybrid search* — directly over the in-process store (SQLite-graph or a
  Kùzu-fork). The loaded model **is** the entity/relation extractor for free.
- **"Graphify"** (Neo4j Graphify, Kenny Bastani's `graphify`) — **INFERENCE / low-confidence:**
  an old (~2014-era) **Neo4j unmanaged-extension** for hierarchical-pattern NLP text
  classification. **JVM, long unmaintained.** Not relevant to either use case; ignore. (If the
  user meant a different "graphify," flag for clarification.)

(Sources: Kùzu — [kuzudb/kuzu](https://github.com/kuzudb/kuzu),
[The Data Quarry: Kùzu embedded](https://thedataquarry.com/blog/embedded-db-2/),
[The Register: abandoned](https://www.theregister.com/2025/10/14/kuzudb_abandoned/);
QLever — [ISWC 2025 Sparqloscope](https://ad-publications.cs.uni-freiburg.de/ISWC_sparqloscope_BKTU_2025.slides.pdf);
HDT — [rdfhdt/hdt-cpp](https://github.com/rdfhdt/hdt-cpp);
Oxigraph — [oxigraph/oxigraph](https://github.com/oxigraph/oxigraph);
Graphiti — [getzep/graphiti](https://github.com/getzep/graphiti).)

**Decisive read:** for **UC1** the only engine satisfying *all* APE constraints today is
**SQLite** (already vendored); every full-SPARQL/graph engine fails the embed test on
language/build/index/maintenance grounds → **SQLite in-binary, SPARQL out-of-process via MCP.**
For **UC2** the technology winner (embedded C++ Cypher = Kùzu-class) is real but **upstream-dead**;
adopt the **Graphiti *pattern* over an embeddable store**, prototype a **Kùzu-fork-under-cosmocc**
before betting on it, SQLite-graph as the safe fallback.

---

## 4. Bundling strategy

Full Wikidata is far too large to embed (≈101 GB JSON / ≈40 GB truthy / 30.6 GB even as the
user's parsed DuckDB). So, mirroring the `--zim` decision (doc 01 §4):

- **Primary: external `--wikidata <path.sqlite>`** (like `--zim` for big ZIMs). The artifact
  is the re-exported SQLite entity store (§2 subset). Opened read-only, `pread`/mmap per page;
  no full load.
- **Optional in-APE embed of a SMALL curated subset** via the existing **zip64 zipalign**
  layer (same mechanism that embeds the model and small ZIMs). Best small artifact: **top-N
  entities (sitelink/pageview-ranked) + English labels/aliases + best-rank claims**, ~0.5–2 GB
  → a self-contained "facts-llamafile". Add a thin `llamafile_open_*` returning
  `{fd, base_offset, size}` and open SQLite over that range (SQLite VFS can read from an
  fd+offset; or extract-to-tempfile on first run if the page-aligned VFS is fussy).

**Smallest genuinely-useful artifact.** Two tiers:
1. **`wikidata-lite` (curated, embeddable, ~0.5–2 GB):** top-N items, en labels+aliases,
   truthy values (entity + quantity + time), property labels. Answers the overwhelming
   majority of "fact about a famous thing" questions (Eiffel Tower height, country capital,
   person birthdate). FTS5 label index. **This is the recommended v1 bundle.**
2. **`wikidata-full-en` (external, ~8–15 GB):** all items, en labels+aliases, best-rank
   claims **with qualifiers**, property labels. The `--wikidata` external default for power
   users.

**Can an HDT of an en/truthy subset fit in tens of GB and be mmap-queried with no server?**
EVIDENCE says yes: ~100 GB RDF → ~10 GB HDT, so a **truthy-en HDT ≈ 15–25 GB**, mmap'd,
triple-pattern, no server — a legitimate alternative to SQLite **if** we want native RDF
triple-pattern semantics. But it buys no capability over the SQLite store for the LLM-fact
tool, at the cost of a new C++/LGPL dependency. **Defer HDT; revisit only if/when we add an
RDF/SPARQL story.**

---

## 5. LLM tool surface + CLI

Register as `server_tool`s in the same registry as `wiki_*` (doc 01 §3) — Wikidata is just
another inlined source. **Offline label resolution** is the crux: claims store Q/P numbers;
the same `id→label` table resolves them, so the tool returns human strings.

Minimal tools:
- **`wikidata_search(query, limit=10)` → `[{qid, label, description}]`.** FTS5 over
  labels+aliases (exact, prefix, token). Returns candidate Qids for disambiguation.
- **`wikidata_entity(qid, lang="en")` → `{qid, label, description, aliases, claims:[{property:{pid,label}, value:{type, qid?, label?, text?, amount?, unit?, time?}, qualifiers:[…]}]}`.**
  All P/Q numbers resolved to labels via the label table. Best-rank only by default.
- **`wikidata_property(qid, pid)` → `[value, …]`.** Direct fetch of one property's values
  (e.g. `wikidata_property("Q243","P2048") → [{amount:330, unit:"metre"}]`). Convenience over
  `entity`.
- **(Optional, deferred) `wikidata_sparql(query)`** → only when an external QLever/Oxigraph
  is configured (bridged via MCP host). Absent → tool not registered.

**Label→Qid disambiguation pattern:** `search` returns several Qids with descriptions; the
model (or the agent loop) picks (e.g. "Eiffel Tower the tower" vs homonyms — the probe showed
≥5 entities labelled exactly "Eiffel Tower"); then `entity`/`property` fetches facts. This is
the same propose-then-fetch loop the wiki tools use.

**CLI subcommand** (mirror the planned `wikipedia` CLI; **short-circuit before model init** so
it's instant):
```
llamafile wikidata search "Eiffel Tower"      # → Q243  Eiffel Tower  | tower on the Champ de Mars…
llamafile wikidata get Q243                    # → labels + resolved claims (P2048 height = 330 metre, …)
llamafile wikidata prop Q243 P2048             # → 330 metre
llamafile wikidata --wikidata /path.sqlite …   # external store; else use embedded curated bundle
```
The CLI and the `server_tool`s call the **same** C functions over the same SQLite handle.

---

## 6. Integration with Wikipedia / the platform

The two sources **compose**, they don't compete:
- **Wikidata = structured/exact** — "what is X's height/birthdate/capital/population",
  relations, units, dates. Deterministic, machine-checkable, tiny payloads.
- **Wikipedia ZIM = prose/context** — explanation, narrative, nuance for RAG.

Agent pattern: `wikidata_search`→`wikidata_entity` for the **exact fact**, then
`wiki_get_article` (via the Qid's sitelink title, which Wikidata stores) for **prose context**
— or the reverse (read the article, verify a number against Wikidata). The Qid↔Wikipedia-title
link (`sitelinks`) is the natural join key and should be retained in the store so the two
tools cross-reference. Both register in the one `server_tool` registry; the existing
agentic loop (doc 01/02) drives them uniformly. Delivery mirrors `--zim`: an external
`--wikidata` path plus an optional in-APE curated bundle.

---

## 7. Recommendation (v1) + biggest risk

**v1 (build order):**
1. **ETL with DuckDB (offline, host-side):** from `wikidata.ddb` (or a fresher
   `latest-all.json` if refresh is wanted), project to **English labels/desc/aliases +
   best-rank claims + qualifiers + property labels + sitelinks**, **normalizing value types**
   (entity / quantity+unit / time / string) and **resolving units to labels**. Emit two
   SQLite artifacts: `wikidata-full-en` (~8–15 GB, external) and `wikidata-lite` (curated
   top-N, ~0.5–2 GB, embeddable). Add **FTS5** over labels+aliases. (DuckDB is the *tool*
   here, never the runtime engine.)
2. **Reader + tools:** open SQLite (already in cosmopolitan) read-only; implement
   `wikidata_search/entity/property` C functions; register as `server_tool`s next to `wiki_*`;
   resolve P/Q→labels via the label table. Pure C, no new deps.
3. **CLI subcommand** `llamafile wikidata …`, short-circuiting before model init.
4. **Embed** `wikidata-lite` via the existing zipalign/zip64 layer + a `llamafile_open_*`
   fd+offset helper (same as the small-ZIM path).
5. **Defer SPARQL:** document the external **QLever/Oxigraph over MCP** bridge as the
   power-user path; register `wikidata_sparql` only when such a server is configured.
6. **(Optional, later)** semantic `label→Qid` using the user's existing nomic/bge ANN as an
   *external* retriever; and/or an **HDT** triple-pattern tier if an RDF story emerges.

**UC2 (agent dynamic KG / long-term memory) — separate workstream, after UC1.** Don't conflate
it with the Wikidata dump. When the multiagent platform (doc 02) + compaction/memory (doc 05)
need cross-session memory: re-implement the **Graphiti pattern in-process** — the loaded model
extracts `(subject, predicate, object)` triples from turns; store them with **bitemporal
validity** (`t_valid`/`t_invalid` + ingestion time) and serve **hybrid retrieval** (FTS + the
existing embedding path + graph traversal). Store backend, in order of preference under the APE
constraint: **(a) SQLite-graph** (zero new deps, safe, the default), **(b) a maintained
Kùzu fork (Vela/bighorn) prototyped under cosmocc** if real Cypher/graph-traversal perf is
needed — gate on a build spike given upstream is abandoned. **Do not** embed Graphiti itself
(Python + external Neo4j/Kùzu server) or Memgraph/TerminusDB/Neo4j (server/JVM/Rust).

**Biggest technical risk — claims fidelity, not the engine.** The engine is settled (SQLite,
already vendored). The real work and the real failure mode is the **ETL correctness**:
- **Rank:** the user's current parse keeps *all* values with **no rank** → it stores Eiffel
  Tower height as 300 **and** 324 **and** 330 m. v1 must keep **best-rank/truthy** so the tool
  returns a single right answer (or clearly labels historical/qualified values via P580/P582).
- **Units & value typing:** values are raw (`+330 x http://www.wikidata.org/entity/Q11573`);
  v1 must parse the amount, resolve the unit entity (Q11573→"metre"), and type each value, or
  the tool emits gibberish instead of "330 metre".
- **Label fallback:** some en labels are empty (e.g. Q42 in this dump) → need en→mul→any
  fallback or nameless entities.

Secondary risk: **source freshness** — the RDF/N-Triples dumps are broken since Jul 2025
([T403882](https://phabricator.wikimedia.org/T403882)); stay on the **JSON** lineage (which is
also what the user already has). If full SPARQL is ever mandated under the APE constraint and
deemed infeasible (it is, today), the pragmatic fallback is exactly this design:
**SQLite/HDT lookup + triple-pattern in-binary, SPARQL out-of-process via MCP.**

---

## 8. Open items / things to verify before building
- Re-export `wikidata.ddb` → SQLite size, with/without qualifiers (confirm the ~8–15 GB / ~4–8 GB
  estimates). Measure FTS5 index size at 82–113 M label rows.
- hdt-cpp exact license + cosmocc buildability (only if HDT tier is pursued).
- SQLite-over-fd+offset VFS vs extract-to-tempfile for the in-APE embed path.
- Best-rank extraction: re-parse from JSON (rank is in the source) vs recover from the
  existing DuckDB (rank was dropped → likely needs a JSON re-pass for the lite/full-en tiers).
- Pick the curated top-N selector (sitelink count / pageviews) and N for the embeddable bundle.
