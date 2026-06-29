#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘

PKGS += LLAMAFILE_ZIM

LLAMAFILE_ZIM_FILES := $(wildcard llamafile/zim/*)
LLAMAFILE_ZIM_HDRS = $(filter %.h,$(LLAMAFILE_ZIM_FILES))
LLAMAFILE_ZIM_SRCS_C = $(filter %.c,$(LLAMAFILE_ZIM_FILES))

# zstddeclib.c is #include'd by zim_cluster.c, so declare it as INCS
LLAMAFILE_ZIM_INCS = llamafile/zim/zstddeclib.c

# Exclude zstddeclib.c and test files from SRCS
LLAMAFILE_ZIM_SRCS = $(filter-out %zstddeclib.c %_test.c,$(LLAMAFILE_ZIM_SRCS_C))

LLAMAFILE_ZIM_OBJS = $(LLAMAFILE_ZIM_SRCS:%.c=o/$(MODE)/%.o)

o/$(MODE)/llamafile/zim/zim.a: $(LLAMAFILE_ZIM_OBJS)

$(LLAMAFILE_ZIM_OBJS): llamafile/zim/BUILD.mk

# Test for HTML-to-text conversion
o/$(MODE)/llamafile/zim/zim_html_test:					\
		o/$(MODE)/llamafile/zim/zim_html_test.o			\
		o/$(MODE)/llamafile/zim/zim.a

.PHONY: llamafile-zim
llamafile-zim: o/$(MODE)/llamafile/zim/zim.a

.PHONY: llamafile-zim-test
llamafile-zim-test: o/$(MODE)/llamafile/zim/zim_html_test
