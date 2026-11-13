CC = gcc
CPPFLAGS += -Iinclude -D_GNU_SOURCE
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread

.PHONY: all examples tests aceptacion clean

all: bin/dsm_node

examples: bin/examples/demo_creator bin/examples/demo_reader

tests: bin/tests/test_protocol bin/tests/test_memory bin/tests/test_signals

aceptacion: bin/tests/aceptacion_red bin/tests/aceptacion_memoria bin/tests/aceptacion_directorio \
	bin/tests/aceptacion_paginador bin/tests/aceptacion_paginacion_remota

PASO_SRC = src/config.c src/protocol.c src/network.c src/memory.c src/directory.c

bin/dsm_node: $(wildcard src/*.c) $(wildcard include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

bin/examples/%: examples/%.c include/dsm.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

bin/tests/test_%: tests/test_%.c include/%.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

bin/tests/aceptacion_red: tests/aceptacion_red.c $(PASO_SRC) $(wildcard include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/aceptacion_red.c $(PASO_SRC) $(LDLIBS)

bin/tests/aceptacion_memoria: tests/aceptacion_memoria.c src/memory.c include/memory.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/aceptacion_memoria.c src/memory.c $(LDLIBS)

bin/tests/aceptacion_directorio: tests/aceptacion_directorio.c $(PASO_SRC) $(wildcard include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/aceptacion_directorio.c $(PASO_SRC) $(LDLIBS)

bin/tests/aceptacion_paginador: tests/aceptacion_paginador.c src/pager.c src/signals.c src/memory.c \
	include/pager.h include/signals.h include/memory.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

bin/tests/aceptacion_paginacion_remota: tests/aceptacion_paginacion_remota.c bin/dsm_node
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf bin
