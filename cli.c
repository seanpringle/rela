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

#include "rela.h"
#include "stdio.h"
#include "string.h"

void hello(rela_vm* rela) {
	rela_push(rela, rela_make_string(rela, "hello world"));
}

rela_register registry[] = {
	{"hello", hello},
};

int main(int argc, char* argv[]) {
	bool decompile = false;
	const char* source = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d")) { decompile = true; continue; }
		source = argv[i];
	}

	if (!source) {
		fprintf(stderr, "missing source file");
		exit(1);
	}

	rela_vm* rela = rela_create(source, NULL, sizeof(registry) / sizeof(rela_register), registry);

	if (!rela) exit(1);
	if (decompile) rela_decompile(rela);

	int rc = rela_run(rela);

	rela_destroy(rela);
	return rc;
}
