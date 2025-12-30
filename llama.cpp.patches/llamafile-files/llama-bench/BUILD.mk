#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘
#
# Llama-bench BUILD.mk - NATIVE STRUCTURE VERSION
#
# The llama-bench code lives in llama.cpp/tools/llama-bench/ in the native structure
#

PKGS += LLAMA_CPP_LLAMA-BENCH

# Main llama-bench sources from upstream llama.cpp (in tools/llama-bench/)
LLAMA_CPP_LLAMA_BENCH_UPSTREAM_FILES := $(wildcard llama.cpp/tools/llama-bench/*.cpp)
LLAMA_CPP_LLAMA_BENCH_UPSTREAM_SRCS = $(filter %.cpp,$(LLAMA_CPP_LLAMA_BENCH_UPSTREAM_FILES))
LLAMA_CPP_LLAMA_BENCH_UPSTREAM_OBJS = $(patsubst llama.cpp/tools/%,o/$(MODE)/llama.cpp/%,$(LLAMA_CPP_LLAMA_BENCH_UPSTREAM_SRCS:%.cpp=%.o))

# All llama-bench objects
LLAMA_CPP_LLAMA_BENCH_OBJS = $(LLAMA_CPP_LLAMA_BENCH_UPSTREAM_OBJS)

o/$(MODE)/llama.cpp/llama-bench/llama-bench:				\
		o/$(MODE)/llama.cpp/llama-bench/llama-bench.o		\
		o/$(MODE)/llama.cpp/llama.cpp.a

$(LLAMA_CPP_LLAMA_BENCH_OBJS): llama.cpp/llama-bench/BUILD.mk

# Include paths for llama-bench
LLAMA_CPP_LLAMA_BENCH_INCLUDES = -Illama.cpp/tools/llama-bench

$(LLAMA_CPP_LLAMA_BENCH_OBJS): private CCFLAGS += $(LLAMA_CPP_LLAMA_BENCH_INCLUDES)

.PHONY: o/$(MODE)/llama.cpp/llama-bench
o/$(MODE)/llama.cpp/llama-bench:					\
		o/$(MODE)/llama.cpp/llama-bench/llama-bench
