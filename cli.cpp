// Rela, MIT License
//
// Copyright (c) 2021 Sean Pringle <sean.pringle@gmail.com> github:seanpringle
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "rela.hpp"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static char* slurp(const char* script);

class RelaCLI : public Rela {
public:
	struct {
		int main = 0;
	} modules;

	RelaCLI(const char* source) : Rela() {
		map_set(map_core(), make_string("hello"), make_function(1));
		modules.main = module(source);
	}

	void execute(int id) override {
		if (id == 1) hello();
	}

	void hello() {
		stack_push(make_string("hello world"));
	}
};

int run(const char* source, bool decompile) {
	RelaCLI rela(source);
	if (decompile) rela.decompile();
	return rela.run();
}

int main(int argc, char* argv[]) {
	bool decompile = false;
	const char* script = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d")) { decompile = true; continue; }
		script = argv[i];
	}

	if (!script) {
		fprintf(stderr, "missing script file");
		exit(1);
	}

	char* source = slurp(script);

	if (!source) {
		fprintf(stderr, "cannot read script file: %s", script);
		exit(1);
	}

	int rc = run(source, decompile);

	free(source);
	return rc;
}

static char* slurp(const char* script) {
	char* source = NULL;
	struct stat st;
	if (stat(script, &st) == 0) {
		FILE *file = fopen(script, "r");
		if (file) {
			size_t bytes = st.st_size;
			source = (char*)malloc(bytes + 1);
			size_t read = fread(source, 1, bytes, file);
			source[bytes] = 0;
			if (read != bytes) {
				free(source);
				source = NULL;
			}
		}
		fclose(file);
	}
	return source;
}

