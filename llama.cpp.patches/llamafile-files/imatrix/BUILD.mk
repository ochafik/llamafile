#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘
#
# Imatrix BUILD.mk - NATIVE STRUCTURE VERSION
#
# The imatrix code lives in llama.cpp/tools/imatrix/ in the native structure
#

PKGS += LLAMA_CPP_IMATRIX

# Main imatrix sources from upstream llama.cpp (in tools/imatrix/)
LLAMA_CPP_IMATRIX_UPSTREAM_FILES := $(wildcard llama.cpp/tools/imatrix/*.cpp)
LLAMA_CPP_IMATRIX_UPSTREAM_SRCS = $(filter %.cpp,$(LLAMA_CPP_IMATRIX_UPSTREAM_FILES))
LLAMA_CPP_IMATRIX_UPSTREAM_OBJS = $(patsubst llama.cpp/tools/%,o/$(MODE)/llama.cpp/%,$(LLAMA_CPP_IMATRIX_UPSTREAM_SRCS:%.cpp=%.o))

# All imatrix objects
LLAMA_CPP_IMATRIX_OBJS = $(LLAMA_CPP_IMATRIX_UPSTREAM_OBJS)

o/$(MODE)/llama.cpp/imatrix/imatrix:					\
		o/$(MODE)/llama.cpp/imatrix/imatrix.o			\
		o/$(MODE)/llama.cpp/imatrix/imatrix.1.asc.zip.o		\
		o/$(MODE)/llama.cpp/llama.cpp.a

# Include paths for imatrix
LLAMA_CPP_IMATRIX_INCLUDES = -Illama.cpp/tools/imatrix

$(LLAMA_CPP_IMATRIX_OBJS): private CCFLAGS += $(LLAMA_CPP_IMATRIX_INCLUDES)

.PHONY: o/$(MODE)/llama.cpp/imatrix
o/$(MODE)/llama.cpp/imatrix:						\
		o/$(MODE)/llama.cpp/imatrix/imatrix
