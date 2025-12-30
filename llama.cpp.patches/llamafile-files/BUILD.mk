#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘
#
# BUILD.mk for llama.cpp - NATIVE STRUCTURE VERSION
#
# This version works with llama.cpp's native directory structure
# without flattening. No renames.sh needed!
#
# Directory structure:
#   llama.cpp/src/       - Core llama source files
#   llama.cpp/include/  - Public headers
#   llama.cpp/common/    - Common utilities
#   llama.cpp/ggml/src/ - GGML source files
#   llama.cpp/ggml/include/ - GGML headers
#
# ============================================================================

PKGS += LLAMA_CPP

# Llama.cpp header files (for build system dependency tracking)
# Get all headers
_llama_cpp_headers := $(shell find llama.cpp -name "*.h" -o -name "*.hpp")
_llama_cpp_headers_full := $(_llama_cpp_headers)

# Also add headers without the llama.cpp/ prefix for mkdeps
_llama_cpp_headers_short := $(foreach h,$(_llama_cpp_headers),$(subst llama.cpp/,,$h))

# Also add headers without src/, common/, ggml/include/ prefixes for relative includes
_llama_cpp_headers_extra := $(foreach h,$(_llama_cpp_headers_short),\
	$(subst src/,$h,$(filter src/%,$h))\
	$(subst common/,$h,$(filter common/%,$h))\
	$(subst ggml/include/,$h,$(filter ggml/include/%,$h))\
	$(subst ggml/src/,$h,$(filter ggml/src/%,$h))\
	$(subst tools/,$h,$(filter tools/%,$h))\
)

LLAMA_CPP_HDRS := $(_llama_cpp_headers_full) $(_llama_cpp_headers_short) $(_llama_cpp_headers_extra)
LLAMA_CPP_INCS = $(shell find llama.cpp -name "*.inc")

# ============================================================================
# Source files - using native llama.cpp structure
# ============================================================================

# Core llama files from src/
LLAMA_CPP_CORE_SRCS :=					\
	llama.cpp/src/llama.cpp				\
	llama.cpp/src/llama-adapter.cpp			\
	llama.cpp/src/llama-arch.cpp			\
	llama.cpp/src/llama-batch.cpp			\
	llama.cpp/src/llama-chat.cpp			\
	llama.cpp/src/llama-context.cpp			\
	llama.cpp/src/llama-cparams.cpp			\
	llama.cpp/src/llama-grammar.cpp			\
	llama.cpp/src/llama-graph.cpp			\
	llama.cpp/src/llama-hparams.cpp			\
	llama.cpp/src/llama-impl.cpp			\
	llama.cpp/src/llama-io.cpp			\
	llama.cpp/src/llama-kv-cache.cpp			\
	llama.cpp/src/llama-kv-cache-iswa.cpp		\
	llama.cpp/src/llama-memory.cpp			\
	llama.cpp/src/llama-memory-hybrid.cpp		\
	llama.cpp/src/llama-memory-recurrent.cpp		\
	llama.cpp/src/llama-mmap.cpp			\
	llama.cpp/src/llama-model.cpp			\
	llama.cpp/src/llama-model-loader.cpp		\
	llama.cpp/src/llama-model-saver.cpp		\
	llama.cpp/src/llama-quant.cpp			\
	llama.cpp/src/llama-sampling.cpp			\
	llama.cpp/src/llama-vocab.cpp			\
	llama.cpp/src/unicode.cpp			\
	llama.cpp/src/unicode-data.cpp

# Common files
LLAMA_CPP_COMMON_SRCS :=				\
	llama.cpp/common/arg.cpp				\
	llama.cpp/common/chat-parser.cpp			\
	llama.cpp/common/chat-peg-parser.cpp		\
	llama.cpp/common/common.cpp				\
	llama.cpp/common/console.cpp				\
	llama.cpp/common/download.cpp				\
	llama.cpp/common/json-schema-to-grammar.cpp	\
	llama.cpp/common/json-partial.cpp			\
	llama.cpp/common/llguidance.cpp				\
	llama.cpp/common/log.cpp					\
	llama.cpp/common/ngram-cache.cpp			\
	llama.cpp/common/peg-parser.cpp				\
	llama.cpp/common/preset.cpp					\
	llama.cpp/common/regex-partial.cpp			\
	llama.cpp/common/sampling.cpp				\
	llama.cpp/common/speculative.cpp			\
	llama.cpp/common/unicode.cpp

# GGML files
LLAMA_CPP_GGML_SRCS :=					\
	llama.cpp/ggml/src/ggml.c				\
	llama.cpp/ggml/src/ggml-alloc.c			\
	llama.cpp/ggml/src/ggml-backend.c		\
	llama.cpp/ggml/src/ggml-opt.c			\
	llama.cpp/ggml/src/ggml-quants.c		\
	llama.cpp/ggml/src/gguf.c

