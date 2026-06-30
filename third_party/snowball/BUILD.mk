#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘

# Snowball libstemmer_c (vendored from https://snowballstem.org/dist/
# libstemmer_c.tgz). Plain generated C, no deps — cosmocc-clean. Provides the
# per-language Snowball stemmers used by the in-process ZIM Xapian reader so a
# query term also matches its morphological variants (STEM_SOME). We vendor only
# the UTF-8 modules (libstemmer_utf8.c -> modules_utf8.h -> src_c/stem_UTF_8_*).
# The generated sources use relative quote-includes ("../runtime/header.h" etc.),
# so the vendored directory layout is kept intact and needs no -I flags.

PKGS += THIRD_PARTY_SNOWBALL

THIRD_PARTY_SNOWBALL_SRCS :=					\
	third_party/snowball/runtime/api.c			\
	third_party/snowball/runtime/utilities.c		\
	third_party/snowball/libstemmer/libstemmer_utf8.c	\
	$(sort $(wildcard third_party/snowball/src_c/stem_UTF_8_*.c))

THIRD_PARTY_SNOWBALL_HDRS := $(wildcard third_party/snowball/*/*.h)

THIRD_PARTY_SNOWBALL_OBJS = $(THIRD_PARTY_SNOWBALL_SRCS:%.c=o/$(MODE)/%.o)

# The generated stemmers are warning-noisy plain C; build them with the C
# compiler and silence the harmless unused-variable churn so -Werror trees stay
# green. -mgcc (matching the vendored sqlite build) avoids cosmo's stricter
# clang front-end choking on the generated code.
o/$(MODE)/third_party/snowball/%.o: third_party/snowball/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -mgcc -w -c -o $@ $<

$(THIRD_PARTY_SNOWBALL_OBJS): third_party/snowball/BUILD.mk

o/$(MODE)/third_party/snowball/snowball.a: $(THIRD_PARTY_SNOWBALL_OBJS)

.PHONY: o/$(MODE)/third_party/snowball
o/$(MODE)/third_party/snowball: o/$(MODE)/third_party/snowball/snowball.a
