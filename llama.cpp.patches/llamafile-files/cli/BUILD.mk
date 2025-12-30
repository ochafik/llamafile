#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘
#
# CLI (Command Line Interface) BUILD.mk - NATIVE STRUCTURE VERSION
#
# The cli code lives in llama.cpp/tools/cli/ in the native structure
# Llamafile-specific files (like embedding.cpp) are in llama.cpp/cli/
#

PKGS += LLAMA_CPP_CLI

# Main cli source from upstream llama.cpp (in tools/cli/)
LLAMA_CPP_CLI_UPSTREAM_FILES := $(wildcard llama.cpp/tools/cli/*.cpp)
LLAMA_CPP_CLI_UPSTREAM_SRCS = $(filter %.cpp,$(LLAMA_CPP_CLI_UPSTREAM_FILES))
LLAMA_CPP_CLI_UPSTREAM_OBJS = $(patsubst llama.cpp/tools/%,o/$(MODE)/llama.cpp/%,$(LLAMA_CPP_CLI_UPSTREAM_SRCS:%.cpp=%.o))

# Llamafile-specific cli files (in cli/ after copying from llamafile-files)
LLAMA_CPP_CLI_LLAMAFILE_FILES := $(wildcard llama.cpp/cli/*)
LLAMA_CPP_CLI_LLAMAFILE_SRCS = $(filter %.cpp,$(LLAMA_CPP_CLI_LLAMAFILE_FILES))
LLAMA_CPP_CLI_LLAMAFILE_OBJS = $(LLAMA_CPP_CLI_LLAMAFILE_SRCS:%.cpp=o/$(MODE)/llama.cpp/cli/%.o)

# All cli objects
LLAMA_CPP_CLI_OBJS = $(LLAMA_CPP_CLI_UPSTREAM_OBJS) $(LLAMA_CPP_CLI_LLAMAFILE_OBJS)

# Include paths for cli
LLAMA_CPP_CLI_INCLUDES = -Illama.cpp/tools/cli

$(LLAMA_CPP_CLI_OBJS): private CCFLAGS += $(LLAMA_CPP_CLI_INCLUDES)

o/$(MODE)/llama.cpp/cli/cli:					\
		o/$(MODE)/llama.cpp/cli/cli.o				\
		o/$(MODE)/llama.cpp/cli/embedding.o		\
		o/$(MODE)/llamafile/server/server.a		\
		o/$(MODE)/llama.cpp/server/server.a		\
		o/$(MODE)/localscore/localscore.a			\
		o/$(MODE)/third_party/mbedtls/mbedtls.a		\
		o/$(MODE)/llama.cpp/mtmd.a			\
		o/$(MODE)/llama.cpp/llama.cpp.a			\
		o/$(MODE)/llamafile/highlight/highlight.a	\
		o/$(MODE)/third_party/stb/stb.a			\
		o/$(MODE)/llama.cpp/cli/cli.1.asc.zip.o	\
		o/$(MODE)/llamafile/server/main.1.asc.zip.o	\
		$(LLAMA_CPP_SERVER_ASSETS:%=o/$(MODE)/%.zip.o)	\
		$(LLAMAFILE_SERVER_ASSETS:%=o/$(MODE)/%.zip.o)	\
		$(THIRD_PARTY_MBEDTLS_A_CERTS:%=o/$(MODE)/%.zip.o) \

.PHONY: o/$(MODE)/llama.cpp/cli
o/$(MODE)/llama.cpp/cli:					\
		o/$(MODE)/llama.cpp/cli/cli