# Llamafile-specific files (in llama.cpp root after copying)
LLAMA_CPP_ROOT_FILES := $(wildcard llama.cpp/*.*)
LLAMA_CPP_ROOT_SRCS_CPP = $(filter %.cpp,$(LLAMA_CPP_ROOT_FILES))
LLAMA_CPP_ROOT_SRCS_C = $(filter %.c,$(LLAMA_CPP_ROOT_FILES))

# All source files
LLAMA_CPP_SRCS_CPP =					\
	$(LLAMA_CPP_CORE_SRCS)				\
	$(LLAMA_CPP_COMMON_SRCS)				\
	$(LLAMA_CPP_GGML_SRCS)				\
	$(LLAMA_CPP_ROOT_SRCS_CPP)

LLAMA_CPP_SRCS_C = $(LLAMA_CPP_ROOT_SRCS_C)
LLAMA_CPP_SRCS = $(LLAMA_CPP_SRCS_CPP) $(LLAMA_CPP_SRCS_C)

# Object files (map src/llama.cpp -> llama.cpp for output)
LLAMA_CPP_SRCS_OBJS_C = $(subst llama.cpp/src/,o/$(MODE)/llama.cpp/,$(LLAMA_CPP_SRCS_C:%.c=%.o))
LLAMA_CPP_SRCS_OBJS_CPP = $(subst llama.cpp/src/,o/$(MODE)/llama.cpp/,$(LLAMA_CPP_SRCS_CPP:%.cpp=%.o))
LLAMA_CPP_SRCS_OBJS = $(LLAMA_CPP_SRCS_OBJS_C) $(LLAMA_CPP_SRCS_OBJS_CPP)

# All files for zip embedding
LLAMA_CPP_FILES = $(shell find llama.cpp -type f ! -path "*/.*" ! -path "*/.git/*" ! -path "*/models/*")
LLAMA_CPP_OBJS =					\
	$(LLAMAFILE_OBJS)				\
	$(LLAMA_CPP_SRCS_OBJS)

o/$(MODE)/llama.cpp/llama.cpp.a: $(LLAMA_CPP_OBJS)

# ============================================================================
# Include paths
# ============================================================================

LLAMA_CPP_INCLUDES =					\
	-Illama.cpp					\
	-Illama.cpp/include				\
	-Illama.cpp/src					\
	-Illama.cpp/common				\
	-Illama.cpp/ggml/include			\
	-Illama.cpp/ggml/src

$(LLAMA_CPP_SRCS_OBJS): private				\
		CCFLAGS +=				\
			$(LLAMA_CPP_INCLUDES)		\
			-DNDEBUG

# Add include paths for tools directories (upstream structure)
LLAMA_CPP_TOOLS_INCLUDES = -Illama.cpp/tools/mtmd -Illama.cpp/tools/cli -Illama.cpp/tools/server

$(LLAMA_CPP_OBJS): private				\
		CCFLAGS +=				\
			$(LLAMA_CPP_TOOLS_INCLUDES)	\
			-Illamafile

$(LLAMA_CPP_OBJS): private				\
		CCFLAGS +=				\
			-DGGML_MULTIPLATFORM		\
			-DGGML_USE_LLAMAFILE

# ============================================================================
# Sub-BUILD.mk includes
# ============================================================================

include llama.cpp/tools/mtmd/BUILD.mk
include llama.cpp/tools/server/BUILD.mk
include llama.cpp/tools/cli/BUILD.mk
include llama.cpp/tools/imatrix/BUILD.mk
include llama.cpp/tools/quantize/BUILD.mk
include llama.cpp/tools/perplexity/BUILD.mk
include llama.cpp/tools/llama-bench/BUILD.mk

# ============================================================================
# Compiler optimizations (same as before)
# ============================================================================

o/$(MODE)/llama.cpp/ggml.o \
o/$(MODE)/llama.cpp/ggml-vector-amd-avx2.o \
o/$(MODE)/llama.cpp/ggml-vector-amd-avx512bf16.o \
o/$(MODE)/llama.cpp/ggml-vector-amd-avx512.o \
o/$(MODE)/llama.cpp/ggml-vector-amd-avx.o \
o/$(MODE)/llama.cpp/ggml-vector-amd-f16c.o \
o/$(MODE)/llama.cpp/ggml-vector-amd-fma.o \
o/$(MODE)/llama.cpp/ggml-vector-arm80.o \
o/$(MODE)/llama.cpp/ggml-vector-arm82.o: \
		private CCFLAGS += -O3 -mgcc

o/$(MODE)/llama.cpp/ggml-alloc.o			\
o/$(MODE)/llama.cpp/ggml-backend.o			\
o/$(MODE)/llama.cpp/json-schema-to-grammar.o		\
o/$(MODE)/llama.cpp/vector.o				\
o/$(MODE)/llama.cpp/unicode.o				\
o/$(MODE)/llama.cpp/sampling.o				\
o/$(MODE)/llama.cpp/common.o:				\
		private CCFLAGS += -Os

o/$(MODE)/llama.cpp/unicode-data.o:			\
		private CCFLAGS += -mgcc

o/$(MODE)/llama.cpp/ggml-quants.o: private CXXFLAGS += -Os
o/$(MODE)/llama.cpp/ggml-quants-amd-k8.o: private TARGET_ARCH += -Xx86_64-mtune=k8
o/$(MODE)/llama.cpp/ggml-quants-amd-ssse3.o: private TARGET_ARCH += -Xx86_64-mtune=core2 -Xx86_64-mssse3
o/$(MODE)/llama.cpp/ggml-quants-amd-avx.o: private TARGET_ARCH += -Xx86_64-mtune=sandybridge -Xx86_64-mavx
o/$(MODE)/llama.cpp/ggml-quants-amd-avx2.o: private TARGET_ARCH += -Xx86_64-mtune=skylake -Xx86_64-mavx -Xx86_64-mf16c -Xx86_64-mfma -Xx86_64-mavx2
o/$(MODE)/llama.cpp/ggml-quants-amd-avx512.o: private TARGET_ARCH += -Xx86_64-mtune=cannonlake -Xx86_64-mavx -Xx86_64-mf16c -Xx86_64-mfma -Xx86_64-mavx2 -Xx86_64-mavx512f
o/$(MODE)/llama.cpp/ggml-quants-amd-avx512vl.o: private TARGET_ARCH += -Xx86_64-mtune=cannonlake -Xx86_64-mavx -Xx86_64-mf16c -Xx86_64-mfma -Xx86_64-mavx2 -Xx86_64-mavx512f -Xx86_64-mavx512bw -Xx86_64-mavx512dq -Xx86_64-mavx512vl

o/$(MODE)/llama.cpp/ggml-vector.o: private CXXFLAGS += -Os
o/$(MODE)/llama.cpp/ggml-vector-amd-k8.o: private TARGET_ARCH += -Xx86_64-mtune=k8
o/$(MODE)/llama.cpp/ggml-vector-amd-ssse3.o: private TARGET_ARCH += -Xx86_64-mtune=core2 -Xx86_64-mssse3
o/$(MODE)/llama.cpp/ggml-vector-amd-avx.o: private TARGET_ARCH += -Xx86_64-mtune=sandybridge -Xx86_64-mavx
o/$(MODE)/llama.cpp/ggml-vector-amd-fma.o: private TARGET_ARCH += -Xx86_64-mtune=bdver2 -Xx86_64-mavx -Xx86_64-mfma
o/$(MODE)/llama.cpp/ggml-vector-amd-f16c.o: private TARGET_ARCH += -Xx86_64-mtune=ivybridge -Xx86_64-mavx -Xx86_64-mf16c
o/$(MODE)/llama.cpp/ggml-vector-amd-avx2.o: private TARGET_ARCH += -Xx86_64-mtune=skylake -Xx86_64-mavx -Xx86_64-mf16c -Xx86_64-mfma -Xx86_64-mavx2
o/$(MODE)/llama.cpp/ggml-vector-amd-avx512.o: private TARGET_ARCH += -Xx86_64-mtune=cannonlake -Xx86_64-mavx -Xx86_64-mf16c -Xx86_64-mfma -Xx86_64-mavx2 -Xx86_64-mavx512f
o/$(MODE)/llama.cpp/ggml-vector-amd-avx512vl.o: private TARGET_ARCH += -Xx86_64-mtune=cannonlake -Xx86_64-mavx -Xx86_64-mf16c -Xx86_64-mfma -Xx86_64-mavx2 -Xx86_64-mavx512f -Xx86_64-mavx512bw -Xx86_64-mavx512dq -Xx86_64-mavx512vl
o/$(MODE)/llama.cpp/ggml-vector-amd-avx512bf16.o: private TARGET_ARCH += -Xx86_64-mtune=znver4 -Xx86_64-mavx -Xx86_64-mf16c -Xx86_64-mfma -Xx86_64-mavx2 -Xx86_64-mavx512f -Xx86_64-mavx512bw -Xx86_64-mavx512dq -Xx86_64-mavx512vl -Xx86_64-mavx512bf16
o/$(MODE)/llama.cpp/ggml-vector-arm82.o: private TARGET_ARCH += -Xaarch64-march=armv8.2-a+fp16

$(LLAMA_CPP_OBJS): llama.cpp/BUILD.mk

.PHONY: o/$(MODE)/llama.cpp
o/$(MODE)/llama.cpp: 					\
		o/$(MODE)/llama.cpp/tools/cli			\
		o/$(MODE)/llama.cpp/tools/mtmd			\
		o/$(MODE)/llama.cpp/tools/server			\
		o/$(MODE)/llama.cpp/tools/imatrix			\
		o/$(MODE)/llama.cpp/tools/quantize			\
		o/$(MODE)/llama.cpp/tools/perplexity			\
		o/$(MODE)/llama.cpp/tools/llama-bench			\

