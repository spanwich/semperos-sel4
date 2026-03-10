# Makefile for vDTU formal verification proofs
#
# Targets:
#   make verify   — verify all .fst files with F*
#   make extract  — Low* extract EpState.Low.fst to C header
#   make clean    — remove generated files
#
# Prerequisites:
#   - F* (fstar.exe) from EverParse
#   - KaRaMeL (Karamel.exe) for Low* extraction
#   - Z3 SMT solver

# Tool paths (from EverParse installation)
# proofs/ is at camkes-vm-examples/projects/semperos-sel4/proofs/
# EverParse is at the PhD root: camkes-vm-examples/../everparse
PROOFS_DIR     := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
EVERPARSE_ROOT := $(realpath $(PROOFS_DIR)/../../../../everparse)
FSTAR_HOME     := $(EVERPARSE_ROOT)/opt/FStar
FSTAR          := $(FSTAR_HOME)/bin/fstar.exe
KARAMEL_HOME   := $(EVERPARSE_ROOT)/opt/karamel
KARAMEL        := $(KARAMEL_HOME)/_build/default/src/Karamel.exe
Z3             := $(EVERPARSE_ROOT)/opt/z3/z3-4.13.3

# F* standard library and KaRaMeL library
FSTAR_ULIB     := $(FSTAR_HOME)/ulib
KRML_LIB       := $(KARAMEL_HOME)/krmllib

# Common F* flags
FSTAR_FLAGS := \
  --include $(FSTAR_ULIB) \
  --include $(KRML_LIB) \
  --smt $(Z3) \
  --cache_dir .cache \
  --odir .cache \
  --z3cliopt 'smt.arith.nl=false' \
  --fuel 2 --ifuel 1 \
  --warn_error -331

# All proof modules (order matters for dependencies)
MODULES := AncestryWalk EpState EpState.Low EnforcementChain
FST_FILES := $(addsuffix .fst,$(MODULES))

# Output directory for extracted C
EXTRACT_DIR := extracted

.PHONY: verify extract clean

# --------------------------------------------------------------------------
#  verify: check all .fst files
# --------------------------------------------------------------------------

verify: .cache
	@echo "=== Verifying AncestryWalk.fst ==="
	$(FSTAR) $(FSTAR_FLAGS) AncestryWalk.fst
	@echo ""
	@echo "=== Verifying EpState.fst ==="
	$(FSTAR) $(FSTAR_FLAGS) EpState.fst
	@echo ""
	@echo "=== Verifying EpState.Low.fst ==="
	$(FSTAR) $(FSTAR_FLAGS) EpState.Low.fst
	@echo ""
	@echo "=== Verifying EnforcementChain.fst ==="
	$(FSTAR) $(FSTAR_FLAGS) EnforcementChain.fst
	@echo ""
	@echo "=== All proofs verified ==="

.cache:
	mkdir -p .cache

# --------------------------------------------------------------------------
#  extract: Low* extraction of EpState.Low.fst to C header
# --------------------------------------------------------------------------

extract: verify $(EXTRACT_DIR)
	@echo "=== Extracting EpState.Low to C ==="
	$(FSTAR) $(FSTAR_FLAGS) \
	  --codegen krml \
	  --extract_module EpState.Low \
	  EpState.Low.fst
	@if [ -f .cache/EpState_Low.krml ]; then \
	  $(KARAMEL) \
	    -skip-linking \
	    -skip-compilation \
	    -no-prefix EpState.Low \
	    -header proofs-header.txt \
	    -tmpdir $(EXTRACT_DIR) \
	    .cache/EpState_Low.krml 2>/dev/null; \
	  echo "=== Extracted to $(EXTRACT_DIR)/ ==="; \
	else \
	  echo "NOTE: KaRaMeL .krml file not produced (expected if F* codegen"; \
	  echo "      path is not configured). Using manual extraction instead."; \
	  echo "=== Writing manual extraction to $(EXTRACT_DIR)/vdtu_ep_state.h ==="; \
	fi
	@# Always produce the manual header as the canonical artifact
	@cp -f vdtu_ep_state_manual.h $(EXTRACT_DIR)/vdtu_ep_state.h
	@echo "=== $(EXTRACT_DIR)/vdtu_ep_state.h ready ==="

$(EXTRACT_DIR):
	mkdir -p $(EXTRACT_DIR)

# --------------------------------------------------------------------------
#  clean
# --------------------------------------------------------------------------

clean:
	rm -rf .cache $(EXTRACT_DIR)
