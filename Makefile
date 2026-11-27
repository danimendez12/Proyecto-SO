CC = gcc
CPPFLAGS += -Iinclude -D_GNU_SOURCE
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread

ACEPTACIONES_API = bin/tests/aceptacion_api bin/tests/aceptacion_api_remoto \
	bin/tests/aceptacion_multipagina bin/tests/aceptacion_multipagina_remota \
	bin/tests/aceptacion_api_errores bin/tests/aceptacion_multihogar \
	bin/tests/aceptacion_multihogar_errores

.PHONY: all apps examples tests aceptacion clean

all: bin/dsm_node bin/dsm_unir bin/dsm_consola bin/apps/matrices bin/apps/mergesort

apps: bin/apps/matrices bin/apps/matrices_secuencial bin/apps/mergesort

examples: bin/examples/demo_creator bin/examples/demo_reader

tests: bin/tests/test_protocol bin/tests/test_memory bin/tests/test_signals

aceptacion: bin/tests/aceptacion_red bin/tests/aceptacion_memoria bin/tests/aceptacion_directorio \
	bin/tests/aceptacion_paginador bin/tests/aceptacion_paginacion_remota bin/tests/aceptacion_escritura_local \
	bin/tests/aceptacion_coherencia bin/tests/aceptacion_escritura_remota $(ACEPTACIONES_API) \
	bin/tests/aceptacion_app_matrices bin/tests/aceptacion_app_mergesort

PASO_SRC = src/config.c src/protocol.c src/network.c src/memory.c src/directory.c
API_SRC = $(PASO_SRC) src/dsm.c src/pager.c src/signals.c src/coherence.c src/aviso.c src/pool.c
MATRICES_CFLAGS ?= -O2
MERGESORT_CFLAGS ?= -O2
GRUPO = apps/demanda_grupo.c apps/demanda_grupo.h apps/demanda_grupo_privado.h
MERGESORT_MOD = $(filter-out apps/mergesort/main.c,$(wildcard apps/mergesort/*.c))

bin/apps/mergesort: $(GRUPO) $(wildcard apps/mergesort/*.c) $(wildcard apps/mergesort/*.h) \
	$(API_SRC) $(wildcard include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MERGESORT_CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

bin/tests/aceptacion_app_mergesort: tests/aceptacion_app_mergesort.c \
	$(GRUPO) $(MERGESORT_MOD) $(wildcard apps/mergesort/*.h) $(API_SRC) $(wildcard include/*.h) \
	bin/apps/mergesort
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MERGESORT_CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

bin/apps/matrices: $(GRUPO) $(wildcard apps/matrices/*.c) $(wildcard apps/matrices/*.h) \
	$(API_SRC) $(wildcard include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MATRICES_CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

# Las fuentes originales MIT se conservan sin cambios, con su compilacion secuencial.
bin/apps/matrices_secuencial: apps/matrices/original/src/sequential.c \
	apps/matrices/original/src/matrix.c apps/matrices/original/src/matrix.h
	mkdir -p $(@D)
	$(CC) -std=c11 -O2 $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

bin/tests/aceptacion_app_matrices: tests/aceptacion_app_matrices.c bin/apps/matrices \
	bin/apps/matrices_secuencial
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

bin/dsm_unir: tools/dsm_unir.c tools/consola_arranque.h | bin/dsm_consola
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

bin/dsm_consola: tools/dsm_consola.c tools/consola_tareas.c tools/consola_consultas.c $(wildcard tools/*.h) $(API_SRC) $(wildcard include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

bin/tests/aceptacion_consola: tests/aceptacion_consola.py | bin/dsm_unir
	install -D -m 755 $< $@

bin/tests/aceptacion_tareas: tools/consola_tareas.c tools/consola_tareas.h

aceptacion: bin/tests/aceptacion_consola

bin/dsm_node: $(wildcard src/*.c) $(wildcard include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

bin/examples/%: examples/%.c $(API_SRC) $(wildcard include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

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
	include/pager.h include/signals.h include/memory.h include/protocol.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

bin/tests/aceptacion_paginacion_remota: tests/aceptacion_paginacion_remota.c bin/dsm_node
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

bin/tests/aceptacion_escritura_local: tests/aceptacion_escritura_local.c src/pager.c src/signals.c src/memory.c \
	include/pager.h include/signals.h include/memory.h include/protocol.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

bin/tests/aceptacion_coherencia: tests/aceptacion_coherencia.c $(PASO_SRC) $(wildcard include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/aceptacion_coherencia.c $(PASO_SRC) $(LDLIBS)

bin/tests/aceptacion_multipagina: override LDFLAGS += -Wl,--wrap=mmap
bin/tests/aceptacion_multihogar_errores: override LDFLAGS += -Wl,--wrap=send
bin/tests/aceptacion_api_remoto bin/tests/aceptacion_multipagina_remota bin/tests/aceptacion_api_errores \
bin/tests/aceptacion_multihogar bin/tests/aceptacion_multihogar_errores: tests/api_prueba.h

bin/tests/aceptacion_consultas: override LDFLAGS += -Wl,--wrap=dsm_red_enviar
bin/tests/aceptacion_mascaras: tests/api_prueba.h
bin/tests/aceptacion_copias: override LDFLAGS += -Wl,--wrap=dsm_coherencia_copiar_controlado \
	-Wl,--wrap=dsm_coherencia_escribir_respaldo -Wl,--wrap=dsm_directorio_escribir_respaldo
bin/tests/aceptacion_cierre_perdido: tests/api_prueba.h
bin/tests/aceptacion_cierre_perdido: override LDFLAGS += -Wl,--wrap=send
bin/tests/aceptacion_inventario: tools/consola_consultas.c tools/consola_consultas.h
bin/tests/aceptacion_inventario: override CPPFLAGS += -Itools
bin/tests/aceptacion_inventario: override LDFLAGS += -Wl,--wrap=dsm_directorio_init
bin/tests/aceptacion_asignacion_disponibles: tests/api_prueba.h
bin/tests/aceptacion_grupo_compacto: $(GRUPO)
bin/tests/aceptacion_demanda: $(GRUPO) | apps
REGRESIONES = bin/tests/aceptacion_copias bin/tests/aceptacion_mascaras \
	bin/tests/aceptacion_demanda bin/tests/aceptacion_consultas \
	bin/tests/aceptacion_avisos bin/tests/aceptacion_pool bin/tests/aceptacion_tareas \
	bin/tests/aceptacion_60puestos bin/tests/aceptacion_grupo_compacto bin/tests/aceptacion_asignacion_disponibles \
	bin/tests/aceptacion_inventario bin/tests/aceptacion_cierre_perdido
aceptacion: $(REGRESIONES)
$(ACEPTACIONES_API) $(REGRESIONES): bin/tests/%: tests/%.c $(API_SRC) $(wildcard include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

bin/tests/aceptacion_escritura_remota: tests/aceptacion_escritura_remota.c bin/dsm_node
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf bin
