CC = gcc
CPPFLAGS += -Iinclude
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread

.PHONY: all examples tests clean

all: bin/dsm_node

examples: bin/examples/demo_creator bin/examples/demo_reader

tests: bin/tests/test_protocol bin/tests/test_memory bin/tests/test_signals

bin/dsm_node: $(wildcard src/*.c) $(wildcard include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

bin/examples/%: examples/%.c include/dsm.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

bin/tests/test_%: tests/test_%.c include/%.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf bin
