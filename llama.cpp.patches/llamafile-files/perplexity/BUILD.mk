#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘
#
# Perplexity BUILD.mk - NATIVE STRUCTURE VERSION
#
# The perplexity code lives in llama.cpp/tools/perplexity/ in the native structure
#

PKGS += LLAMA_CPP_PERPLEXITY

# Main perplexity sources from upstream llama.cpp (in tools/perplexity/)
LLAMA_CPP_PERPLEXITY_UPSTREAM_FILES := $(wildcard llama.cpp/tools/perplexity/*.cpp)
LLAMA_CPP_PERPLEXITY_UPSTREAM_SRCS = $(filter %.cpp,$(LLAMA_CPP_PERPLEXITY_UPSTREAM_FILES))
LLAMA_CPP_PERPLEXITY_UPSTREAM_OBJS = $(patsubst llama.cpp/tools/%,o/$(MODE)/llama.cpp/%,$(LLAMA_CPP_PERPLEXITY_UPSTREAM_SRCS:%.cpp=%.o))

# All perplexity objects
LLAMA_CPP_PERPLEXITY_OBJS = $(LLAMA_CPP_PERPLEXITY_UPSTREAM_OBJS)

.PHONY: o/$(MODE)/llama.cpp/perplexity
o/$(MODE)/llama.cpp/perplexity:						\
		o/$(MODE)/llama.cpp/perplexity/perplexity

o/$(MODE)/llama.cpp/perplexity/perplexity:				\
		o/$(MODE)/llama.cpp/perplexity/perplexity.o		\
		o/$(MODE)/llama.cpp/perplexity/perplexity.1.asc.zip.o	\
		o/$(MODE)/llama.cpp/llama.cpp.a

# Include paths for perplexity
LLAMA_CPP_PERPLEXITY_INCLUDES = -Illama.cpp/tools/perplexity

$(LLAMA_CPP_PERPLEXITY_OBJS): private CCFLAGS += $(LLAMA_CPP_PERPLEXITY_INCLUDES)
