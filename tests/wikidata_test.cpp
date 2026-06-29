// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2026 Mozilla.ai
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//
// Unit test for the offline Wikidata structured-fact reader
// (llamafile/wikidata.{h,cpp}). It builds a TINY in-process SQLite+FTS5 store
// matching the production schema (see wikidata.h), then exercises the public
// reader over it. Nothing is committed: the fixture is created in a temp file
// at startup with the vendored sqlite (third_party/sqlite) and unlinked at the
// end, so the test is fully hermetic.
//
// The fixture models the canonical example: Q243 "Eiffel Tower" with a P2048
// (height) quantity claim whose UNIT is Q11573 (metre) — both present in the
// store, so unit P->Q resolution is exercised — plus a P580 (start time)
// qualifier and a P17 (country) -> Q142 (France) entity-valued claim. Property
// ids (P2048/P580/P17) and the country target (Q142) carry their own label
// rows so claim/qualifier/unit/entity resolution all have something to resolve.

#include "wikidata.h"

#include "sqlite/sqlite3.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        g_checks++;                                                   \
        if (!(cond)) {                                                \
            g_fails++;                                                \
            fprintf(stderr, "FAIL: %s\n  at %s:%d\n", (msg),         \
                    __FILE__, __LINE__);                              \
        }                                                            \
    } while (0)

// ---------------------------------------------------------------------------
// Build the fixture store (matches wikidata.cpp's expected schema exactly).
// ---------------------------------------------------------------------------
static bool exec(sqlite3 *db, const char *sql) {
    char *err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite exec failed: %s\n  sql: %.120s\n",
                err ? err : "?", sql);
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

static bool build_fixture(const char *path) {
    sqlite3 *db = nullptr;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "cannot create fixture db: %s\n", sqlite3_errmsg(db));
        return false;
    }
    bool ok =
        exec(db,
             "CREATE TABLE entity("
             "  id TEXT, label TEXT, description TEXT, aliases TEXT, claims TEXT);"
             "CREATE UNIQUE INDEX entity_id ON entity(id);"
             "CREATE VIRTUAL TABLE fts USING fts5(label, aliases);") &&

        // Entities. rowid is explicit so entity.rowid == fts.rowid (the search
        // SQL joins on it). claims is the nested [ [ [P,val], <qualifiers...> ] ]
        // JSON the production ETL emits.
        exec(db,
             "INSERT INTO entity(rowid,id,label,description,aliases,claims) VALUES"
             // Q243: height = +330 x <unit Q11573>, start-time qualifier P580,
             // and a country claim P17 -> Q142.
             "(1,'Q243','Eiffel Tower','wrought-iron lattice tower in Paris',"
             "  '[\"Tour Eiffel\",\"La dame de fer\"]',"
             "  '[[[\"P2048\",\"+330 x http://www.wikidata.org/entity/Q11573\"],"
             "     [\"P580\",\"+1889-03-31T00:00:00Z\"]],"
             "    [[\"P17\",\"Q142\"]]]'),"
             // Q90 Paris (for the OR-query recall test).
             "(2,'Q90','Paris','capital of France','[\"City of Light\"]','[]'),"
             // Unit + target + property-label rows (no claims of their own).
             "(3,'Q11573','metre','SI base unit of length','[]','[]'),"
             "(4,'Q142','France','country in Western Europe','[]','[]'),"
             "(5,'P2048','height','vertical distance','[]','[]'),"
             "(6,'P580','start time','','[]','[]'),"
             "(7,'P17','country','sovereign state of this item','[]','[]');") &&

        // FTS index rows (rowid-aligned with entity).
        exec(db,
             "INSERT INTO fts(rowid,label,aliases) VALUES"
             "(1,'Eiffel Tower','Tour Eiffel La dame de fer'),"
             "(2,'Paris','City of Light'),"
             "(3,'metre',''),"
             "(4,'France',''),"
             "(5,'height',''),"
             "(6,'start time',''),"
             "(7,'country','');");

    sqlite3_close(db);
    return ok;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static bool hits_contain(const std::vector<wikidata_hit> &hits, const char *id) {
    for (const auto &h : hits) if (h.id == id) return true;
    return false;
}

