// -*- mode:c;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=c ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2024 Mozilla Foundation
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

#include "zim.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Parser states
enum {
    STATE_TEXT,           // Outside any tag
    STATE_TAG_START,      // Just saw '<'
    STATE_TAG_NAME,       // Reading tag name
    STATE_TAG_BODY,       // Inside tag (attributes)
    STATE_TAG_QUOTE,      // Inside single-quoted attribute
    STATE_TAG_DQUOTE,     // Inside double-quoted attribute
    STATE_COMMENT_1,      // Saw '<!-'
    STATE_COMMENT,        // Inside <!-- comment
    STATE_COMMENT_END_1,  // Saw '-' in comment
    STATE_COMMENT_END_2,  // Saw '--' in comment
    STATE_ENTITY,         // Inside &entity;
    STATE_SCRIPT,         // Inside <script>
    STATE_STYLE,          // Inside <style>
};

// Block-level tags that should insert newlines
static const char *block_tags[] = {
    "p", "div", "br", "hr", "h1", "h2", "h3", "h4", "h5", "h6",
    "li", "tr", "blockquote", "pre", "article", "section",
    "header", "footer", "table", "ul", "ol", "dl", "dt", "dd",
    NULL
};

// Tags whose content should be stripped entirely
static const char *strip_tags[] = {
    "script", "style", "noscript", "template",
    NULL
};

static int str_in_list(const char *tag, size_t len, const char **list) {
    for (const char **p = list; *p; p++) {
        if (strlen(*p) == len && strncasecmp(*p, tag, len) == 0)
            return 1;
    }
    return 0;
}

// UTF-8 encode a Unicode codepoint
static size_t utf8_encode(unsigned long cp, char *out) {
    if (cp < 0x80) {
        out[0] = cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = 0xC0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3F);
        return 2;
    } else if (cp < 0x10000) {
        out[0] = 0xE0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3F);
        out[2] = 0x80 | (cp & 0x3F);
        return 3;
    } else if (cp < 0x110000) {
        out[0] = 0xF0 | (cp >> 18);
        out[1] = 0x80 | ((cp >> 12) & 0x3F);
        out[2] = 0x80 | ((cp >> 6) & 0x3F);
        out[3] = 0x80 | (cp & 0x3F);
        return 4;
    }
    return 0;
}

// Common HTML entities
static const struct {
    const char *name;
    const char *value;
} named_entities[] = {
    {"amp", "&"},
    {"lt", "<"},
    {"gt", ">"},
    {"nbsp", " "},
    {"quot", "\""},
    {"apos", "'"},
    {"ndash", "\xe2\x80\x93"},  // –
    {"mdash", "\xe2\x80\x94"},  // —
    {"lsquo", "\xe2\x80\x98"},  // '
    {"rsquo", "\xe2\x80\x99"},  // '
    {"ldquo", "\xe2\x80\x9c"},  // "
    {"rdquo", "\xe2\x80\x9d"},  // "
    {"bull", "\xe2\x80\xa2"},   // •
    {"hellip", "\xe2\x80\xa6"}, // …
    {"copy", "\xc2\xa9"},       // ©
    {"reg", "\xc2\xae"},        // ®
    {"trade", "\xe2\x84\xa2"},  // ™
    {"euro", "\xe2\x82\xac"},   // €
    {"pound", "\xc2\xa3"},      // £
    {"yen", "\xc2\xa5"},        // ¥
    {"cent", "\xc2\xa2"},       // ¢
    {"deg", "\xc2\xb0"},        // °
    {"plusmn", "\xc2\xb1"},     // ±
    {"times", "\xc3\x97"},      // ×
    {"divide", "\xc3\xb7"},     // ÷
    {"frac12", "\xc2\xbd"},     // ½
    {"frac14", "\xc2\xbc"},     // ¼
    {"frac34", "\xc2\xbe"},     // ¾
    {NULL, NULL}
};

