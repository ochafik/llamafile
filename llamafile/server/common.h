// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;tab-width:8;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Wrapper header for llama.cpp common functionality
// Used by tool calling code

#pragma once

// Include llama.cpp headers (full path from repo root)
#include "llama.cpp/common/common.h"

#include <string>
#include <string_view>

// [llamafile] Compatibility define for enum value removed from llama.cpp
// COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL was renamed to PATTERN
#ifndef COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL
#define COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN
#endif

// [llamafile] UTF-8 parsing utilities for streaming-aware unicode support
// These were removed from llama.cpp but are needed for peg-parser

struct utf8_parse_result {
    uint32_t codepoint;      // Decoded codepoint (only valid if status == SUCCESS)
    size_t bytes_consumed;   // How many bytes this codepoint uses (1-4)
    enum status_t { SUCCESS, INCOMPLETE, INVALID } status;

    utf8_parse_result(enum status_t s, uint32_t cp = 0, size_t bytes = 0)
        : codepoint(cp), bytes_consumed(bytes), status(s) {}
};

inline utf8_parse_result parse_utf8_codepoint(std::string_view input, size_t offset) {
    if (offset >= input.size()) {
        return utf8_parse_result(utf8_parse_result::INCOMPLETE);
    }

    // ASCII fast path
    if (!(input[offset] & 0x80)) {
        return utf8_parse_result(utf8_parse_result::SUCCESS, input[offset], 1);
    }

    // Invalid: continuation byte as first byte
    if (!(input[offset] & 0x40)) {
        return utf8_parse_result(utf8_parse_result::INVALID);
    }

    // 2-byte sequence
    if (!(input[offset] & 0x20)) {
        if (offset + 1 >= input.size()) {
            return utf8_parse_result(utf8_parse_result::INCOMPLETE);
        }
        if ((input[offset + 1] & 0xc0) != 0x80) {
            return utf8_parse_result(utf8_parse_result::INVALID);
        }
        auto result = ((input[offset] & 0x1f) << 6) | (input[offset + 1] & 0x3f);
        return utf8_parse_result(utf8_parse_result::SUCCESS, result, 2);
    }

    // 3-byte sequence
    if (!(input[offset] & 0x10)) {
        if (offset + 2 >= input.size()) {
            return utf8_parse_result(utf8_parse_result::INCOMPLETE);
        }
        if ((input[offset + 1] & 0xc0) != 0x80 || (input[offset + 2] & 0xc0) != 0x80) {
            return utf8_parse_result(utf8_parse_result::INVALID);
        }
        auto result = ((input[offset] & 0x0f) << 12) | ((input[offset + 1] & 0x3f) << 6) | (input[offset + 2] & 0x3f);
        return utf8_parse_result(utf8_parse_result::SUCCESS, result, 3);
    }

    // 4-byte sequence
    if (!(input[offset] & 0x08)) {
        if (offset + 3 >= input.size()) {
            return utf8_parse_result(utf8_parse_result::INCOMPLETE);
        }
        if ((input[offset + 1] & 0xc0) != 0x80 || (input[offset + 2] & 0xc0) != 0x80 || (input[offset + 3] & 0xc0) != 0x80) {
            return utf8_parse_result(utf8_parse_result::INVALID);
        }
        auto result = ((input[offset] & 0x07) << 18) | ((input[offset + 1] & 0x3f) << 12) | ((input[offset + 2] & 0x3f) << 6) | (input[offset + 3] & 0x3f);
        return utf8_parse_result(utf8_parse_result::SUCCESS, result, 4);
    }

    // Invalid first byte
    return utf8_parse_result(utf8_parse_result::INVALID);
}

// [llamafile] Utility functions for partial stop word detection
// These were removed from llama.cpp common but are still needed for chat parsing

inline bool string_ends_with(const std::string_view & str, const std::string_view & suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Finds if the end of `str` contains a partial match of `stop`.
// Returns the position in `str` where the partial match begins, or npos if no partial match.
// For example: string_find_partial_stop("hello wo", "world") returns 6 (position of "wo")
inline size_t string_find_partial_stop(const std::string_view & str, const std::string_view & stop) {
    if (!str.empty() && !stop.empty()) {
        const char text_last_char = str.back();
        for (int64_t char_index = stop.size() - 1; char_index >= 0; char_index--) {
            if (stop[char_index] == text_last_char) {
                const auto current_partial = stop.substr(0, char_index + 1);
                if (string_ends_with(str, current_partial)) {
                    return str.size() - char_index - 1;
                }
            }
        }
    }
    return std::string::npos;
}
