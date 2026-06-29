// vlib_video_tool_parser.cpp — hand-rolled <tool_call> parser.
//
// Mirrors mlx_vlm/continuous_analyzer.py:parse_tool_call (lines 98-146):
//
//   1. Try Qwen2.5 JSON between <tool_call>...</tool_call>
//   2. If unclosed, try Qwen2.5 JSON with no closing </tool_call>
//   3. Try Qwen3.5 XML <function=name>...</function>
//   4. Last resort: bare <function=name> with no closing markers.
//
// The Python uses `re.search(... re.DOTALL)`. We do not bring in std::regex
// because:
//   - libstdc++ < 11 has documented catastrophic-backtracking bugs on the
//     greedy `.*?` patterns we'd need.
//   - We do not need any regex feature beyond literal-prefix scanning, so
//     a state machine is both faster and easier to audit.

#include "vlib_video_tool_parser.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace vlib {

namespace {

// Locate the next occurrence of `needle` starting at `from`.
// Returns std::string::npos if not found.
size_t find_at(const std::string & hay, const char * needle, size_t from) {
    return hay.find(needle, from);
}

// Skip ASCII whitespace.
size_t skip_ws(const std::string & s, size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    return i;
}

// ---------------------------------------------------------------------------
// Minimal JSON scanner — finds the matching '}' starting at a '{' position,
// honoring string/escape rules. Returns npos if no balanced match.
// ---------------------------------------------------------------------------
size_t find_matching_brace(const std::string & s, size_t open_pos) {
    if (open_pos >= s.size() || s[open_pos] != '{') {
        return std::string::npos;
    }
    int depth = 0;
    bool in_str = false;
    bool esc    = false;
    for (size_t i = open_pos; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (esc) { esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"')  { in_str = false; continue; }
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') { ++depth; }
        else if (c == '}') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

// ---------------------------------------------------------------------------
// Tiny ad-hoc JSON value reader — only what `parse_tool_call` needs:
//   - object   { "k": v, ... }
//   - string   "..."
//   - number   (we keep raw text)
//   - bool     true/false
//   - null
//   - nested object/array (kept as raw text)
//
// We return string representations only; nested structure is preserved
// verbatim for the `arguments` map.
// ---------------------------------------------------------------------------

struct json_parser {
    const std::string & s;
    size_t i;
    bool ok;

    json_parser(const std::string & s_) : s(s_), i(0), ok(true) {}

    void skip() { i = skip_ws(s, i); }

    bool match(char c) {
        skip();
        if (i < s.size() && s[i] == c) { ++i; return true; }
        return false;
    }

    bool peek(char c) {
        skip();
        return i < s.size() && s[i] == c;
    }

    // Read a JSON string (content between the surrounding quotes), unescaping
    // a small set of common escapes. Leaves `i` past the closing quote.
    std::string read_string() {
        skip();
        std::string out;
        if (i >= s.size() || s[i] != '"') { ok = false; return out; }
        ++i;
        bool esc = false;
        while (i < s.size()) {
            char c = s[i++];
            if (esc) {
                switch (c) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        // basic BMP unicode; we keep \uXXXX literally if not in
                        // basic ASCII to avoid UTF-8 reencoding here.
                        if (i + 4 > s.size()) { ok = false; return out; }
                        unsigned codepoint = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i++];
                            codepoint <<= 4;
                            if      (h >= '0' && h <= '9') codepoint |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') codepoint |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') codepoint |= (unsigned)(h - 'A' + 10);
                            else { ok = false; return out; }
                        }
                        if (codepoint < 0x80) {
                            out += (char)codepoint;
                        } else if (codepoint < 0x800) {
                            out += (char)(0xC0 | (codepoint >> 6));
                            out += (char)(0x80 | (codepoint & 0x3F));
                        } else {
                            out += (char)(0xE0 | (codepoint >> 12));
                            out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                            out += (char)(0x80 | (codepoint & 0x3F));
                        }
                        break;
                    }
                    default: out += c; break;
                }
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                return out;
            } else {
                out += c;
            }
        }
        ok = false;
        return out;
    }

    // Read any JSON value, returning its raw text (verbatim from the source,
    // outer whitespace trimmed, surrounding quotes preserved for strings).
    // Used to preserve nested object/array values in the arguments map.
    std::string read_raw_value() {
        skip();
        if (i >= s.size()) { ok = false; return {}; }
        char c = s[i];
        size_t start = i;
        if (c == '"') {
            // string — consume to matching quote
            ++i;
            bool esc = false;
            while (i < s.size()) {
                char d = s[i++];
                if (esc) { esc = false; continue; }
                if (d == '\\') { esc = true; continue; }
                if (d == '"')  { return s.substr(start, i - start); }
            }
            ok = false;
            return {};
        }
        if (c == '{' || c == '[') {
            // object/array — find matching close at the same depth
            int depth = 0;
            bool in_str = false;
            bool esc = false;
            char open  = c;
            char close = (c == '{') ? '}' : ']';
            for (; i < s.size(); ++i) {
                char d = s[i];
                if (in_str) {
                    if (esc) { esc = false; continue; }
                    if (d == '\\') { esc = true; continue; }
                    if (d == '"')  { in_str = false; continue; }
                    continue;
                }
                if (d == '"') { in_str = true; continue; }
                if (d == open)  { ++depth; }
                else if (d == close) {
                    --depth;
                    if (depth == 0) { ++i; return s.substr(start, i - start); }
                }
            }
            ok = false;
            return {};
        }
        // bareword: number / true / false / null — terminate at , } ]
        while (i < s.size()) {
            char d = s[i];
            if (d == ',' || d == '}' || d == ']' || std::isspace(static_cast<unsigned char>(d))) break;
            ++i;
        }
        return s.substr(start, i - start);
    }
};

