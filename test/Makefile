CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c11
LDFLAGS ?= -pthread

BUILD := build

.PHONY: all run clean

all: run

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_seqlock: test_seqlock.c ../main/seqlock.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_seqlock.c $(LDFLAGS)

run: $(BUILD)/test_seqlock
	./$(BUILD)/test_seqlock

clean:
	rm -rf $(BUILD)
