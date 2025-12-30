#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘
#
# MTMD (Multimodal Tools) BUILD.mk - NATIVE STRUCTURE VERSION
#
# The mtmd code lives in llama.cpp/tools/mtmd/ in the native structure
# Llamafile-specific files (like mtmd-quantize) are in llama.cpp/mtmd/
#

PKGS += LLAMA_CPP_MTMD

# Main mtmd sources from upstream llama.cpp (in tools/mtmd/)
LLAMA_CPP_MTMD_UPSTREAM_FILES := $(wildcard llama.cpp/tools/mtmd/*.cpp)
LLAMA_CPP_MTMD_UPSTREAM_SRCS = $(filter %.cpp,$(LLAMA_CPP_MTMD_UPSTREAM_FILES))
LLAMA_CPP_MTMD_UPSTREAM_OBJS = $(patsubst llama.cpp/tools/%,o/$(MODE)/llama.cpp/%,$(LLAMA_CPP_MTMD_UPSTREAM_SRCS:%.cpp=%.o))

# Llamafile-specific mtmd files (in mtmd/ after copying from llamafile-files)
LLAMA_CPP_MTMD_LLAMAFILE_FILES := $(wildcard llama.cpp/mtmd/*)
LLAMA_CPP_MTMD_LLAMAFILE_SRCS = $(filter %.cpp,$(LLAMA_CPP_MTMD_LLAMAFILE_FILES))
LLAMA_CPP_MTMD_LLAMAFILE_OBJS = $(LLAMA_CPP_MTMD_LLAMAFILE_SRCS:%.cpp=o/$(MODE)/llama.cpp/mtmd/%.o)

# All mtmd objects
LLAMA_CPP_MTMD_OBJS = $(LLAMA_CPP_MTMD_UPSTREAM_OBJS) $(LLAMA_CPP_MTMD_LLAMAFILE_OBJS)

o/$(MODE)/llama.cpp/mtmd.a: $(LLAMA_CPP_MTMD_OBJS)

# Include paths for mtmd
LLAMA_CPP_MTMD_INCLUDES = -Illama.cpp/tools/mtmd

$(LLAMA_CPP_MTMD_OBJS): private CCFLAGS += $(LLAMA_CPP_MTMD_INCLUDES)

# mtmd-quantize tool (llamafile-specific)
o/$(MODE)/llama.cpp/mtmd/mtmd-quantize:			\
		o/$(MODE)/llama.cpp/mtmd/mtmd-quantize.o	\
		o/$(MODE)/llama.cpp/mtmd/mtmd-quantize.1.asc.zip.o\
		o/$(MODE)/llama.cpp/mtmd.a			\
		o/$(MODE)/llama.cpp/llama.cpp.a		\
		o/$(MODE)/third_party/stb/stb.a

.PHONY: o/$(MODE)/llama.cpp/mtmd
o/$(MODE)/llama.cpp/mtmd:				\
		o/$(MODE)/llama.cpp/mtmd.a			\
		o/$(MODE)/llama.cpp/mtmd/mtmd-quantize
