// test_tool_parser — exercise vlib::parse_tool_call on the synthetic
// fixtures under tools/vlib-video/fixtures/tool_calls.jsonl.
//
// Fixture line shape:
//   {"input":"...", "expect_name":"...", "expect_args":{...},
//    "format":"...", "expect_parse_failure":bool?}
//
// We hand-roll the JSONL reader to avoid pulling nlohmann/json into the
// vlib-video CMake target. That keeps the test as small as the lib it
// exercises and matches the project's hand-rolled-parser style.

#include "vlib_video_tool_parser.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Tiny JSON-string unescape (mirrors what vlib's own parser does, but here
// we only need to decode test-vector strings, not arbitrary model output).
std::string unescape(const std::string & s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) { out += c; continue; }
        char n = s[++i];
        switch (n) {
            case 'n':  out += '\n'; break;
            case 't':  out += '\t'; break;
            case 'r':  out += '\r'; break;
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            default:   out += '\\'; out += n; break;
        }
    }
    return out;
}

// Read a JSON string starting at *i (which must point to the opening `"`).
// Advances *i past the closing `"`.
bool read_str(const std::string & s, size_t & i, std::string & out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    std::string raw;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '\\' && i < s.size()) {
            raw += '\\';
            raw += s[i++];
            continue;
        }
        if (c == '"') {
            out = unescape(raw);
            return true;
        }
        raw += c;
    }
    return false;
}

void skip_ws(const std::string & s, size_t & i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
}

// Parse a flat string-keyed JSON object (`{"k":"v","k2":"v2"}`) into a map.
// Used for both `expect_args` and the top-level fixture record.
struct fixture {
    std::string input;
    std::string expect_name;
    std::unordered_map<std::string, std::string> expect_args;
    std::string format;
    bool expect_parse_failure = false;
};

// Minimal record parser. The fixture format uses a known set of keys, so
// we don't need a general-purpose JSON parser. Returns false on malformed
// input.
bool parse_record(const std::string & line, fixture & out) {
    size_t i = 0;
    skip_ws(line, i);
    if (i >= line.size() || line[i] != '{') return false;
    ++i;
    while (i < line.size()) {
        skip_ws(line, i);
        if (i < line.size() && line[i] == '}') { ++i; return true; }
        std::string key;
        if (!read_str(line, i, key)) return false;
        skip_ws(line, i);
        if (i >= line.size() || line[i] != ':') return false;
        ++i;
        skip_ws(line, i);
        if (i >= line.size()) return false;

        if (key == "expect_args") {
            // Nested object — read flat string-to-string map (or empty).
            if (line[i] != '{') return false;
            ++i;
            while (i < line.size()) {
                skip_ws(line, i);
                if (line[i] == '}') { ++i; break; }
                std::string k;
                if (!read_str(line, i, k)) return false;
                skip_ws(line, i);
                if (i >= line.size() || line[i] != ':') return false;
                ++i;
                skip_ws(line, i);
                std::string v;
                if (!read_str(line, i, v)) return false;
                out.expect_args[k] = v;
                skip_ws(line, i);
                if (i < line.size() && line[i] == ',') ++i;
            }
        } else if (key == "expect_parse_failure") {
            // bool literal
            if (line.compare(i, 4, "true") == 0)  { out.expect_parse_failure = true;  i += 4; }
            else if (line.compare(i, 5, "false") == 0) { out.expect_parse_failure = false; i += 5; }
            else return false;
        } else {
            std::string v;
            if (!read_str(line, i, v)) return false;
            if      (key == "input")       out.input = v;
            else if (key == "expect_name") out.expect_name = v;
            else if (key == "format")      out.format = v;
        }
        skip_ws(line, i);
        if (i < line.size() && line[i] == ',') ++i;
    }
    return false;
}

bool maps_equal(const std::unordered_map<std::string, std::string> & a,
                const std::unordered_map<std::string, std::string> & b) {
    if (a.size() != b.size()) return false;
    for (const auto & kv : a) {
        auto it = b.find(kv.first);
        if (it == b.end()) return false;
        if (it->second != kv.second) return false;
    }
    return true;
}

} // namespace

int main(int argc, char ** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s fixtures.jsonl\n", argv[0]);
        return 2;
    }
    std::ifstream f(argv[1]);
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    int total = 0;
    int passed = 0;
    int failed = 0;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        fixture fx;
        if (!parse_record(line, fx)) {
            fprintf(stderr, "FATAL: malformed fixture line:\n%s\n", line.c_str());
            return 3;
        }
        total++;

        vlib::tool_call tc;
        bool got = vlib::parse_tool_call(fx.input, tc);

        bool ok = true;
        std::string why;
        if (fx.expect_parse_failure) {
            if (got) { ok = false; why = "expected parse failure but got success"; }
        } else {
            if (!got) {
                ok = false; why = "expected success but parse_tool_call returned false";
            } else {
                if (tc.name != fx.expect_name) {
                    ok = false;
                    why = "name mismatch: got '" + tc.name + "' want '" + fx.expect_name + "'";
                } else if (!maps_equal(tc.arguments, fx.expect_args)) {
                    ok = false;
                    std::ostringstream oss;
                    oss << "args mismatch: got {";
                    for (const auto & kv : tc.arguments) oss << kv.first << "=" << kv.second << ", ";
                    oss << "} want {";
                    for (const auto & kv : fx.expect_args) oss << kv.first << "=" << kv.second << ", ";
                    oss << "}";
                    why = oss.str();
                }
            }
        }

        if (ok) {
            passed++;
        } else {
            failed++;
            fprintf(stderr, "FAIL [%s]: %s\n  input: %s\n", fx.format.c_str(), why.c_str(), fx.input.c_str());
        }
    }
    fprintf(stderr, "tool-parser: %d/%d passed\n", passed, total);
    return failed == 0 ? 0 : 1;
}
