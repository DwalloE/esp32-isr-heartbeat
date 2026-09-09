CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L

BUILD := build

.PHONY: all verify disasm clean

all: $(BUILD)/broken $(BUILD)/fixed disasm

$(BUILD):
	@mkdir -p $(BUILD)

# Same source, one flag apart.
$(BUILD)/broken: volatile_demo.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/fixed: volatile_demo.c | $(BUILD)
	$(CC) $(CFLAGS) -DUSE_VOLATILE -o $@ $<

# The artifact: the poll loop, both ways, as the compiler actually emitted it.
disasm: $(BUILD)/broken $(BUILD)/fixed
	@objdump -d --no-show-raw-insn $(BUILD)/broken \
	  | awk '/<wait_for_tick>:/,/^$$/' > $(BUILD)/broken.wait_for_tick.asm
	@objdump -d --no-show-raw-insn $(BUILD)/fixed \
	  | awk '/<wait_for_tick>:/,/^$$/' > $(BUILD)/fixed.wait_for_tick.asm
	@echo "--- wait_for_tick(), NOT volatile -------------------------------"
	@cat $(BUILD)/broken.wait_for_tick.asm
	@echo "--- wait_for_tick(), volatile -----------------------------------"
	@cat $(BUILD)/fixed.wait_for_tick.asm

# Asserts both halves: the bug is present without volatile, and gone with it.
verify: $(BUILD)/broken $(BUILD)/fixed disasm
	@echo "== expecting the volatile-less build to hang and be killed =="
	@set +e; ./$(BUILD)/broken; rc=$$?; set -e; \
	  if [ "$$rc" -eq 2 ]; then \
	    echo "PASS: missing volatile reproduced (exit 2)"; \
	  else \
	    echo "FAIL: expected exit 2, got $$rc."; \
	    echo "      Your compiler chose not to hoist the load. Check"; \
	    echo "      $(BUILD)/broken.wait_for_tick.asm before concluding the"; \
	    echo "      hazard is not real - see docs/volatile.md."; \
	    exit 1; \
	  fi
	@echo "== expecting the volatile build to run to completion =="
	@./$(BUILD)/fixed
	@echo "PASS: volatile build completed 5 beats"

clean:
	rm -rf $(BUILD)
