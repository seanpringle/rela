
dev: LFLAGS=-lm -lpcre
dev: CFLAGS=-Wall -O0 -g -DPCRE -Wno-format-truncation
dev:
	g++ $(CFLAGS) -std=c++17 -o rela cli.cpp $(LFLAGS)

rel: LFLAGS=-lm -lpcre
rel: CFLAGS=-Wall -O3 -DPCRE -Wno-format-truncation
rel:
	g++ $(CFLAGS) -std=c++17 -o rela cli.cpp $(LFLAGS)

prof: rel
	LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libprofiler.so.0 CPUPROFILE=/tmp/rela.prof ./rela bench.rela
	google-pprof --web ./rela /tmp/rela.prof

.PHONY: test
test:
	$(foreach script, $(wildcard test/*), echo $(script) && ./rela $(script) &&) true

leak: dev
	$(foreach script, $(wildcard test/*), echo $(script) && valgrind --leak-check=full ./rela $(script) &&) true

clean:
	rm -f rela librela.a *.o
