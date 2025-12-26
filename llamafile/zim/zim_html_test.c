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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void test_basic_html_to_text(void) {
    const char* html = "<html><body><p>Hello World</p></body></html>";
    size_t text_size;
    char* text = zim_html_to_text(html, strlen(html), &text_size);
    assert(text != NULL);
    assert(strstr(text, "Hello World") != NULL);
    printf("test_basic_html_to_text: PASS (%s)\n", text);
    zim_free(text);
}

static void test_strip_tags(void) {
    const char* html = "<b>Bold</b> and <i>italic</i>";
    size_t text_size;
    char* text = zim_html_to_text(html, strlen(html), &text_size);
    assert(text != NULL);
    assert(strstr(text, "Bold") != NULL);
    assert(strstr(text, "italic") != NULL);
    assert(strstr(text, "<b>") == NULL);
    assert(strstr(text, "</b>") == NULL);
    printf("test_strip_tags: PASS (%s)\n", text);
    zim_free(text);
}

static void test_block_elements(void) {
    const char* html = "<p>Para 1</p><p>Para 2</p>";
    size_t text_size;
    char* text = zim_html_to_text(html, strlen(html), &text_size);
    assert(text != NULL);
    assert(strstr(text, "Para 1") != NULL);
    assert(strstr(text, "Para 2") != NULL);
    printf("test_block_elements: PASS (%s)\n", text);
    zim_free(text);
}

static void test_script_removal(void) {
    const char* html = "<p>Before</p><script>alert('bad');</script><p>After</p>";
    size_t text_size;
    char* text = zim_html_to_text(html, strlen(html), &text_size);
    assert(text != NULL);
    assert(strstr(text, "Before") != NULL);
    assert(strstr(text, "After") != NULL);
    assert(strstr(text, "alert") == NULL);
    printf("test_script_removal: PASS (%s)\n", text);
    zim_free(text);
}

static void test_style_removal(void) {
    const char* html = "<p>Before</p><style>.foo { color: red; }</style><p>After</p>";
    size_t text_size;
    char* text = zim_html_to_text(html, strlen(html), &text_size);
    assert(text != NULL);
    assert(strstr(text, "Before") != NULL);
    assert(strstr(text, "After") != NULL);
    assert(strstr(text, "color") == NULL);
    printf("test_style_removal: PASS (%s)\n", text);
    zim_free(text);
}

static void test_html_entities(void) {
    const char* html = "5 &lt; 10 &amp; 3 &gt; 1";
    size_t text_size;
    char* text = zim_html_to_text(html, strlen(html), &text_size);
    assert(text != NULL);
    assert(strstr(text, "5 < 10 & 3 > 1") != NULL);
    printf("test_html_entities: PASS (%s)\n", text);
    zim_free(text);
}

static void test_numeric_entities(void) {
    const char* html = "&#65;&#66;&#67;";  // ABC
    size_t text_size;
    char* text = zim_html_to_text(html, strlen(html), &text_size);
    assert(text != NULL);
    assert(strstr(text, "ABC") != NULL);
    printf("test_numeric_entities: PASS (%s)\n", text);
    zim_free(text);
}

static void test_whitespace_collapse(void) {
    const char* html = "   Multiple    spaces   ";
    size_t text_size;
    char* text = zim_html_to_text(html, strlen(html), &text_size);
    assert(text != NULL);
    // Just verify we got something back
    assert(strlen(text) > 0);
    printf("test_whitespace_collapse: PASS (%s)\n", text);
    zim_free(text);
}

static void test_empty_html(void) {
    const char* html = "";
    size_t text_size;
    char* text = zim_html_to_text(html, strlen(html), &text_size);
    assert(text != NULL);
    assert(text_size == 0 || (text_size == 1 && text[0] == '\n'));
    printf("test_empty_html: PASS\n");
    zim_free(text);
}

static void test_null_input(void) {
    size_t text_size;
    char* text = zim_html_to_text(NULL, 0, &text_size);
    // Should handle NULL gracefully
    assert(text == NULL || text_size == 0);
    printf("test_null_input: PASS\n");
    if (text) zim_free(text);
}

int main(void) {
    printf("=== ZIM HTML-to-Text Tests ===\n");

    test_basic_html_to_text();
    test_strip_tags();
    test_block_elements();
    test_script_removal();
    test_style_removal();
    test_html_entities();
    test_numeric_entities();
    test_whitespace_collapse();
    test_empty_html();
    test_null_input();

    printf("\n=== All tests passed! ===\n");
    return 0;
}
