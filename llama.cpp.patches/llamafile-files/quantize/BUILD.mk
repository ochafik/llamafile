#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘
#
# Quantize BUILD.mk - NATIVE STRUCTURE VERSION
#
# The quantize code lives in llama.cpp/tools/quantize/ in the native structure
#

PKGS += LLAMA_CPP_QUANTIZE

# Main quantize sources from upstream llama.cpp (in tools/quantize/)
LLAMA_CPP_QUANTIZE_UPSTREAM_FILES := $(wildcard llama.cpp/tools/quantize/*.cpp)
LLAMA_CPP_QUANTIZE_UPSTREAM_SRCS = $(filter %.cpp,$(LLAMA_CPP_QUANTIZE_UPSTREAM_FILES))
LLAMA_CPP_QUANTIZE_UPSTREAM_OBJS = $(patsubst llama.cpp/tools/%,o/$(MODE)/llama.cpp/%,$(LLAMA_CPP_QUANTIZE_UPSTREAM_SRCS:%.cpp=%.o))

# All quantize objects
LLAMA_CPP_QUANTIZE_OBJS = $(LLAMA_CPP_QUANTIZE_UPSTREAM_OBJS)

o/$(MODE)/llama.cpp/quantize/quantize:					\
		o/$(MODE)/llama.cpp/quantize/quantize.o			\
		o/$(MODE)/llama.cpp/quantize/quantize.1.asc.zip.o	\
		o/$(MODE)/llama.cpp/llama.cpp.a

# Include paths for quantize
LLAMA_CPP_QUANTIZE_INCLUDES = -Illama.cpp/tools/quantize

$(LLAMA_CPP_QUANTIZE_OBJS): private CCFLAGS += $(LLAMA_CPP_QUANTIZE_INCLUDES)

.PHONY: o/$(MODE)/llama.cpp/quantize
o/$(MODE)/llama.cpp/quantize:						\
		o/$(MODE)/llama.cpp/quantize/quantize