// Decode an HTML entity, return number of bytes written to out
static size_t decode_entity(const char *entity, size_t len, char *out) {
    // Numeric entities: &#123; or &#x1F;
    if (len > 1 && entity[0] == '#') {
        unsigned long cp;
        char *end;
        if (entity[1] == 'x' || entity[1] == 'X') {
            cp = strtoul(entity + 2, &end, 16);
        } else {
            cp = strtoul(entity + 1, &end, 10);
        }
        if (end == entity + len && cp > 0 && cp < 0x110000) {
            return utf8_encode(cp, out);
        }
    }

    // Named entities
    for (int i = 0; named_entities[i].name; i++) {
        if (strlen(named_entities[i].name) == len &&
            memcmp(named_entities[i].name, entity, len) == 0) {
            size_t vlen = strlen(named_entities[i].value);
            memcpy(out, named_entities[i].value, vlen);
            return vlen;
        }
    }

    // Unknown entity
    return 0;
}

char *zim_html_to_text(const char *html, size_t html_size, size_t *text_size) {
    if (!html || html_size == 0) {
        *text_size = 0;
        return strdup("");
    }

    // Allocate output buffer (can't be larger than input + entities)
    char *out = malloc(html_size * 2 + 1);
    if (!out) return NULL;

    size_t out_len = 0;
    int state = STATE_TEXT;
    char tag_name[32];
    size_t tag_name_len = 0;
    char entity[32];
    size_t entity_len = 0;
    int last_was_space = 1;
    int last_was_newline = 1;
    int is_closing_tag = 0;
    const char *strip_end_tag = NULL;  // If set, skip until this end tag

    for (size_t i = 0; i < html_size; i++) {
        unsigned char c = html[i];

        // If we're skipping content (script/style)
        if (strip_end_tag) {
            if (c == '<') {
                // Check for end tag
                size_t end_len = strlen(strip_end_tag);
                if (i + end_len + 2 < html_size &&
                    html[i + 1] == '/' &&
                    strncasecmp(html + i + 2, strip_end_tag, end_len) == 0) {
                    // Skip to after the end tag
                    i += 2 + end_len;
                    while (i < html_size && html[i] != '>') i++;
                    strip_end_tag = NULL;
                    state = STATE_TEXT;
                }
            }
            continue;
        }

        switch (state) {
        case STATE_TEXT:
            if (c == '<') {
                state = STATE_TAG_START;
                tag_name_len = 0;
                is_closing_tag = 0;
            } else if (c == '&') {
                state = STATE_ENTITY;
                entity_len = 0;
            } else {
                // Normalize whitespace
                if (c == '\n' || c == '\r') {
                    if (!last_was_newline && !last_was_space) {
                        out[out_len++] = ' ';
                        last_was_space = 1;
                    }
                } else if (isspace(c)) {
                    if (!last_was_space) {
                        out[out_len++] = ' ';
                        last_was_space = 1;
                    }
                } else {
                    out[out_len++] = c;
                    last_was_space = 0;
                    last_was_newline = 0;
                }
            }
            break;

        case STATE_TAG_START:
            if (c == '!') {
                state = STATE_COMMENT_1;
            } else if (c == '/') {
                is_closing_tag = 1;
                state = STATE_TAG_NAME;
            } else if (isalpha(c)) {
                tag_name[0] = tolower(c);
                tag_name_len = 1;
                state = STATE_TAG_NAME;
            } else if (c == '>') {
                state = STATE_TEXT;
            } else {
                // Not a valid tag, output the '<'
                out[out_len++] = '<';
                last_was_space = 0;
                last_was_newline = 0;
                i--;  // Reprocess this character
                state = STATE_TEXT;
            }
            break;

        case STATE_TAG_NAME:
            if (isalnum(c) || c == '-' || c == ':') {
                if (tag_name_len < sizeof(tag_name) - 1) {
                    tag_name[tag_name_len++] = tolower(c);
                }
            } else if (c == '>' || isspace(c)) {
                tag_name[tag_name_len] = '\0';

                // Check if this is a block tag
                if (str_in_list(tag_name, tag_name_len, block_tags)) {
                    // Add newline for block elements
                    if (!last_was_newline && out_len > 0) {
                        // Trim trailing space before newline
                        if (last_was_space && out_len > 0) {
                            out_len--;
                        }
                        out[out_len++] = '\n';
                        last_was_newline = 1;
                        last_was_space = 1;
                    }
                }

                // Check if this is a strip tag (script/style)
                if (!is_closing_tag && str_in_list(tag_name, tag_name_len, strip_tags)) {
                    // Set up to skip content until end tag
                    strip_end_tag = tag_name;
                    // Continue to find '>' then skip
                    if (c == '>') {
                        state = STATE_TEXT;
                    } else {
                        state = STATE_TAG_BODY;
                    }
                    break;
                }

                if (c == '>') {
                    state = STATE_TEXT;
                } else {
                    state = STATE_TAG_BODY;
                }
            } else {
                state = STATE_TAG_BODY;
            }
            break;

        case STATE_TAG_BODY:
            if (c == '>') {
                state = STATE_TEXT;
                if (strip_end_tag) {
                    // We're entering a strip section
                }
            } else if (c == '"') {
                state = STATE_TAG_DQUOTE;
            } else if (c == '\'') {
                state = STATE_TAG_QUOTE;
            }
            break;

        case STATE_TAG_QUOTE:
            if (c == '\'') state = STATE_TAG_BODY;
            break;

        case STATE_TAG_DQUOTE:
            if (c == '"') state = STATE_TAG_BODY;
            break;

        case STATE_COMMENT_1:
            if (c == '-') {
                if (i + 1 < html_size && html[i + 1] == '-') {
                    i++;
                    state = STATE_COMMENT;
                } else {
                    state = STATE_TAG_BODY;
                }
            } else {
                state = STATE_TAG_BODY;
            }
            break;

        case STATE_COMMENT:
            if (c == '-') state = STATE_COMMENT_END_1;
            break;

        case STATE_COMMENT_END_1:
            if (c == '-') state = STATE_COMMENT_END_2;
            else state = STATE_COMMENT;
            break;

        case STATE_COMMENT_END_2:
            if (c == '>') state = STATE_TEXT;
            else if (c != '-') state = STATE_COMMENT;
            break;

        case STATE_ENTITY:
            if (c == ';') {
                char decoded[8];
                size_t decoded_len = decode_entity(entity, entity_len, decoded);
                if (decoded_len > 0) {
                    memcpy(out + out_len, decoded, decoded_len);
                    out_len += decoded_len;
                    last_was_space = (decoded[0] == ' ');
                    last_was_newline = 0;
                } else {
                    // Unknown entity - output as-is
                    out[out_len++] = '&';
                    memcpy(out + out_len, entity, entity_len);
                    out_len += entity_len;
                    out[out_len++] = ';';
                    last_was_space = 0;
                    last_was_newline = 0;
                }
                state = STATE_TEXT;
            } else if (entity_len < sizeof(entity) - 1 &&
                       (isalnum(c) || c == '#')) {
                entity[entity_len++] = c;
            } else {
                // Invalid entity - output as-is
                out[out_len++] = '&';
                memcpy(out + out_len, entity, entity_len);
                out_len += entity_len;
                last_was_space = 0;
                last_was_newline = 0;
                i--;  // Reprocess this character
                state = STATE_TEXT;
            }
            break;
        }
    }

    // Trim trailing whitespace
    while (out_len > 0 && isspace((unsigned char)out[out_len - 1])) {
        out_len--;
    }

    out[out_len] = '\0';

    // Shrink buffer to actual size
    char *result = realloc(out, out_len + 1);
    if (!result) result = out;

    *text_size = out_len;
    return result;
}

// Get content as text (automatically converts HTML)
char *zim_get_content_text(zim_archive *archive, const zim_entry *entry, size_t *size) {
    // Get raw content first
    size_t raw_size;
    void *raw = zim_get_content(archive, entry, &raw_size);
    if (!raw) {
        return NULL;
    }

    // Check MIME type
    const char *mime = zim_get_mimetype(archive, entry);
    if (mime && (strstr(mime, "text/html") || strstr(mime, "application/xhtml"))) {
        // Convert HTML to text
        char *text = zim_html_to_text(raw, raw_size, size);
        zim_free(raw);
        return text;
    }

    // Not HTML - return as-is
    *size = raw_size;
    return raw;
}
