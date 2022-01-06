
OBJECTS=$(shell ls -1 *.c | sed 's/c$$/o/g')
TESTS=$(wildcard test/*)

dev: LFLAGS=-lm -lpcre
dev: CFLAGS=-Wall -Werror -O0 -g -std=c11 -DPCRE
dev: $(OBJECTS)
	gcc $(CFLAGS) -o rela $(OBJECTS) $(LFLAGS)

rel: LFLAGS=-lm -lpcre
rel: CFLAGS=-Wall -O3 -std=c11 -DNDEBUG -DPCRE -DPCRE_STUDY_JIT_COMPILE
rel: $(OBJECTS)
	gcc $(CFLAGS) -o rela $(OBJECTS) $(LFLAGS)

lite: LFLAGS=-lm
lite: CFLAGS=-Wall -Os -std=c11 -DNDEBUG
lite: $(OBJECTS)
	gcc $(CFLAGS) -o rela $(OBJECTS) $(LFLAGS)

prof: rel
	LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libprofiler.so.0 CPUPROFILE=/tmp/rela.prof ./rela test.ts
	google-pprof --web ./rela /tmp/rela.prof

%.o: %.c *.h
	gcc $(CFLAGS) -c $< -o $@

test: dev
	$(foreach script, $(TESTS), echo $(script) && ./rela $(script) &&) true

clean:
	rm -f rela *.o
