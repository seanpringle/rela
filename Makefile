
dev: LFLAGS=-lm -lpcre
dev: CFLAGS=-Wall -Werror -Og -g -std=c11 -DPCRE
dev: rela.o cli.o
	gcc $(CFLAGS) -o rela rela.o cli.o $(LFLAGS)

rel: LFLAGS=-lm -lpcre
rel: CFLAGS=-Wall -O3 -std=c11 -DNDEBUG -DPCRE -DPCRE_STUDY_JIT_COMPILE
rel: rela.o cli.o
	gcc $(CFLAGS) -o rela rela.o cli.o $(LFLAGS)
	ar rcs librela.a rela.o

prof: rel
	LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libprofiler.so.0 CPUPROFILE=/tmp/rela.prof ./rela bench.rela
	google-pprof --web ./rela /tmp/rela.prof

%.o: %.c *.h
	gcc $(CFLAGS) -c $< -o $@

.PHONY: test
test:
	$(foreach script, $(wildcard test/*), echo $(script) && ./rela $(script) &&) true

leak: dev
	$(foreach script, $(wildcard test/*), echo $(script) && valgrind --leak-check=full ./rela $(script) &&) true

clean:
	rm -f rela librela.a *.o
