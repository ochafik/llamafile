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
#include <stdio.h>
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

// =================================================================
// HTML -> Markdown
// =================================================================
//
// A second, structure-preserving converter (the one above flattens to plain
// text). It walks the article HTML and emits clean Markdown: h1-h6 -> #..######,
// <ul>/<ol>/<li> -> "- " / "1. ", <b>/<strong> -> **, <i>/<em> -> *,
// <a href> -> [text](url) with internal ZIM links rewritten to /wiki/<path>
// (so they are clickable both in the rendered chat and the /wiki browser),
// <p> -> blank line, tables -> a minimal pipe form. <head>/<script>/<style>
// chrome is dropped. The article's <h1 class="firstHeading"> becomes the
// top-level "# Title". Not a perfect HTML renderer — readable + structured.

// Rewrite a link target: internal ZIM links -> /wiki/<path>; external
// (http/https/protocol-relative/mailto/...), in-page anchors (#...) and
// already-absolute (/...) targets are left untouched. Shared with the
// in-process /wiki article browser (wiki_route.cpp). `out` must hold outsz bytes.
void zim_rewrite_link(const char *href, char *out, size_t outsz) {
    if (!href || !out || outsz == 0) { if (out && outsz) out[0] = 0; return; }
    // Leave anchors, already-absolute paths, and known external schemes alone.
    if (href[0] == '#' || href[0] == '/' ||
        strncmp(href, "//", 2) == 0 ||
        strncasecmp(href, "http://", 7) == 0 ||
        strncasecmp(href, "https://", 8) == 0 ||
        strncasecmp(href, "mailto:", 7) == 0 ||
        strncasecmp(href, "tel:", 4) == 0 ||
        strncasecmp(href, "ftp:", 4) == 0 ||
        strncasecmp(href, "data:", 5) == 0 ||
        strncasecmp(href, "javascript:", 11) == 0) {
        snprintf(out, outsz, "%s", href);
        return;
    }
    // Internal, relative ZIM link. Strip any leading ./ or ../ segments and
    // mount it under /wiki/ (the article paths in this ZIM are flat, e.g.
    // "France", "_mw_/style.css").
    const char *p = href;
    for (;;) {
        if (p[0] == '.' && p[1] == '/') { p += 2; continue; }
        if (p[0] == '.' && p[1] == '.' && p[2] == '/') { p += 3; continue; }
        break;
    }
    snprintf(out, outsz, "/wiki/%s", p);
}

// ---- growable output buffer ----
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} md_out;

static int md_reserve(md_out *o, size_t extra) {
    if (o->buf && o->len + extra + 1 <= o->cap) return 1;
    size_t ncap = o->cap ? o->cap : 4096;
    while (ncap < o->len + extra + 1) ncap *= 2;
    char *nb = realloc(o->buf, ncap);
    if (!nb) return 0;
    o->buf = nb;
    o->cap = ncap;
    return 1;
}
static void md_putc(md_out *o, char c) {
    if (!md_reserve(o, 1)) return;
    o->buf[o->len++] = c;
}
static void md_puts(md_out *o, const char *s) {
    size_t n = strlen(s);
    if (!md_reserve(o, n)) return;
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}
static int md_at_line_start(md_out *o) {
    return o->len == 0 || o->buf[o->len - 1] == '\n';
}
static void md_trim_trailing_spaces(md_out *o) {
    while (o->len > 0 && (o->buf[o->len - 1] == ' ' || o->buf[o->len - 1] == '\t'))
        o->len--;
}
static void md_ensure_nl(md_out *o) {
    md_trim_trailing_spaces(o);
    if (!md_at_line_start(o)) md_putc(o, '\n');
}
static void md_ensure_blank(md_out *o) {
    md_ensure_nl(o);
    if (o->len >= 1 && (o->len < 2 || o->buf[o->len - 2] != '\n')) md_putc(o, '\n');
}

// Append a run of text, collapsing whitespace and suppressing spaces at the
// start of a line. last_was_space tracks the cross-call whitespace state.
static void md_text(md_out *o, const char *s, size_t n, int *last_was_space) {
    for (size_t k = 0; k < n; k++) {
        unsigned char ch = (unsigned char) s[k];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v') {
            if (!*last_was_space) {
                if (!md_at_line_start(o)) md_putc(o, ' ');
                *last_was_space = 1;
            }
        } else {
            md_putc(o, (char) ch);
            *last_was_space = 0;
        }
    }
}

// Tags whose entire content is dropped from the Markdown.
static const char *md_strip_tags[] = {
    "script", "style", "head", "noscript", "template", "svg", NULL
};