// Strip surrounding quotes from a JSON string-value's raw form.
std::string unwrap_json_string(const std::string & raw) {
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        // re-parse to apply unescaping
        json_parser jp(raw);
        return jp.read_string();
    }
    return raw;
}

// ---------------------------------------------------------------------------
// Stage 1/2: Qwen2.5 JSON form.
//
//   <tool_call>  WS  { "name": "...", "arguments": { ... } }  WS  </tool_call>
//
// We accept missing close tag (model truncated) and extra trailing content
// after the close tag.
// ---------------------------------------------------------------------------
bool try_parse_json_form(const std::string & text, tool_call & out) {
    static const char OPEN[]  = "<tool_call>";
    static const char CLOSE[] = "</tool_call>";

    size_t open_pos = text.find(OPEN);
    if (open_pos == std::string::npos) return false;

    size_t after_open = open_pos + sizeof(OPEN) - 1;
    size_t brace_pos  = text.find('{', after_open);
    if (brace_pos == std::string::npos) return false;

    size_t end_brace = find_matching_brace(text, brace_pos);
    if (end_brace == std::string::npos) return false;

    // Optional close tag — if present, must follow brace_pos's close.
    size_t close_pos = text.find(CLOSE, end_brace + 1);
    (void)close_pos; // we don't require it

    std::string json_obj = text.substr(brace_pos, end_brace - brace_pos + 1);

    json_parser jp(json_obj);
    if (!jp.match('{')) return false;

    std::string name;
    std::string raw_arguments;
    bool first = true;
    while (true) {
        jp.skip();
        if (jp.peek('}')) { jp.match('}'); break; }
        if (!first) {
            if (!jp.match(',')) return false;
        }
        first = false;
        std::string key = jp.read_string();
        if (!jp.ok) return false;
        if (!jp.match(':')) return false;
        jp.skip();
        if (key == "name") {
            name = jp.read_string();
            if (!jp.ok) return false;
        } else if (key == "arguments" || key == "parameters") {
            raw_arguments = jp.read_raw_value();
            if (!jp.ok) return false;
        } else {
            // unknown top-level key — preserve via a raw skip
            (void)jp.read_raw_value();
            if (!jp.ok) return false;
        }
    }
    if (name.empty()) {
        return false;
    }
    out.name = name;
    out.arguments.clear();

    // Parse arguments object (if any) into a string-only map.
    std::string args = raw_arguments;
    // Strip whitespace
    {
        size_t a = 0;
        while (a < args.size() && std::isspace(static_cast<unsigned char>(args[a]))) ++a;
        size_t b = args.size();
        while (b > a && std::isspace(static_cast<unsigned char>(args[b-1]))) --b;
        args = args.substr(a, b - a);
    }
    if (args.empty() || args == "null") {
        return true;
    }
    if (args.size() < 2 || args.front() != '{') {
        // Not an object — drop arguments silently (matches Python behavior of
        // .get("arguments", {}) when arguments is malformed).
        return true;
    }
    json_parser ap(args);
    if (!ap.match('{')) return true;
    bool first_a = true;
    while (true) {
        ap.skip();
        if (ap.peek('}')) { ap.match('}'); break; }
        if (!first_a) {
            if (!ap.match(',')) return true;
        }
        first_a = false;
        std::string k = ap.read_string();
        if (!ap.ok) break;
        if (!ap.match(':')) break;
        std::string v = ap.read_raw_value();
        if (!ap.ok) break;
        out.arguments[k] = unwrap_json_string(v);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Stage 3/4: Qwen3.5 XML form.
//
//   <tool_call>
//     <function=NAME>
//       <parameter=KEY> VALUE </parameter>
//       ...
//     </function>
//     [</tool_call>]   (optional)
//
// We mirror the Python: accept missing closing tags, and preserve VALUE
// verbatim (with leading/trailing whitespace stripped).
// ---------------------------------------------------------------------------
bool try_parse_xml_form(const std::string & text, tool_call & out) {
    static const char TC_OPEN[]    = "<tool_call>";
    static const char FN_OPEN[]    = "<function=";
    static const char FN_CLOSE[]   = "</function>";
    static const char PRM_OPEN[]   = "<parameter=";
    static const char PRM_CLOSE[]  = "</parameter>";

    size_t tc_pos = text.find(TC_OPEN);
    if (tc_pos == std::string::npos) return false;

    size_t fn_pos = text.find(FN_OPEN, tc_pos);
    if (fn_pos == std::string::npos) return false;

    size_t name_start = fn_pos + sizeof(FN_OPEN) - 1;
    size_t name_end   = text.find('>', name_start);
    if (name_end == std::string::npos) return false;

    std::string name = text.substr(name_start, name_end - name_start);
    // Trim
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))  name.pop_back();
    size_t lead = 0;
    while (lead < name.size() && std::isspace(static_cast<unsigned char>(name[lead]))) ++lead;
    name = name.substr(lead);
    if (name.empty()) return false;

    out.name = name;
    out.arguments.clear();

    size_t scan_from = name_end + 1;
    // Hard upper bound — stop scanning at </function> (or end of input).
    size_t hard_end = text.find(FN_CLOSE, scan_from);
    if (hard_end == std::string::npos) {
        hard_end = text.size();
    }

    while (true) {
        size_t p_open = text.find(PRM_OPEN, scan_from);
        if (p_open == std::string::npos || p_open >= hard_end) break;
        size_t key_start = p_open + sizeof(PRM_OPEN) - 1;
        size_t key_end   = text.find('>', key_start);
        if (key_end == std::string::npos || key_end >= hard_end) break;
        std::string key = text.substr(key_start, key_end - key_start);
        // Trim key
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        size_t klead = 0;
        while (klead < key.size() && std::isspace(static_cast<unsigned char>(key[klead]))) ++klead;
        key = key.substr(klead);

        size_t val_start = key_end + 1;
        size_t val_end   = text.find(PRM_CLOSE, val_start);
        std::string value;
        if (val_end == std::string::npos || val_end >= hard_end) {
            // unclosed parameter — take to hard_end
            val_end = hard_end;
            value = text.substr(val_start, val_end - val_start);
            // Trim
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
            size_t vlead = 0;
            while (vlead < value.size() && std::isspace(static_cast<unsigned char>(value[vlead]))) ++vlead;
            value = value.substr(vlead);
            out.arguments[key] = value;
            scan_from = hard_end;
            break;
        } else {
            value = text.substr(val_start, val_end - val_start);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
            size_t vlead = 0;
            while (vlead < value.size() && std::isspace(static_cast<unsigned char>(value[vlead]))) ++vlead;
            value = value.substr(vlead);
            out.arguments[key] = value;
            scan_from = val_end + sizeof(PRM_CLOSE) - 1;
        }
    }
    return true;
}

} // namespace

bool parse_tool_call(const std::string & text, tool_call & out) {
    // Order matches the Python: JSON first, XML fallback last.
    if (try_parse_json_form(text, out)) return true;
    if (try_parse_xml_form (text, out)) return true;
    return false;
}

std::vector<tool_call> parse_tool_calls(const std::string & text) {
    std::vector<tool_call> out;
    size_t cursor = 0;
    while (cursor < text.size()) {
        size_t open_pos = text.find("<tool_call>", cursor);
        if (open_pos == std::string::npos) break;
        // Find scope end: prefer </tool_call>, fall back to next <tool_call> or EOS.
        size_t scope_end = text.find("</tool_call>", open_pos);
        size_t next_open = text.find("<tool_call>", open_pos + 1);
        size_t end;
        if (scope_end != std::string::npos &&
            (next_open == std::string::npos || scope_end < next_open)) {
            end = scope_end + std::string("</tool_call>").size();
        } else if (next_open != std::string::npos) {
            end = next_open;
        } else {
            end = text.size();
        }
        std::string slice = text.substr(open_pos, end - open_pos);
        tool_call tc;
        if (parse_tool_call(slice, tc)) {
            out.push_back(std::move(tc));
        }
        cursor = end;
    }
    return out;
}

} // namespace vlib
