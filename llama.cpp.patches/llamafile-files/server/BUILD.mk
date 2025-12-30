#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘
#
# Server BUILD.mk - NATIVE STRUCTURE VERSION
#
# The server code lives in llama.cpp/tools/server/ in the native structure
#

PKGS += LLAMA_CPP_SERVER

# Main server sources from upstream llama.cpp (in tools/server/)
LLAMA_CPP_SERVER_UPSTREAM_FILES := $(wildcard llama.cpp/tools/server/*.cpp)
LLAMA_CPP_SERVER_UPSTREAM_SRCS = $(filter %.cpp,$(LLAMA_CPP_SERVER_UPSTREAM_FILES))
LLAMA_CPP_SERVER_UPSTREAM_OBJS = $(patsubst llama.cpp/tools/%,o/$(MODE)/llama.cpp/%,$(LLAMA_CPP_SERVER_UPSTREAM_SRCS:%.cpp=%.o))

# All server objects (upstream only - llamafile has its own server/)
LLAMA_CPP_SERVER_OBJS = $(LLAMA_CPP_SERVER_UPSTREAM_OBJS)

o/$(MODE)/llama.cpp/server/server.a:				\
		$(LLAMA_CPP_SERVER_OBJS)

o/$(MODE)/llama.cpp/server/server.o: private			\
		CCFLAGS += -Os

o/$(MODE)/llama.cpp/server/impl.o: private CXXFLAGS += -O1

# Include paths for server
LLAMA_CPP_SERVER_INCLUDES = -Illama.cpp/tools/server

$(LLAMA_CPP_SERVER_OBJS): private CCFLAGS += $(LLAMA_CPP_SERVER_INCLUDES)

$(LLAMA_CPP_SERVER_OBJS): llama.cpp/server/BUILD.mk

.PHONY: o/$(MODE)/llama.cpp/server
o/$(MODE)/llama.cpp/server:					\
		o/$(MODE)/llama.cpp/server/server.a