// Extract an attribute value (e.g. href) from a tag's attribute region.
static void md_extract_attr(const char *attrs, size_t len, const char *key,
                            char *out, size_t outsz) {
    out[0] = 0;
    size_t klen = strlen(key);
    for (size_t i = 0; i + klen <= len; i++) {
        if (strncasecmp(attrs + i, key, klen) != 0) continue;
        // word boundary before the key (avoid matching e.g. "xhref")
        if (i > 0) {
            char pc = attrs[i - 1];
            if (isalnum((unsigned char) pc) || pc == '-' || pc == '_') continue;
        }
        size_t j = i + klen;
        while (j < len && isspace((unsigned char) attrs[j])) j++;
        if (j >= len || attrs[j] != '=') continue;
        j++;
        while (j < len && isspace((unsigned char) attrs[j])) j++;
        char quote = 0;
        if (j < len && (attrs[j] == '"' || attrs[j] == '\'')) { quote = attrs[j]; j++; }
        size_t o = 0;
        while (j < len && o < outsz - 1) {
            char c = attrs[j];
            if (quote) { if (c == quote) break; }
            else { if (isspace((unsigned char) c) || c == '>') break; }
            out[o++] = c;
            j++;
        }
        out[o] = 0;
        return;
    }
}

#define MD_MAX_LIST 16

static void md_handle_tag(md_out *o, const char *name, int closing,
                          const char *attrs, size_t attrs_len,
                          char *list_type, int *list_count, int *list_depth,
                          int *in_link, char *link_href, int *last_was_space) {
    // headings h1..h6
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0) {
        int level = name[1] - '0';
        md_ensure_blank(o);
        if (!closing) {
            for (int k = 0; k < level; k++) md_putc(o, '#');
            md_putc(o, ' ');
        }
        *last_was_space = 1;
        return;
    }
    if (!strcmp(name, "p") || !strcmp(name, "div") || !strcmp(name, "section") ||
        !strcmp(name, "article") || !strcmp(name, "dl") || !strcmp(name, "dd") ||
        !strcmp(name, "dt") || !strcmp(name, "figure") || !strcmp(name, "figcaption")) {
        md_ensure_blank(o);
        *last_was_space = 1;
        return;
    }
    if (!strcmp(name, "br")) { md_ensure_nl(o); *last_was_space = 1; return; }
    if (!strcmp(name, "hr")) {
        md_ensure_blank(o); md_puts(o, "---"); md_ensure_blank(o); *last_was_space = 1;
        return;
    }
    if (!strcmp(name, "blockquote")) {
        md_ensure_blank(o);
        if (!closing) { md_puts(o, "> "); *last_was_space = 1; }
        return;
    }
    if (!strcmp(name, "b") || !strcmp(name, "strong")) { md_puts(o, "**"); return; }
    if (!strcmp(name, "i") || !strcmp(name, "em")) { md_puts(o, "*"); return; }
    if (!strcmp(name, "code") || !strcmp(name, "tt")) { md_puts(o, "`"); return; }
    if (!strcmp(name, "pre")) { md_ensure_blank(o); *last_was_space = 1; return; }
    if (!strcmp(name, "ul") || !strcmp(name, "ol")) {
        if (!closing) {
            md_ensure_nl(o);
            if (*list_depth < MD_MAX_LIST) {
                list_type[*list_depth] = (name[0] == 'o') ? 'o' : 'u';
                list_count[*list_depth] = 0;
                (*list_depth)++;
            }
        } else {
            if (*list_depth > 0) (*list_depth)--;
            md_ensure_blank(o);
        }
        *last_was_space = 1;
        return;
    }
    if (!strcmp(name, "li")) {
        if (!closing) {
            md_ensure_nl(o);
            int d = *list_depth;
            int indent = d > 1 ? d - 1 : 0;
            for (int k = 0; k < indent; k++) md_puts(o, "  ");
            if (d > 0 && list_type[d - 1] == 'o') {
                char num[16];
                snprintf(num, sizeof num, "%d. ", ++list_count[d - 1]);
                md_puts(o, num);
            } else {
                md_puts(o, "- ");
            }
            *last_was_space = 1;
        }
        return;
    }
    if (!strcmp(name, "table")) { md_ensure_blank(o); *last_was_space = 1; return; }
    if (!strcmp(name, "tr")) {
        md_ensure_nl(o);
        if (!closing) { md_puts(o, "| "); *last_was_space = 1; }
        return;
    }
    if (!strcmp(name, "td") || !strcmp(name, "th")) {
        if (closing) { md_puts(o, " | "); *last_was_space = 1; }
        return;
    }
    if (!strcmp(name, "a")) {
        if (!closing) {
            char href[1024];
            md_extract_attr(attrs, attrs_len, "href", href, sizeof href);
            if (href[0]) {
                char rew[1100];
                zim_rewrite_link(href, rew, sizeof rew);
                md_putc(o, '[');
                strncpy(link_href, rew, 1099);
                link_href[1099] = 0;
                *in_link = 1;
            }
        } else if (*in_link) {
            md_trim_trailing_spaces(o);
            md_puts(o, "](");
            md_puts(o, link_href);
            md_putc(o, ')');
            *in_link = 0;
        }
        return;
    }
    // <img>, <span>, <meta>, <link>, ... -> no markup
}

