#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#── vi: set noet ft=make ts=8 sw=8 fenc=utf-8 :vi ────────────────────┘

SHELL = /bin/sh
MAKEFLAGS += --no-builtin-rules

.SUFFIXES:
.DELETE_ON_ERROR:
.FEATURES: output-sync

# setup target needs to run before build/config.mk checks make version
ifneq ($(MAKECMDGOALS),setup)
include build/config.mk
include build/rules.mk

include third_party/BUILD.mk
include llamafile/BUILD.mk
include llama.cpp/BUILD.mk
-include stable-diffusion.cpp/BUILD.mk
-include whisper.cpp/BUILD.mk
include localscore/BUILD.mk
endif

# the root package is `o//` by default
# building a package also builds its sub-packages
.PHONY: o/$(MODE)/
o/$(MODE)/:	o/$(MODE)/llamafile					\
		o/$(MODE)/llama.cpp					\
		o/$(MODE)/localscore					\
		o/$(MODE)/third_party					\
		o/$(MODE)/depend.test

# for installing to `make PREFIX=/usr/local`
.PHONY: install
install:	llamafile/zipalign.1					\
		llamafile/server/main.1					\
		llama.cpp/main/main.1					\
		llama.cpp/imatrix/imatrix.1				\
		llama.cpp/quantize/quantize.1				\
		llama.cpp/perplexity/perplexity.1			\
		llama.cpp/mtmd/mtmd-quantize.1				\
		o/$(MODE)/llamafile/zipalign				\
		o/$(MODE)/llamafile/tokenize				\
		o/$(MODE)/llama.cpp/main/main				\
		o/$(MODE)/llama.cpp/imatrix/imatrix			\
		o/$(MODE)/llama.cpp/quantize/quantize			\
		o/$(MODE)/llama.cpp/llama-bench/llama-bench		\
		o/$(MODE)/localscore/localscore		\
		o/$(MODE)/llama.cpp/perplexity/perplexity		\
		o/$(MODE)/llama.cpp/mtmd/mtmd-quantize		\
		o/$(MODE)/llamafile/server/main
	mkdir -p $(PREFIX)/bin
	$(INSTALL) o/$(MODE)/llamafile/zipalign $(PREFIX)/bin/zipalign
	$(INSTALL) o/$(MODE)/llamafile/tokenize $(PREFIX)/bin/llamafile-tokenize
	$(INSTALL) o/$(MODE)/llama.cpp/main/main $(PREFIX)/bin/llamafile
	$(INSTALL) o/$(MODE)/llama.cpp/imatrix/imatrix $(PREFIX)/bin/llamafile-imatrix
	$(INSTALL) o/$(MODE)/llama.cpp/quantize/quantize $(PREFIX)/bin/llamafile-quantize
	$(INSTALL) o/$(MODE)/llama.cpp/llama-bench/llama-bench $(PREFIX)/bin/llamafile-bench
	$(INSTALL) o/$(MODE)/localscore/localscore $(PREFIX)/bin/localscore
	$(INSTALL) build/llamafile-convert $(PREFIX)/bin/llamafile-convert
	$(INSTALL) build/llamafile-upgrade-engine $(PREFIX)/bin/llamafile-upgrade-engine
	$(INSTALL) o/$(MODE)/llama.cpp/perplexity/perplexity $(PREFIX)/bin/llamafile-perplexity
	$(INSTALL) o/$(MODE)/llama.cpp/mtmd/mtmd-quantize $(PREFIX)/bin/mtmd-quantize
	$(INSTALL) o/$(MODE)/llamafile/server/main $(PREFIX)/bin/llamafiler
	mkdir -p $(PREFIX)/share/man/man1
	$(INSTALL) -m 0644 llamafile/zipalign.1 $(PREFIX)/share/man/man1/zipalign.1
	$(INSTALL) -m 0644 llamafile/server/main.1 $(PREFIX)/share/man/man1/llamafiler.1
	$(INSTALL) -m 0644 llama.cpp/main/main.1 $(PREFIX)/share/man/man1/llamafile.1
	$(INSTALL) -m 0644 llama.cpp/imatrix/imatrix.1 $(PREFIX)/share/man/man1/llamafile-imatrix.1
	$(INSTALL) -m 0644 llama.cpp/quantize/quantize.1 $(PREFIX)/share/man/man1/llamafile-quantize.1
	$(INSTALL) -m 0644 llama.cpp/perplexity/perplexity.1 $(PREFIX)/share/man/man1/llamafile-perplexity.1
	$(INSTALL) -m 0644 llama.cpp/mtmd/mtmd-quantize.1 $(PREFIX)/share/man/man1/mtmd-quantize.1

.PHONY: check
check: o/$(MODE)/llamafile/check

.PHONY: cosmocc
cosmocc: $(COSMOCC) # cosmocc toolchain setup

.PHONY: cosmocc-ci
cosmocc-ci: $(COSMOCC) $(PREFIX)/bin/ape # cosmocc toolchain setup in ci context

.PHONY: setup
setup: # Initialize and configure all dependencies (submodules, patches, etc.)
	@echo "Setting up dependencies..."
	@mkdir -p o/tmp
	@if [ ! -f llama.cpp/.git ]; then \
		echo "Initializing llama.cpp submodule..."; \
		git submodule update --init llama.cpp; \
	fi
	@echo "Applying llama.cpp patches..."
	@export TMPDIR=$$(pwd)/o/tmp && ./llama.cpp.patches/apply-patches.sh
	@echo "Setup complete!"

ifneq ($(MAKECMDGOALS),setup)
include build/deps.mk
include build/tags.mk
endif