int main(int argc, char **argv) {
    char path[] = "/tmp/wikidata_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);  // sqlite reopens it by name
    printf("=== Wikidata reader tests (fixture: %s) ===\n", path);

    if (!build_fixture(path)) {
        fprintf(stderr, "could not build fixture\n");
        unlink(path);
        return 1;
    }

    wikidata_store *s = wikidata_open(path);
    CHECK(s != nullptr, "wikidata_open succeeds on the fixture");
    if (!s) { unlink(path); return 1; }

    // --- FTS5 search by label -> id ---
    {
        auto hits = wikidata_search(s, "Eiffel Tower", 10);
        CHECK(!hits.empty(), "search 'Eiffel Tower' returns hits");
        CHECK(!hits.empty() && hits[0].id == "Q243",
              "exact-label match boosts Q243 to the top");
        CHECK(!hits.empty() && hits[0].label == "Eiffel Tower",
              "top hit label is 'Eiffel Tower'");
    }
    {   // alias-only term still resolves via the indexed aliases column
        auto hits = wikidata_search(s, "metre", 10);
        CHECK(hits_contain(hits, "Q11573"), "search 'metre' finds Q11573");
    }

    // --- FTS5 boolean / OR'd query passes through verbatim ---
    {
        auto hits = wikidata_search(s, "Eiffel OR Paris", 10);
        CHECK(hits_contain(hits, "Q243") && hits_contain(hits, "Q90"),
              "FTS5 'Eiffel OR Paris' returns both Q243 and Q90");
    }

    // --- entity fetch: label + description + decoded/resolved claims ---
    {
        wikidata_entity e = wikidata_get(s, "Q243");
        CHECK(e.found, "wikidata_get(Q243) found");
        CHECK(e.label == "Eiffel Tower", "entity label");
        CHECK(e.description.find("tower") != std::string::npos,
              "entity description decoded");
        bool has_alias = false;
        for (auto &a : e.aliases) if (a == "Tour Eiffel") has_alias = true;
        CHECK(has_alias, "aliases decoded (contains 'Tour Eiffel')");
        CHECK(e.claims.size() == 2, "Q243 has 2 decoded statements");

        if (e.claims.size() >= 2) {
            const auto &st = e.claims[0];  // P2048 height
            CHECK(st.pid == "P2048", "claim[0] pid P2048");
            CHECK(st.prop_label == "height", "claim[0] property resolved to 'height'");
            CHECK(st.value.type == "quantity", "claim[0] value type quantity");
            CHECK(st.value.text == "330 metre",
                  "claim[0] quantity unit resolved (P->Q Q11573 -> 'metre')");
            // qualifier P580 start time -> precision-aware date
            CHECK(st.qualifiers.size() == 1, "claim[0] has 1 qualifier");
            if (st.qualifiers.size() == 1) {
                CHECK(st.qualifiers[0].pid == "P580", "qualifier pid P580");
                CHECK(st.qualifiers[0].prop_label == "start time",
                      "qualifier property resolved to 'start time'");
                CHECK(st.qualifiers[0].value.type == "time",
                      "qualifier value type time");
                CHECK(st.qualifiers[0].value.text == "1889-03-31",
                      "qualifier time formatted to '1889-03-31'");
            }
            const auto &st2 = e.claims[1];  // P17 country -> Q142 (entity)
            CHECK(st2.pid == "P17", "claim[1] pid P17");
            CHECK(st2.value.type == "entity", "claim[1] value type entity");
            CHECK(st2.value.id == "Q142", "claim[1] entity id Q142");
            CHECK(st2.value.text == "France",
                  "claim[1] entity P->Q resolved to label 'France'");
        }
    }

    // --- property lookup convenience: value with resolved unit ---
    {
        auto vals = wikidata_property(s, "Q243", "P2048");
        CHECK(vals.size() == 1, "wikidata_property(Q243,P2048) returns 1 statement");
        if (vals.size() == 1) {
            CHECK(vals[0].value.text == "330 metre",
                  "property value '330 metre' (unit resolved)");
            CHECK(vals[0].prop_label == "height", "property label 'height'");
        }
        auto none = wikidata_property(s, "Q243", "P9999");
        CHECK(none.empty(), "absent property -> empty");
    }

    // --- missing entity ---
    {
        wikidata_entity e = wikidata_get(s, "Q999999");
        CHECK(!e.found, "missing id -> not found");
    }

    wikidata_close(s);
    unlink(path);

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails) {
        printf("WIKIDATA TESTS FAILED\n");
        return 1;
    }
    printf("ALL WIKIDATA TESTS PASSED\n");
    return 0;
}