char *zim_html_to_markdown(const char *html, size_t html_size, size_t *md_size) {
    md_out o = { 0 };
    if (!html || html_size == 0) { *md_size = 0; return strdup(""); }
    md_reserve(&o, html_size); // size hint

    char list_type[MD_MAX_LIST];
    int  list_count[MD_MAX_LIST];
    int  list_depth = 0;
    int  in_link = 0;
    char link_href[1100];
    char skip_name[32];
    int  skip_depth = 0;
    int  last_was_space = 1;

    size_t i = 0;
    while (i < html_size) {
        unsigned char c = (unsigned char) html[i];

        // Inside a stripped element (<script>/<style>/<head>/...): drop all
        // text and entities; only tags are inspected (to find the matching close).
        if (skip_depth > 0 && c != '<') { i++; continue; }

        if (c == '<') {
            size_t j = i + 1;
            // comment
            if (j + 2 < html_size && html[j] == '!' && html[j + 1] == '-' && html[j + 2] == '-') {
                j += 3;
                while (j + 2 < html_size && !(html[j] == '-' && html[j + 1] == '-' && html[j + 2] == '>'))
                    j++;
                i = (j + 3 <= html_size) ? j + 3 : html_size;
                continue;
            }
            // declaration / doctype
            if (j < html_size && html[j] == '!') {
                while (j < html_size && html[j] != '>') j++;
                i = (j < html_size) ? j + 1 : html_size;
                continue;
            }
            int closing = 0;
            if (j < html_size && html[j] == '/') { closing = 1; j++; }
            char name[32];
            size_t nl = 0;
            while (j < html_size && (isalnum((unsigned char) html[j]) || html[j] == '-')) {
                if (nl < sizeof(name) - 1) name[nl++] = tolower((unsigned char) html[j]);
                j++;
            }
            name[nl] = 0;
            // attribute region (up to the closing '>', respecting quotes)
            size_t attr_start = j;
            char q = 0;
            while (j < html_size) {
                char cc = html[j];
                if (q) { if (cc == q) q = 0; }
                else if (cc == '"' || cc == '\'') q = cc;
                else if (cc == '>') break;
                j++;
            }
            size_t attr_end = j;
            size_t next = (j < html_size) ? j + 1 : html_size;

            if (skip_depth > 0) {
                if (nl && strcmp(name, skip_name) == 0) {
                    if (!closing) skip_depth++;
                    else if (--skip_depth == 0) { /* leaving stripped element */ }
                }
                i = next;
                continue;
            }
            if (!closing && nl && str_in_list(name, nl, md_strip_tags)) {
                strncpy(skip_name, name, sizeof(skip_name) - 1);
                skip_name[sizeof(skip_name) - 1] = 0;
                skip_depth = 1;
                i = next;
                continue;
            }
            if (nl) {
                md_handle_tag(&o, name, closing, html + attr_start, attr_end - attr_start,
                              list_type, list_count, &list_depth,
                              &in_link, link_href, &last_was_space);
            }
            i = next;
            continue;
        }

        if (c == '&') {
            size_t j = i + 1;
            char ent[32];
            size_t el = 0;
            while (j < html_size && html[j] != ';' &&
                   (isalnum((unsigned char) html[j]) || html[j] == '#') && el < sizeof(ent) - 1) {
                ent[el++] = html[j];
                j++;
            }
            if (j < html_size && html[j] == ';') {
                char dec[8];
                size_t dl = decode_entity(ent, el, dec);
                if (dl > 0) {
                    md_text(&o, dec, dl, &last_was_space);
                    i = j + 1;
                    continue;
                }
            }
            md_text(&o, "&", 1, &last_was_space);
            i++;
            continue;
        }

        char ch = (char) c;
        md_text(&o, &ch, 1, &last_was_space);
        i++;
    }

    if (in_link) {
        md_puts(&o, "](");
        md_puts(&o, link_href);
        md_putc(&o, ')');
    }
    md_trim_trailing_spaces(&o);
    while (o.len > 0 && o.buf[o.len - 1] == '\n') o.len--;
    if (!md_reserve(&o, 1)) { if (o.buf) o.buf[o.len] = 0; }
    else o.buf[o.len] = 0;
    *md_size = o.len;
    return o.buf ? o.buf : strdup("");
}

// Get content as Markdown (automatically converts HTML; non-HTML returned raw).
char *zim_get_content_markdown(zim_archive *archive, const zim_entry *entry, size_t *size) {
    size_t raw_size;
    void *raw = zim_get_content(archive, entry, &raw_size);
    if (!raw) return NULL;
    const char *mime = zim_get_mimetype(archive, entry);
    if (mime && (strstr(mime, "text/html") || strstr(mime, "application/xhtml"))) {
        char *md = zim_html_to_markdown(raw, raw_size, size);
        zim_free(raw);
        return md;
    }
    *size = raw_size;
    return (char *) raw;
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
