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

#ifndef _rela_h
#define _rela_h

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

struct _rela_vm;
typedef struct _rela_vm rela_vm;

// @return number of relevant stack items
typedef size_t (*rela_callback)(rela_vm* rela);

// Create a Rela VM, install callbacks and compile source code
rela_vm* rela_create(
	const char* source,
	size_t memory,
	size_t callbacks,
	const char** callback_names,
	rela_callback* callback_functions
);

// Execute the Rela VM code once then reset all resources
int rela_run(rela_vm* rela);

void rela_destroy(rela_vm* rela);
void rela_decompile(rela_vm* rela);

// Number of stack items supplied to a callback
size_t rela_depth(rela_vm* rela);
void rela_push_number(rela_vm* rela, double val);
void rela_push_integer(rela_vm* rela, int64_t val);
void rela_push_string(rela_vm* rela, const char* str);
void rela_push_data(rela_vm* rela, void* data);

double rela_pop_number(rela_vm* rela);
int64_t rela_pop_integer(rela_vm* rela);
const char* rela_pop_string(rela_vm* rela);
void* rela_pop_data(rela_vm* rela);

#endif
