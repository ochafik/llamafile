// vlib_video_tool_parser.h — port of mlx_vlm/continuous_analyzer.py:parse_tool_call
//
// Parses the two flavours of Qwen tool-call output we see from
// mlx-vlm-continuous's watchdawg pipeline:
//
//   Qwen2.5 JSON: <tool_call>{"name": ..., "arguments": {...}}</tool_call>
//   Qwen3.5 XML:  <tool_call><function=name><parameter=p>v</parameter></function></tool_call>
//
// Hand-rolled, no <regex> dependency — keeps the parser deterministic across
// libstdc++/libc++ and avoids the catastrophic backtracking a few of the JSON
// fixtures would otherwise hit.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace vlib {

struct tool_call {
    // Canonical action name. The MLX reference treats `do_nothing` as the
    // canonical "no signal" tool; we accept `ignore_frame` as a synonym
    // (older watchdawg recordings use it) and surface both verbatim — the
    // session's process_frame state machine handles aliasing.
    std::string name;

    // String-only arguments. We deliberately do not parse nested JSON values
    // — the only fields the watchdawg pipeline uses are scalars (text,
    // observation, etc.). If a JSON value is an object/array we serialize it
    // back to its raw text form so the caller can re-parse if needed.
    std::unordered_map<std::string, std::string> arguments;
};

// Parse a single <tool_call> ... </tool_call> block out of `text`.
// Returns true on success and fills `out`. Returns false if no parseable tool
// call is found.
//
// Behaviour mirrors the Python reference:
//   1. Try Qwen2.5 JSON between the open/close tags.
//   2. If unclosed, fall back to a JSON-without-close-tag form (model
//      truncated mid-stream).
//   3. Otherwise try Qwen3.5 XML <function=name>...<parameter=p>v</parameter>...
//   4. As a last resort, accept a bare <function=name> opener with optional
//      parameters and no closing markers.
bool parse_tool_call(const std::string & text, tool_call & out);

// Convenience: returns all tool calls in the text. Useful for fixtures
// that contain multiple <tool_call> blocks back-to-back.
std::vector<tool_call> parse_tool_calls(const std::string & text);

} // namespace vlib
