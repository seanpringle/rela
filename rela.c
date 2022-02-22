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

#define _GNU_SOURCE
#include "rela.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <float.h>
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>

#ifndef NDEBUG
#include <signal.h>
#endif

#ifdef PCRE
#include <pcre.h>
#endif

#ifdef __linux__
#include <sys/mman.h>
#include <errno.h>
#endif

enum opcode_t {
	// <order-important>
	OP_STOP=0, OP_JMP, OP_FOR, OP_PID, OP_LIT, OP_MARK, OP_LIMIT, OP_CLEAN, OP_RETURN,
	OPP_FNAME, OPP_CFUNC, OPP_ASSIGNL, OPP_ASSIGNP, OPP_MUL_LIT, OPP_ADD_LIT, OPP_GNAME,
	OPP_COPIES, OPP_UPDATE,
	// </order-important>
	OP_PRINT, OP_COROUTINE, OP_RESUME, OP_YIELD, OP_CALL, OP_GLOBAL, OP_MAP, OP_VECTOR, OP_VPUSH,
	OP_META_SET, OP_META_GET, OP_UNMAP, OP_LOOP, OP_UNLOOP, OP_BREAK, OP_CONTINUE, OP_JFALSE,
	OP_JTRUE, OP_NIL, OP_COPY, OP_SHUNT, OP_SHIFT, OP_TRUE, OP_FALSE, OP_ASSIGN, OP_AND, OP_OR,
	OP_FIND, OP_SET, OP_GET, OP_COUNT, OP_DROP, OP_ADD, OP_NEG, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NOT,
	OP_EQ, OP_NE, OP_LT, OP_GT, OP_LTE, OP_GTE, OP_CONCAT, OP_MATCH, OP_SORT, OP_ASSERT, OP_GC,
	OP_SIN, OP_COS, OP_TAN, OP_ASIN, OP_ACOS, OP_ATAN, OP_SINH, OP_COSH, OP_TANH, OP_CEIL, OP_FLOOR,
	OP_SQRT, OP_ABS, OP_ATAN2, OP_LOG, OP_LOG10, OP_POW, OP_MIN, OP_MAX, OP_TYPE, OP_UNPACK,
	OPERATIONS
};

enum type_t {
	NIL = 0, INTEGER, FLOAT, STRING, BOOLEAN, VECTOR, MAP, SUBROUTINE, COROUTINE, CALLBACK, USERDATA, NODE,
	TYPES
};

const char* type_names[TYPES] = {
	[NIL] = "nil",
	[INTEGER] = "integer",
	[FLOAT] = "number",
	[STRING] = "string",
	[BOOLEAN] = "boolean",
	[VECTOR] = "vector",
	[MAP] = "map",
	[SUBROUTINE] = "subroutine",
	[COROUTINE] = "coroutine",
	[CALLBACK] = "callback",
	[USERDATA] = "userdata",
	[NODE] = "node",
};

enum {
	NODE_MULTI=1, NODE_NAME, NODE_LITERAL, NODE_OPCODE, NODE_IF, NODE_WHILE, NODE_FUNCTION, NODE_RETURN,
	NODE_VEC, NODE_MAP, NODE_FOR, NODE_CALL_CHAIN, NODE_OPERATOR
};

#define STRTMP 100
#define STRBUF 1000

struct _vec_t;
struct _map_t;
struct _cor_t;
struct _node_t;
struct _data_t;

typedef struct {
	enum type_t type;
	union {
		bool flag;
		int sub;
		int64_t inum;
		double fnum;
		const char* str;
		struct _vec_t* vec;
		struct _map_t* map;
		struct _cor_t* cor;
		struct _node_t* node;
		struct _data_t* data;
		rela_callback cb;
	};
} item_t;

typedef struct _vec_t {
	item_t meta;
	item_t* items;
	int count;
	int buffer;
} vec_t; // vector

typedef struct _map_t {
	item_t meta;
	vec_t keys;
	vec_t vals;
} map_t;

typedef struct _data_t {
	item_t meta;
	void* ptr;
} data_t; // userdata

// powers of 2
#define STACK 32u
#define LOCALS 32u
#define PATH 8u

typedef struct {
	int loops;
	int marks;
	int ip;
	item_t map;
	struct {
		const char* keys[LOCALS];
		item_t vals[LOCALS];
		int depth;
	} locals;
	struct {
		int cells[PATH];
		int depth;
	} path;
} frame_t;

typedef struct _cor_t {
	int ip;
	int state;
	struct {
		item_t cells[STACK];
		int depth;
	} stack;
	struct {
		item_t cells[STACK];
		int depth;
	} other;
	struct {
		frame_t cells[STACK];
		int depth;
	} frames;
	struct {
		int cells[STACK];
		int depth;
	} marks;
	struct {
		int cells[STACK];
		int depth;
	} loops;
	item_t map;
} cor_t; // coroutine

typedef struct{
	enum opcode_t op;
	int cache;
	item_t item;
} code_t; // compiled "bytecode"

typedef struct _node_t {
	int type;
	int opcode;
	int call;
	item_t item;
	struct _node_t *args;
	struct _node_t *chain;
	vec_t* keys;
	vec_t* vals;
	int results;
	struct {
		int id;
		int ids[PATH];
		int depth;
	} fpath;
	bool index;
	bool field;
	bool method;
	bool control;
	bool single;
} node_t; // AST

typedef struct {
	char *name;
	int opcode;
} keyword_t;

keyword_t keywords[] = {
	{ .name = "global",     .opcode = OP_GLOBAL },
	{ .name = "true",       .opcode = OP_TRUE   },
	{ .name = "false",      .opcode = OP_FALSE  },
	{ .name = "nil",        .opcode = OP_NIL    },
};

typedef struct {
	char *name;
	int precedence;
	int opcode;
	int argc;
	bool single; // single result
} operator_t;

// order is important; eg longer >= should be matched before >
operator_t operators[] = {
	{ .name = "||", .precedence = 0, .opcode = OP_OR,    .argc = 2, .single = true },
	{ .name = "or", .precedence = 0, .opcode = OP_OR,    .argc = 2, .single = true },
	{ .name = "&&", .precedence = 1, .opcode = OP_AND,   .argc = 2, .single = true },
	{ .name = "==", .precedence = 2, .opcode = OP_EQ,    .argc = 2, .single = true },
	{ .name = "!=", .precedence = 2, .opcode = OP_NE,    .argc = 2, .single = true },
	{ .name = ">=", .precedence = 2, .opcode = OP_GTE,   .argc = 2, .single = true },
	{ .name = ">",  .precedence = 2, .opcode = OP_GT,    .argc = 2, .single = true },
	{ .name = "<=", .precedence = 2, .opcode = OP_LTE,   .argc = 2, .single = true },
	{ .name = "<",  .precedence = 2, .opcode = OP_LT,    .argc = 2, .single = true },
	{ .name = "~",  .precedence = 2, .opcode = OP_MATCH, .argc = 2, .single = true },
	{ .name = "+",  .precedence = 3, .opcode = OP_ADD,   .argc = 2, .single = true },
	{ .name = "-",  .precedence = 3, .opcode = OP_SUB,   .argc = 2, .single = true },
	{ .name = "*",  .precedence = 4, .opcode = OP_MUL,   .argc = 2, .single = true },
	{ .name = "/",  .precedence = 4, .opcode = OP_DIV,   .argc = 2, .single = true },
	{ .name = "%",  .precedence = 4, .opcode = OP_MOD,   .argc = 2, .single = true },
	{ .name = "...",.precedence = 4, .opcode = OP_UNPACK,.argc = 1, .single = false },
};

typedef keyword_t modifier_t;

modifier_t modifiers[] = {
	{ .name = "#", .opcode = OP_COUNT },
	{ .name = "-", .opcode = OP_NEG   },
	{ .name = "!", .opcode = OP_NOT   },
};

typedef struct {
	void** pages;
	bool* used;
	bool* mark;
	int page;
	int object;
	int extant;
	int depth;
	int next;
} pool_t;

typedef struct {
	char** cells;
	bool* mark;
	int depth;
} string_region_t;

typedef struct _rela_vm {
	// routines[0] == main coroutine, always set at run-time
	// routines[1...n] resume/yield chain
	vec_t routines;
	cor_t* routine;

	map_t* scope_core;
	map_t* scope_global;

	struct {
		node_t** cells;
		int depth;
	} nodes;

	pool_t maps;
	pool_t vecs;
	pool_t cors;
	pool_t data;

	// compiled "bytecode"
	struct {
		code_t* cells;
		int depth;
		int start;
	} code;

	struct {
		item_t* cfunc;
		int cfuncs;
	} cache;

	struct {
		vec_t entries;
		vec_t names;
	} modules;

	// interned strings
	string_region_t stringsA; // young
	string_region_t stringsB; // old

	// compile-time scope tree
	struct {
		int id;
		int ids[PATH];
		int depth;
	} fpath;

	jmp_buf jmp;
	char err[STRTMP];
	void* custom;

#ifdef __linux__
	struct {
		void** iptrx;
		unsigned char* code;
		int depth;
		int limit;
	} jit;
#endif

} rela_vm;

typedef int (*strcb)(int);

static item_t nil(rela_vm* vm);
static bool equal(rela_vm* vm, item_t a, item_t b);
static bool less(rela_vm* vm, item_t a, item_t b);
static void push(rela_vm* vm, item_t item);
static item_t pop(rela_vm* vm);
static item_t top(rela_vm* vm);
static const char* tmptext(rela_vm* vm, item_t item, char* tmp, int size);
static int parse(rela_vm* vm, const char *source, int results, int mode);
static int parse_block(rela_vm* vm, const char *source, node_t *node);
static int parse_branch(rela_vm* vm, const char *source, node_t *node);
static int parse_arglist(rela_vm* vm, const char *source);
static int parse_node(rela_vm* vm, const char *source);
static void method(rela_vm* vm, item_t item, int argc, item_t* argv, int retc, item_t* retv);
static bool tick(rela_vm* vm);

#ifdef NDEBUG
void explode(rela_vm* vm) { longjmp(vm->jmp, 1); }
#else
void explode(rela_vm* vm) { raise(SIGUSR1); }
#endif

#define ensure(vm,c,...) if (!(c)) { snprintf(vm->err, sizeof(vm->err), __VA_ARGS__); explode(vm); }

static int pool_index(rela_vm* vm, pool_t* pool, void* ptr) {
	for (int page = 0; page < pool->depth/pool->page; page++) {
		if (pool->pages[page] <= ptr && ptr < pool->pages[page] + pool->object*pool->page) {
			return page*pool->page + (ptr - pool->pages[page])/pool->object;
		}
	}
	return -1;
}

static void* pool_ptr(rela_vm* vm, pool_t* pool, int index) {
	assert(index < pool->depth);
	int page = index/pool->page;
	int cell = index%pool->page;
	return pool->pages[page] + (cell * pool->object);
}

static void* pool_allot_index(rela_vm* vm, pool_t* pool, int index) {
	assert(index < pool->depth);
	pool->used[index] = true;

	void* ptr = pool_ptr(vm, pool, index);
	memset(ptr, 0, pool->object);

	pool->next = index+1;
	pool->extant++;

	return ptr;
}

static void* pool_alloc(rela_vm* vm, pool_t* pool) {
	for (int i = pool->next; i < pool->depth; i++) {
		if (!pool->used[i]) return pool_allot_index(vm, pool, i);
	}
	for (int i = 0; i < pool->next; i++) {
		if (!pool->used[i]) return pool_allot_index(vm, pool, i);
	}

	int index = pool->depth;
	int pages = pool->depth/pool->page + 1;

	pool->depth += pool->page;

	pool->used = realloc(pool->used, sizeof(bool) * pool->depth);
	pool->mark = realloc(pool->mark, sizeof(bool) * pool->depth);
	pool->pages = realloc(pool->pages, pages * sizeof(void*));
	ensure(vm, pool->used && pool->mark && pool->pages, "oom");

	memset(pool->used + sizeof(bool)*index, 0, sizeof(bool)*pool->page);
	memset(pool->mark + sizeof(bool)*index, 0, sizeof(bool)*pool->page);

	pool->pages[pages-1] = malloc(pool->object * pool->page);
	ensure(vm, pool->pages[pages-1], "oom");

	return pool_allot_index(vm, pool, index);
}

static void pool_free(rela_vm* vm, pool_t* pool, void* ptr) {
	int index = pool_index(vm, pool, ptr);
	if (index >= 0) {
		memset(ptr, 0, pool->object);
		pool->used[index] = false;
		pool->extant--;
	}
	assert(pool->extant >= 0);
}

static void pool_clear(rela_vm* vm, pool_t* pool) {
	for (int i = 0, l = pool->depth/pool->page; i < l; i++) {
		free(pool->pages[i]);
	}
	free(pool->pages);
	free(pool->used);
	free(pool->mark);
	memset(pool, 0, sizeof(pool_t));
}

#define RESULTS_DISCARD 0
#define RESULTS_FIRST 1
#define RESULTS_ALL -1

#define PARSE_UNGREEDY 0
// parse() consumes multiple nodes: a[,b]
#define PARSE_COMMA 1<<0
// parse() consumes and/or nodes: x and y or z
#define PARSE_ANDOR 1<<2

#define PROCESS_ASSIGN (1<<0)
#define PROCESS_CHAIN (1<<1)

#define COR_SUSPENDED 0
#define COR_RUNNING 1
#define COR_DEAD 2

static int str_lower_bound(rela_vm* vm, string_region_t* region, const char *str);
static size_t vec_size(rela_vm* vm, vec_t* vec);
static item_t vec_get(rela_vm* vm, vec_t* vec, int i);
static void gc_mark_vec(rela_vm* vm, vec_t* vec);
static void gc_mark_map(rela_vm* vm, map_t* map);
static void gc_mark_cor(rela_vm* vm, cor_t* cor);
static void gc_mark_str(rela_vm* vm, const char* str);
static void gc_mark_data(rela_vm* vm, data_t* data);

static void gc_mark_item(rela_vm* vm, item_t item) {
	if (item.type == STRING) gc_mark_str(vm, item.str);
	if (item.type == VECTOR) gc_mark_vec(vm, item.vec);
	if (item.type == MAP) gc_mark_map(vm, item.map);
	if (item.type == COROUTINE) gc_mark_cor(vm, item.cor);
	if (item.type == USERDATA && item.data->meta.type != NIL) gc_mark_item(vm, item.data->meta);
	if (item.type == USERDATA) gc_mark_data(vm, item.data);
}

static void gc_mark_str(rela_vm* vm, const char* str) {
	int index = str_lower_bound(vm, &vm->stringsA, str);
	if (index < vm->stringsA.depth && vm->stringsA.cells[index] == str) vm->stringsA.mark[index] = true;
}

static void gc_mark_vec(rela_vm* vm, vec_t* vec) {
	if (!vec) return;
	gc_mark_item(vm, vec->meta);
	int index = pool_index(vm, &vm->vecs, vec);
	if (index >= 0) {
		if (vm->vecs.mark[index]) return;
		vm->vecs.mark[index] = true;
	}
	for (int i = 0, l = vec_size(vm, vec); i < l; i++) {
		gc_mark_item(vm, vec_get(vm, vec, i));
	}
}

static void gc_mark_map(rela_vm* vm, map_t* map) {
	if (!map) return;
	gc_mark_item(vm, map->meta);
	int index = pool_index(vm, &vm->maps, map);
	if (index >= 0) {
		if (vm->maps.mark[index]) return;
		vm->maps.mark[index] = true;
	}
	gc_mark_vec(vm, &map->keys);
	gc_mark_vec(vm, &map->vals);
}

static void gc_mark_cor(rela_vm* vm, cor_t* cor) {
	if (!cor) return;

	int index = pool_index(vm, &vm->cors, cor);
	if (index >= 0) {
		if (vm->cors.mark[index]) return;
		vm->cors.mark[index] = true;
	}

	for (int i = 0, l = cor->stack.depth; i < l; i++)
		gc_mark_item(vm, cor->stack.cells[i]);

	for (int i = 0, l = cor->other.depth; i < l; i++)
		gc_mark_item(vm, cor->other.cells[i]);

	gc_mark_item(vm, cor->map);

	for (int i = 0, l = cor->frames.depth; i < l; i++) {
		frame_t* frame = &cor->frames.cells[i];
		for (int j = 0; j < frame->locals.depth; j++) {
			gc_mark_item(vm, frame->locals.vals[j]);
		}
	}
}

static void gc_mark_data(rela_vm* vm, data_t* data) {
	if (!data) return;
	int index = pool_index(vm, &vm->data, data);
	if (index >= 0) {
		if (vm->data.mark[index]) return;
		vm->data.mark[index] = true;
	}
}

// A naive mark-and-sweep collector that is never called implicitly
// at run-time. Can be explicitly triggered with "collect()" via
// script or with rela_collect() via callback.
static void gc(rela_vm* vm) {
	memset(vm->maps.mark, 0, sizeof(bool)*vm->maps.depth);
	memset(vm->vecs.mark, 0, sizeof(bool)*vm->vecs.depth);
	memset(vm->cors.mark, 0, sizeof(bool)*vm->cors.depth);
	memset(vm->data.mark, 0, sizeof(bool)*vm->data.depth);

	vm->stringsA.mark = calloc(sizeof(bool),vm->stringsA.depth);
	ensure(vm, vm->stringsA.mark, "oom");

	gc_mark_map(vm, vm->scope_core);
	gc_mark_map(vm, vm->scope_global);
	gc_mark_vec(vm, &vm->modules.entries);
	gc_mark_vec(vm, &vm->modules.names);

	for (int i = 0, l = vec_size(vm, &vm->routines); i < l; i++) {
		gc_mark_cor(vm, vec_get(vm, &vm->routines, i).cor);
	}

	for (int i = 0, l = vm->code.depth; i < l; i++) {
		gc_mark_item(vm, vm->code.cells[i].item);
	}

	for (int i = 0, l = vm->vecs.depth; i < l; i++) {
		if (vm->vecs.used[i] && !vm->vecs.mark[i]) {
			vec_t* vec = pool_ptr(vm, &vm->vecs, i);
			free(vec->items);
			pool_free(vm, &vm->vecs, vec);
		}
	}

	for (int i = 0, l = vm->maps.depth; i < l; i++) {
		if (vm->maps.used[i] && !vm->maps.mark[i]) {
			map_t* map = pool_ptr(vm, &vm->maps, i);
			free(map->keys.items);
			free(map->vals.items);
			pool_free(vm, &vm->maps, map);
		}
	}

	for (int i = 0, l = vm->data.depth; i < l; i++) {
		if (vm->data.used[i] && !vm->data.mark[i]) {
			data_t* data = pool_ptr(vm, &vm->data, i);
			pool_free(vm, &vm->data, data);
		}
	}

	for (int i = 0, l = vm->cors.depth; i < l; i++) {
		if (vm->cors.used[i] && !vm->cors.mark[i]) {
			cor_t* cor = pool_ptr(vm, &vm->cors, i);
			pool_free(vm, &vm->cors, cor);
		}
	}

	for (int l = 0, r = 0, d = vm->stringsA.depth; r < d; ) {
		if (!vm->stringsA.mark[r]) {
			free(vm->stringsA.cells[r++]);
			vm->stringsA.depth--;
		} else {
			vm->stringsA.cells[l++] = vm->stringsA.cells[r++];
		}
	}

	free(vm->stringsA.mark);
	vm->stringsA.mark = NULL;
}

static vec_t* vec_allot(rela_vm* vm) {
	return pool_alloc(vm, &vm->vecs);
}

static map_t* map_allot(rela_vm* vm) {
	return pool_alloc(vm, &vm->maps);
}

static data_t* data_allot(rela_vm* vm) {
	return pool_alloc(vm, &vm->data);
}

static cor_t* cor_allot(rela_vm* vm) {
	return pool_alloc(vm, &vm->cors);
}

static size_t vec_size(rela_vm* vm, vec_t* vec) {
	return vec ? vec->count: 0;
}

static item_t* vec_ins(rela_vm* vm, vec_t* vec, int index) {
	assert(index >= 0 && index <= vec->count);
	assert(vec->count <= vec->buffer);

	if (!vec->items || vec->count == vec->buffer) {
		vec->buffer = vec->buffer ? vec->buffer*2: 8;
		vec->items = realloc(vec->items, sizeof(item_t) * vec->buffer);
		ensure(vm, vec->items, "oom");
	}

	if (index < vec->count)
		memmove(&vec->items[index+1], &vec->items[index], (vec->count - index) * sizeof(item_t));

	vec->count++;
	vec->items[index].type = NIL;

	return &vec->items[index];
}

static void vec_del(rela_vm* vm, vec_t* vec, int index) {
	assert(index >= 0 && index < vec->count);
	memmove(&vec->items[index], &vec->items[index+1], (vec->count - index - 1) * sizeof(item_t));
	vec->count--;
}

static void vec_push(rela_vm* vm, vec_t* vec, item_t item) {
	vec_ins(vm, vec, vec->count)[0] = item;
}

static void vec_push_allot(rela_vm* vm, vec_t** vec, item_t item) {
	if (!*vec) *vec = vec_allot(vm);
	vec_ins(vm, *vec, (*vec)->count)[0] = item;
}

static item_t vec_pop(rela_vm* vm, vec_t* vec) {
	ensure(vm, vec->count > 0, "vec_pop underflow");
	return vec->items[--vec->count];
}

static item_t vec_top(rela_vm* vm, vec_t* vec) {
	ensure(vm, vec->count > 0, "vec_top underflow");
	return vec->items[vec->count-1];
}

static item_t* vec_cell(rela_vm* vm, vec_t* vec, int index) {
	if (index < 0) index = vec->count + index;
	ensure(vm, index >= 0 && index < vec->count, "vec_cell out of bounds");
	return &vec->items[index];
}

static item_t vec_get(rela_vm* vm, vec_t* vec, int index) {
	return vec_cell(vm, vec, index)[0];
}

static int vec_lower_bound(rela_vm* vm, vec_t* vec, item_t key) {
	int size = vec->count;
	if (!size) return 0;

	if (size < 16) {
		int i = 0;
		while (i < size && less(vm, vec->items[i], key)) i++;
		return i;
	}

	int lower = 0;
	int upper = size-1;

	while (lower <= upper) {
		int i = (int)floor((float)(lower + upper) / 2.0f);
		if (equal(vm, vec->items[i], key)) return i;
		if (less(vm, vec->items[i], key)) lower = i+1; else upper = i-1;
	}
	return lower;
}

static void vec_swap(rela_vm* vm, vec_t* vec, int ai, int bi) {
	item_t a = vec_get(vm, vec, ai);
	item_t b = vec_get(vm, vec, bi);
	vec_cell(vm, vec, ai)[0] = b;
	vec_cell(vm, vec, bi)[0] = a;
}

static void vec_sort(rela_vm* vm, vec_t* vec, int low, int high) {
	if (low < high) {
		// todo median of 3
		item_t pivot = vec_get(vm, vec, low+(high-low)/2);
		int left = low;
		int right = high;
		while (left <= right) {
			while (less(vm, vec_get(vm, vec, left), pivot)) left++;
			while (less(vm, pivot, vec_get(vm, vec, right))) right--;
			if (left <= right) {
				vec_swap(vm, vec, left, right);
				left++;
				right--;
			}
		}
		vec_sort(vm, vec, low, right);
		vec_sort(vm, vec, left, high);
	}
}

static void vec_clear(rela_vm* vm, vec_t* vec) {
	vec->count = 0;
}

static int map_lower_bound(rela_vm* vm, map_t* map, item_t key) {
	return vec_lower_bound(vm, &map->keys, key);
}

static item_t* map_ref(rela_vm* vm, map_t* map, item_t key) {
	int i = map_lower_bound(vm, map, key);
	return (i < vec_size(vm, &map->keys) && equal(vm, vec_get(vm, &map->keys, i), key))
		? vec_cell(vm, &map->vals, i): NULL;
}

static bool map_get(rela_vm* vm, map_t* map, item_t key, item_t* val) {
	item_t* item = map_ref(vm, map, key);
	if (!item) return false;
	assert(item->type != NIL);
	*val = *item;
	return true;
}

static void map_clr(rela_vm* vm, map_t* map, item_t key) {
	int i = map_lower_bound(vm, map, key);
	if (i < vec_size(vm, &map->keys) && equal(vm, vec_get(vm, &map->keys, i), key)) {
		vec_del(vm, &map->keys, i);
		vec_del(vm, &map->vals, i);
	}
}

static void map_set(rela_vm* vm, map_t* map, item_t key, item_t val) {
	if (val.type == NIL) {
		map_clr(vm, map, key);
		return;
	}
	int i = map_lower_bound(vm, map, key);
	if (i < vec_size(vm, &map->keys) && equal(vm, vec_get(vm, &map->keys, i), key)) {
		vec_cell(vm, &map->vals, i)[0] = val;
	}
	else {
		vec_ins(vm, &map->keys, i)[0] = key;
		vec_ins(vm, &map->vals, i)[0] = val;
	}
}

static int str_lower_bound(rela_vm* vm, string_region_t* region, const char *str) {
	ensure(vm, str, "str_lower_bound() null string");
	int index = 0;
	if (region->depth) {
		int lower = 0;
		int upper = region->depth-1;
		while (lower <= upper) {
			int i = (int)floor((float)(lower + upper) / 2.0f);
			int c = strcmp(region->cells[i], str);
			if (c == 0) { index = i; break; }
			if (c < 0) lower = i+1; else upper = i-1;
			index = lower;
		}
	}
	assert(index >= 0 && index <= region->depth);
	return index;
}

static const char* strintern(rela_vm* vm, const char* str) {
	int indexB = str_lower_bound(vm, &vm->stringsB, str);

	if (indexB < vm->stringsB.depth && vm->stringsB.cells[indexB] == str) {
		return str;
	}

	if (indexB < vm->stringsB.depth && strcmp(vm->stringsB.cells[indexB], str) == 0) {
		return vm->stringsB.cells[indexB];
	}

	int index = str_lower_bound(vm, &vm->stringsA, str);

	if (index < vm->stringsA.depth && vm->stringsA.cells[index] == str) {
		return str;
	}

	if (index < vm->stringsA.depth && strcmp(vm->stringsA.cells[index], str) == 0) {
		return vm->stringsA.cells[index];
	}

	int len = strlen(str);
	char* cpy = calloc(len+1,1);
	ensure(vm, cpy, "oom");
	memmove(cpy, str, len);

	vm->stringsA.cells = realloc(vm->stringsA.cells, ++vm->stringsA.depth * sizeof(char*));
	ensure(vm, vm->stringsA.cells, "oom");

	if (index < vm->stringsA.depth-1) {
		memmove(&vm->stringsA.cells[index+1], &vm->stringsA.cells[index], (vm->stringsA.depth - index - 1) * sizeof(char*));
	}
	vm->stringsA.cells[index] = cpy;
	return cpy;
}

static const char* substr(rela_vm* vm, const char *start, int off, int len) {
	char buf[STRBUF];
	ensure(vm, len < STRBUF, "substr max len exceeded (%d bytes)", STRBUF-1);
	memcpy(buf, start+off, len);
	buf[len] = 0;
	return strintern(vm,buf);
}

static const char* strliteral(rela_vm* vm, const char *str, char **err) {
	char res[STRBUF];
	char *rp = res, *sp = (char*)str+1, *ep = res + STRBUF;

	while (sp && *sp) {
		int c = *sp++;
		if (c == '"') break;

		if (c == '\\') {
			c = *sp++;
				 if (c == 'a') c = '\a';
			else if (c == 'b') c = '\b';
			else if (c == 'f') c = '\f';
			else if (c == 'n') c = '\n';
			else if (c == 'r') c = '\r';
			else if (c == 't') c = '\t';
			else if (c == 'v') c = '\v';
		}
		*rp++ = c;
		if (rp >= ep) rp = ep-1;
	}
	*rp = 0;
	ensure(vm, rp <= ep, "strliteral max length exceeded (%d bytes)", STRBUF-1);

	if (err) *err = sp;
	return strintern(vm,res);
}

static int str_skip(const char *source, strcb cb) {
	int offset = 0;
	while (source[offset] && cb(source[offset])) offset++;
	return offset;
}

static int str_scan(const char *source, strcb cb) {
	int offset = 0;
	while (source[offset] && !cb(source[offset])) offset++;
	return offset;
}

static void reset(rela_vm* vm) {
	vm->scope_global = NULL;
	vm->routines.count = 0;
	vm->routine = NULL;
	free(vm->cache.cfunc);
	vm->cache.cfunc = NULL;
	gc(vm);
}

static item_t nil(rela_vm* vm) {
	return (item_t){.type = NIL, .str = NULL};
}

static item_t integer(rela_vm* vm, int64_t i) {
	return (item_t){.type = INTEGER, .inum = i};
}

static item_t number(rela_vm* vm, double d) {
	return (item_t){.type = FLOAT, .fnum = d};
}

static item_t string(rela_vm* vm, const char* s) {
	return (item_t){.type = STRING, .str = strintern(vm, s) };
}

static bool truth(rela_vm* vm, item_t a) {
	if (a.type == INTEGER) return a.inum != 0;
	if (a.type == FLOAT) return a.fnum > 0+DBL_EPSILON || a.fnum < 0-DBL_EPSILON;
	if (a.type == STRING) return a.str && a.str[0];
	if (a.type == BOOLEAN) return a.flag;
	if (a.type == VECTOR) return vec_size(vm, a.vec) > 0;
	if (a.type == MAP) return vec_size(vm, &a.map->keys) > 0;
	if (a.type == SUBROUTINE) return true;
	if (a.type == COROUTINE) return true;
	if (a.type == CALLBACK) return true;
	if (a.type == USERDATA) return true;
	if (a.type == NODE) return true;
	return false;
}

static bool meta_get(rela_vm* vm, item_t meta, const char* name, item_t *func) {
	if (meta.type == MAP) return map_get(vm, meta.map, string(vm, name), func);
	if (meta.type == SUBROUTINE || meta.type == CALLBACK) {
		item_t key = string(vm, name);
		method(vm, meta, 1, &key, 1, func);
		return func->type != NIL;
	}
	return false;
}

static bool equal(rela_vm* vm, item_t a, item_t b) {
	item_t func;
	item_t argv[2] = {a,b};
	item_t retv[1];

	if (a.type == b.type) {
		if (a.type == INTEGER) return a.inum == b.inum;
		if (a.type == FLOAT) return fabs(a.fnum - b.fnum) < DBL_EPSILON*10;
		if (a.type == STRING) return a.str == b.str; // .str must use strintern
		if (a.type == BOOLEAN) return a.flag == b.flag;
		if (a.type == VECTOR && meta_get(vm, a.vec->meta, "==", &func)) {
			method(vm, func, 2, argv, 1, retv);
			return truth(vm, retv[0]);
		}
		if (a.type == VECTOR && a.vec == b.vec) return true;
		if (a.type == VECTOR && vec_size(vm, a.vec) == vec_size(vm, b.vec)) {
			for (int i = 0, l = vec_size(vm, a.vec); i < l; i++) {
				if (!equal(vm, vec_get(vm, a.vec, i), vec_get(vm, b.vec, i))) return false;
			}
			return true;
		}
		if (a.type == MAP && meta_get(vm, a.map->meta, "==", &func)) {
			method(vm, func, 2, argv, 1, retv);
			return truth(vm, retv[0]);
		}
		if (a.type == MAP && a.map == b.map) return true;
		if (a.type == MAP && vec_size(vm, &a.map->keys) == vec_size(vm, &b.map->keys)) {
			for (int i = 0, l = vec_size(vm, &a.map->keys); i < l; i++) {
				if (!equal(vm, vec_get(vm, &a.map->keys, i), vec_get(vm, &b.map->keys, i))) return false;
				if (!equal(vm, vec_get(vm, &a.map->vals, i), vec_get(vm, &b.map->vals, i))) return false;
			}
			return true;
		}
		if (a.type == SUBROUTINE) return a.sub == b.sub;
		if (a.type == COROUTINE) return a.cor == b.cor;
		if (a.type == USERDATA && meta_get(vm, a.data->meta, "==", &func)) {
			method(vm, func, 2, argv, 1, retv);
			return truth(vm, retv[0]);
		}
		if (a.type == USERDATA) return a.data == b.data;
		if (a.type == NODE) return a.node == b.node;
		if (a.type == NIL) return true;
	}
	return false;
}

static bool less(rela_vm* vm, item_t a, item_t b) {
	item_t func;
	item_t argv[2] = {a,b};
	item_t retv[1];

	if (a.type == b.type) {
		if (a.type == INTEGER) return a.inum < b.inum;
		if (a.type == FLOAT) return a.fnum < b.fnum;
		if (a.type == STRING) return a.str != b.str && strcmp(a.str, b.str) < 0;
		if (a.type == VECTOR && meta_get(vm, a.vec->meta, "<", &func)) {
			method(vm, func, 2, argv, 1, retv);
			return truth(vm, retv[0]);
		}
		if (a.type == VECTOR) return vec_size(vm, a.vec) < vec_size(vm, b.vec);
		if (a.type == MAP && meta_get(vm, a.map->meta, "<", &func)) {
			method(vm, func, 2, argv, 1, retv);
			return truth(vm, retv[0]);
		}
		if (a.type == MAP) return vec_size(vm, &a.map->keys) < vec_size(vm, &b.map->keys);
		if (a.type == USERDATA && meta_get(vm, a.data->meta, "<", &func)) {
			method(vm, func, 2, argv, 1, retv);
			return truth(vm, retv[0]);
		}
	}
	return false;
}

static int count(rela_vm* vm, item_t a) {
	item_t func;
	item_t argv[1] = {a};
	item_t retv[1];

	if (a.type == INTEGER) return a.inum;
	if (a.type == FLOAT) return floor(a.fnum);
	if (a.type == STRING) return strlen(a.str);
	if (a.type == VECTOR) return vec_size(vm, a.vec);
	if (a.type == MAP) return vec_size(vm, &a.map->keys);
	if (a.type == USERDATA && meta_get(vm, a.data->meta, "#", &func)) {
		method(vm, func, 1, argv, 1, retv);
		ensure(vm, retv[0].type == INTEGER, "meta method # should return an integer");
		return retv[0].inum;
	}
	return 0;
}

static item_t add(rela_vm* vm, item_t a, item_t b) {
	item_t func;
	item_t argv[2] = {a,b};
	item_t retv[1];

	if (a.type == INTEGER && b.type == INTEGER) return (item_t){.type = INTEGER, .inum = a.inum + b.inum};
	if (a.type == INTEGER && b.type == FLOAT) return (item_t){.type = INTEGER, .inum = a.inum + b.fnum};
	if (a.type == FLOAT && b.type == INTEGER) return (item_t){.type = FLOAT, .fnum = a.fnum + b.inum};
	if (a.type == FLOAT && b.type == FLOAT) return (item_t){.type = FLOAT, .fnum = a.fnum + b.fnum};
	if (a.type == VECTOR && meta_get(vm, a.vec->meta, "+", &func)) {
		method(vm, func, 2, argv, 1, retv);
		return retv[0];
	}
	if (a.type == MAP && meta_get(vm, a.map->meta, "+", &func)) {
		method(vm, func, 2, argv, 1, retv);
		return retv[0];
	}
	if (a.type == USERDATA && meta_get(vm, a.data->meta, "+", &func)) {
		method(vm, func, 2, argv, 1, retv);
		return retv[0];
	}
	return nil(vm);
}

static item_t multiply(rela_vm* vm, item_t a, item_t b) {
	item_t func;
	item_t argv[2] = {a,b};
	item_t retv[1];

	if (a.type == INTEGER && b.type == INTEGER) return (item_t){.type = INTEGER, .inum = a.inum * b.inum};
	if (a.type == INTEGER && b.type == FLOAT) return (item_t){.type = INTEGER, .inum = a.inum * b.fnum};
	if (a.type == FLOAT && b.type == INTEGER) return (item_t){.type = FLOAT, .fnum = a.fnum * b.inum};
	if (a.type == FLOAT && b.type == FLOAT) return (item_t){.type = FLOAT, .fnum = a.fnum * b.fnum};
	if (a.type == VECTOR && meta_get(vm, a.vec->meta, "*", &func)) {
		method(vm, func, 2, argv, 1, retv);
		return retv[0];
	}
	if (a.type == MAP && meta_get(vm, a.map->meta, "*", &func)) {
		method(vm, func, 2, argv, 1, retv);
		return retv[0];
	}
	if (a.type == USERDATA && meta_get(vm, a.data->meta, "*", &func)) {
		method(vm, func, 2, argv, 1, retv);
		return retv[0];
	}
	return nil(vm);
}

static item_t divide(rela_vm* vm, item_t a, item_t b) {
	item_t func;
	item_t argv[2] = {a,b};
	item_t retv[1];

	if (a.type == INTEGER && b.type == INTEGER) return (item_t){.type = INTEGER, .inum = a.inum / b.inum};
	if (a.type == INTEGER && b.type == FLOAT) return (item_t){.type = INTEGER, .inum = a.inum / b.fnum};
	if (a.type == FLOAT && b.type == INTEGER) return (item_t){.type = FLOAT, .fnum = a.fnum / b.inum};
	if (a.type == FLOAT && b.type == FLOAT) return (item_t){.type = FLOAT, .fnum = a.fnum / b.fnum};
	if (a.type == VECTOR && meta_get(vm, a.vec->meta, "/", &func)) {
		method(vm, func, 2, argv, 1, retv);
		return retv[0];
	}
	if (a.type == MAP && meta_get(vm, a.map->meta, "/", &func)) {
		method(vm, func, 2, argv, 1, retv);
		return retv[0];
	}
	if (a.type == USERDATA && meta_get(vm, a.data->meta, "/", &func)) {
		method(vm, func, 2, argv, 1, retv);
		return retv[0];
	}
	return nil(vm);
}

static const char* tmptext(rela_vm* vm, item_t a, char* tmp, int size) {
	if (a.type == STRING) return a.str;

	item_t func;
	item_t argv[1] = {a};
	item_t retv[1];

	assert(a.type >= 0 && a.type < TYPES);

	if (a.type == NIL) snprintf(tmp, size, "nil");
	if (a.type == INTEGER) snprintf(tmp, size, "%ld", a.inum);
	if (a.type == FLOAT) snprintf(tmp, size, "%f", a.fnum);
	if (a.type == BOOLEAN) snprintf(tmp, size, "%s", a.flag ? "true": "false");
	if (a.type == SUBROUTINE) snprintf(tmp, size, "%s(%d)", type_names[a.type], a.sub);
	if (a.type == COROUTINE) snprintf(tmp, size, "%s", type_names[a.type]);
	if (a.type == CALLBACK) snprintf(tmp, size, "%s", type_names[a.type]);
	if (a.type == NODE) snprintf(tmp, size, "%s", type_names[a.type]);

	if (a.type == USERDATA && meta_get(vm, a.data->meta, "$", &func)) {
		method(vm, func, 1, argv, 1, retv);
		ensure(vm, retv[0].type == STRING, "$ should return a string");
		return retv[0].str;
	}

	if (a.type == USERDATA) snprintf(tmp, size, "%s", type_names[a.type]);

	char subtmpA[STRTMP];
	char subtmpB[STRTMP];

	if (a.type == VECTOR && meta_get(vm, a.vec->meta, "$", &func)) {
		method(vm, func, 1, argv, 1, retv);
		ensure(vm, retv[0].type == STRING, "$ should return a string");
		return retv[0].str;
	}

	if (a.type == VECTOR) {
		int len = snprintf(tmp, size, "[");
		for (int i = 0, l = vec_size(vm, a.vec); len < size && i < l; i++) {
			if (len < size) len += snprintf(tmp+len, size-len, "%s",
				tmptext(vm, vec_get(vm, a.vec, i), subtmpA, sizeof(subtmpA)));
			if (len < size && i < l-1) len += snprintf(tmp+len, size-len, ", ");
		}
		if (len < size) len += snprintf(tmp+len, size-len, "]");
	}

	if (a.type == MAP && meta_get(vm, a.map->meta, "$", &func)) {
		method(vm, func, 1, argv, 1, retv);
		ensure(vm, retv[0].type == STRING, "$ should return a string");
		return retv[0].str;
	}

	if (a.type == MAP) {
		int len = snprintf(tmp, size, "{");
		for (int i = 0, l = vec_size(vm, &a.map->keys); len < size && i < l; i++) {
			if (len < size) len += snprintf(tmp+len, size-len, "%s = %s",
				tmptext(vm, vec_get(vm, &a.map->keys, i), subtmpA, sizeof(subtmpA)),
				tmptext(vm, vec_get(vm, &a.map->vals, i), subtmpB, sizeof(subtmpB))
			);
			if (len < size && i < l-1) len += snprintf(tmp+len, size-len, ", ");
		}
		if (len < size) len += snprintf(tmp+len, size-len, "}");
	}

	assert(tmp[0]);
	return tmp;
}

static code_t* compiled(rela_vm* vm, int offset) {
	return &vm->code.cells[offset < 0 ? vm->code.depth+offset: offset];
}

static int compile(rela_vm* vm, int op, item_t item) {
	vm->code.cells = realloc(vm->code.cells, sizeof(code_t)*(vm->code.depth+10));
	ensure(vm, vm->code.cells, "oom");

	// peephole
	if (vm->code.depth > 0) {
		code_t* back1 = compiled(vm, -1);
		code_t* back2 = vm->code.depth > 1 ? compiled(vm, -2): NULL;
		code_t* back3 = vm->code.depth > 2 ? compiled(vm, -3): NULL;

		// remove implicit return block dead code
		if (op == OP_CLEAN && back1->op == OP_CLEAN) return vm->code.depth-1;
		if (op == OP_CLEAN && back1->op == OP_RETURN) return vm->code.depth-1;
		if (op == OP_RETURN && back1->op == OP_RETURN) return vm->code.depth-1;

		// lit,find -> fname (duplicate vars, single lookup array[#array])
		if (op == OP_FIND && back1->op == OP_LIT) {
			for(;;) {
				// fname,copies=n (n+1 stack items: one original + n copies)
				if (back2 && back3 && back2->op == OPP_COPIES && back3->op == OPP_FNAME && equal(vm, back1->item, back3->item)) {
					back2->item.inum++;
					vm->code.depth--;
					break;
				}
				// fname,copies=1 (two stack items: one original + one copy)
				if (back2 && back2->op == OPP_FNAME && equal(vm, back1->item, back2->item)) {
					back1->op = OPP_COPIES;
					back1->item = integer(vm, 1);
					break;
				}
				back1->op = OPP_FNAME;
				break;
			}
			return vm->code.depth-1;
		}

		// lit,get -> gname
		if (op == OP_GET && back1->op == OP_LIT) {
			back1->op = OPP_GNAME;
			return vm->code.depth-1;
		}

		// fname,call -> cfunc
		if (op == OP_CALL && back1->op == OPP_FNAME) {
			back1->op = OPP_CFUNC;
			return vm->code.depth-1;
		}

		// fname[a],op,lit[a],assign0 -> update[a],op (var=var+n, var=var*n)
		if (op == OP_ASSIGN && item.type == INTEGER && item.inum == 0 && back1->op == OP_LIT && back2 && back3) {
			bool sameName = back3->op == OPP_FNAME && equal(vm, back1->item, back3->item);
			bool simpleOp = back2->op == OPP_ADD_LIT || back2->op == OPP_MUL_LIT;
			if (sameName && simpleOp) {
				vm->code.cells[vm->code.depth-3] = (code_t){.op = OPP_UPDATE, .item = back1->item};
				vm->code.depth--;
				return vm->code.depth-1;
			}
		}

		// mark,update,op,limit -> update,op
		if (op == OP_LIMIT && item.type == INTEGER && item.inum == 0 && back2 && back3) {
			if (back3->op == OP_MARK && back2->op == OPP_UPDATE) {
				vm->code.cells[vm->code.depth-3] = *back2;
				vm->code.cells[vm->code.depth-2] = *back1;
				vm->code.depth--;
				return vm->code.depth-1;
			}
		}

		// lit,assign0 -> assignl
		if (op == OP_ASSIGN && item.type == INTEGER && item.inum == 0 && back1->op == OP_LIT) {
			back1->op = OPP_ASSIGNL;
			return vm->code.depth-1;
		}

		// mark,lit,assignl,limit0 -> lit,assignp (map { litkey = litval }, var = lit)
		if (op == OP_LIMIT && item.type == INTEGER && item.inum == 0 && back2 && back3) {
			if (back3->op == OP_MARK && back2->op == OP_LIT && back1->op == OPP_ASSIGNL) {
				item_t key = back1->item;
				vm->code.cells[vm->code.depth-3] = vm->code.cells[vm->code.depth-2];
				vm->code.cells[vm->code.depth-2] = (code_t){.op = OPP_ASSIGNP, .item = key};
				vm->code.depth--;
				return vm->code.depth-1;
			}
		}

		// lit,neg
		if (op == OP_NEG && back1->op == OP_LIT && back1->item.type == INTEGER) {
			back1->item.inum = -back1->item.inum;
			return vm->code.depth-1;
		}

		// lit,neg
		if (op == OP_NEG && back1->op == OP_LIT && back1->item.type == FLOAT) {
			back1->item.fnum = -back1->item.fnum;
			return vm->code.depth-1;
		}

		// lit,add
		if (op == OP_ADD && back1->op == OP_LIT) {
			back1->op = OPP_ADD_LIT;
			return vm->code.depth-1;
		}

		// lit,mul
		if (op == OP_MUL && back1->op == OP_LIT) {
			back1->op = OPP_MUL_LIT;
			return vm->code.depth-1;
		}
	}

	vm->code.cells[vm->code.depth++] = (code_t){.op = op, .item = item};
	return vm->code.depth-1;
}

static item_t* stack_ref(rela_vm* vm, cor_t* routine, int index) {
	return &routine->stack.cells[index];
}

static item_t* stack_cell(rela_vm* vm, int index) {
	if (index < 0) index = vm->routine->stack.depth + index;
	assert(index >= 0 && index < vm->routine->stack.depth);
	return stack_ref(vm, vm->routine, index);
}

static int depth(rela_vm* vm) {
	int base = vm->routine->marks.depth ? vm->routine->marks.cells[vm->routine->marks.depth-1]: 0;
	return vm->routine->stack.depth - base;
}

static item_t* item(rela_vm* vm, int i) {
	int base = vm->routine->marks.cells[vm->routine->marks.depth-1];
	int index = i >= 0 ? base+i: i;
	return stack_ref(vm, vm->routine, index);
}

static void push(rela_vm* vm, item_t item) {
	assert(vm->routine->stack.depth < STACK);
	int index = vm->routine->stack.depth++;
	*stack_ref(vm, vm->routine, index) = item;
}

static item_t pop(rela_vm* vm) {
	assert(vm->routine->stack.depth > 0);
	int index = --vm->routine->stack.depth;
	return *stack_ref(vm, vm->routine, index);
}

static item_t top(rela_vm* vm) {
	assert(vm->routine->stack.depth > 0);
	int index = vm->routine->stack.depth-1;
	return *stack_ref(vm, vm->routine, index);
}

static void opush(rela_vm* vm, item_t item) {
	assert(vm->routine->other.depth < STACK);
	vm->routine->other.cells[vm->routine->other.depth++] = item;
}

static item_t opop(rela_vm* vm) {
	assert(vm->routine->other.depth > 0);
	return vm->routine->other.cells[--vm->routine->other.depth];
}

static item_t otop(rela_vm* vm) {
	assert(vm->routine->other.depth > 0);
	return vm->routine->other.cells[vm->routine->other.depth-1];
}

static item_t pop_type(rela_vm* vm, int type) {
	item_t i = pop(vm);
	if (type == FLOAT && i.type == INTEGER) return number(vm, i.inum);
	ensure(vm, i.type == type, "pop_type expected %s, found %s", type_names[type], type_names[i.type]);
	return i;
}

static int isnamefirst(int c) {
	return isalpha(c) || c == '_';
}

static int isname(int c) {
	return isnamefirst(c) || isdigit(c);
}

static int islf(int c) {
	return c == '\n';
}

static int skip_gap(const char *source) {
	int offset = 0;
	while (source[offset]) {
		if (isspace(source[offset])) {
			offset += str_skip(&source[offset], isspace);
			continue;
		}
		if (source[offset] == '/' && source[offset+1] == '/') {
			while (source[offset] == '/' && source[offset+1] == '/') {
				offset += str_scan(&source[offset], islf);
				offset += str_skip(&source[offset], islf);
			}
			continue;
		}
		break;
	}
	return offset;
}

static bool peek(const char* source, const char* name) {
	while (*source && *name && *source == *name) { ++source; ++name; }
	return *source && !*name && !isname(*source);
}

static node_t* node_allot(rela_vm* vm) {
	vm->nodes.cells = realloc(vm->nodes.cells, (vm->nodes.depth+1)*sizeof(node_t*));
	ensure(vm, vm->nodes.cells, "oom");
	vm->nodes.cells[vm->nodes.depth] = calloc(sizeof(node_t),1);
	ensure(vm, vm->nodes.cells[vm->nodes.depth], "oom");
	return vm->nodes.cells[vm->nodes.depth++];
}

static int parse_block(rela_vm* vm, const char *source, node_t *node) {
	int length = 0;
	int found_end = 0;
	int offset = skip_gap(source);

	// don't care about this, but Lua habits...
	if (peek(&source[offset], "do")) offset += 2;

	while (source[offset]) {
		if ((length = skip_gap(&source[offset])) > 0) {
			offset += length;
			continue;
		}
		if (peek(&source[offset], "end")) {
			offset += 3;
			found_end = 1;
			break;
		}

		offset += parse(vm, &source[offset], RESULTS_DISCARD, PARSE_COMMA|PARSE_ANDOR);
		vec_push_allot(vm, &node->vals, pop(vm));
	}

	ensure(vm, found_end, "expected keyword 'end': %s", &source[offset]);
	return offset;
}

static int parse_branch(rela_vm* vm, const char *source, node_t *node) {
	int length = 0;
	int found_else = 0;
	int found_end = 0;
	int offset = skip_gap(source);

	// don't care about this, but Lua habits...
	if (peek(&source[offset], "then")) offset += 4;

	while (source[offset]) {
		if ((length = skip_gap(&source[offset])) > 0) {
			offset += length;
			continue;
		}

		if (peek(&source[offset], "else")) {
			offset += 4;
			found_else = 1;
			break;
		}

		if (peek(&source[offset], "end")) {
			offset += 3;
			found_end = 1;
			break;
		}

		offset += parse(vm, &source[offset], RESULTS_DISCARD, PARSE_COMMA|PARSE_ANDOR);
		vec_push_allot(vm, &node->vals, pop(vm));
	}

	if (found_else) {
		while (source[offset]) {
			if ((length = skip_gap(&source[offset])) > 0) {
				offset += length;
				continue;
			}

			if (peek(&source[offset], "end")) {
				offset += 3;
				found_end = 1;
				break;
			}

			offset += parse(vm, &source[offset], RESULTS_DISCARD, PARSE_COMMA|PARSE_ANDOR);
			vec_push_allot(vm, &node->keys, pop(vm));
		}
	}

	ensure(vm, found_else || found_end, "expected keyword 'else' or 'end': %s", &source[offset]);

	// last NODE_MULTI in the block returns its value (ternary operator equiv)
	if (vec_size(vm, node->vals))
		vec_cell(vm, node->vals, vec_size(vm, node->vals)-1)->node->results = RESULTS_FIRST;
	if (vec_size(vm, node->keys))
		vec_cell(vm, node->keys, vec_size(vm, node->keys)-1)->node->results = RESULTS_FIRST;

	return offset;
}

static int parse_arglist(rela_vm* vm, const char *source) {
	int mark = depth(vm);
	int offset = skip_gap(source);

	if (source[offset] == '(') {
		offset++;
		offset += skip_gap(&source[offset]);

		if (source[offset] != ')') {
			offset += parse(vm, &source[offset], RESULTS_ALL, PARSE_COMMA|PARSE_ANDOR);
			offset += skip_gap(&source[offset]);
		}

		ensure(vm, source[offset] == ')', "expected closing paren: %s", &source[offset]);
		offset++;
	}
	else {
		offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_COMMA|PARSE_ANDOR);
	}

	if (depth(vm) == mark)
		push(vm, nil(vm));

	return offset;
}

// Since anything can return values, including some control structures, there is no
// real difference between statements and expressions. Just nodes in the parse tree.
static int parse_node(rela_vm* vm, const char *source) {
	int length = 0;
	int offset = skip_gap(source);

	modifier_t* modifier = NULL;

	for (int i = 0; i < sizeof(modifiers) / sizeof(modifier_t); i++) {
		modifier_t *mod = &modifiers[i];
		if (!strncmp(mod->name, &source[offset], strlen(mod->name))) {
			modifier = mod;
			break;
		}
	}

	if (modifier) {
		offset += strlen(modifier->name);
		offset += parse_node(vm, &source[offset]);
		node_t* outer = node_allot(vm);
		outer->type = NODE_OPCODE;
		outer->opcode = modifier->opcode;
		outer->single = true;
		outer->args = pop(vm).node;
		push(vm, (item_t){.type = NODE, .node = outer});
		return offset;
	}

	node_t *node = node_allot(vm);

	if (isnamefirst(source[offset])) {
		node->type = NODE_NAME;
		length = str_skip(&source[offset], isname);

		int have_keyword = 0;

		for (int i = 0; i < sizeof(keywords) / sizeof(keyword_t) && !have_keyword; i++) {
			keyword_t *keyword = &keywords[i];

			if (peek(&source[offset], keyword->name)) {
				node->type = NODE_OPCODE;
				node->opcode = keyword->opcode;
				have_keyword = 1;
				offset += length;
			}
		}

		if (!have_keyword) {
			if (peek(&source[offset], "if")) {
				offset += 2;
				node->type = NODE_IF;
				node->control = false; // true breaks ternary

				// conditions
				offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_COMMA|PARSE_ANDOR);
				node->args = pop(vm).node;

				// block, optional else
				offset += parse_branch(vm, &source[offset], node);
			}
			else
			if (peek(&source[offset], "while")) {
				offset += 5;
				node->type = NODE_WHILE;
				node->control = true;

				// conditions
				offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_COMMA|PARSE_ANDOR);
				node->args = pop(vm).node;

				// block
				offset += parse_block(vm, &source[offset], node);
			}
			else
			if (peek(&source[offset], "for")) {
				offset += 3;
				node->type = NODE_FOR;
				node->control = true;

				// [key[,val]] local variable names
				offset += skip_gap(&source[offset]);
				if (!peek(&source[offset], "in")) {
					ensure(vm, isnamefirst(source[offset]), "expected for [<key>,]val in iterable: %s", &source[offset]);

					length = str_skip(&source[offset], isname);
					vec_push_allot(vm, &node->keys, string(vm, substr(vm, &source[offset], 0, length)));
					offset += length;

					offset += skip_gap(&source[offset]);
					if (source[offset] == ',') {
						offset++;
						length = str_skip(&source[offset], isname);
						vec_push_allot(vm, &node->keys, string(vm, substr(vm, &source[offset], 0, length)));
						offset += length;
					}
				}

				offset += skip_gap(&source[offset]);
				ensure(vm, peek(&source[offset], "in"), "expected for [<key>,]val in iterable: %s", &source[offset]);
				offset += 2;

				// iterable
				offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_COMMA|PARSE_ANDOR);
				node->args = pop(vm).node;

				// block
				offset += parse_block(vm, &source[offset], node);
			}
			else
			if (peek(&source[offset], "function")) {
				offset += 8;
				node->type = NODE_FUNCTION;
				node->control = true;

				ensure(vm, vm->fpath.depth < PATH, "reached function nest limit(%d)", PATH);

				memmove(node->fpath.ids, vm->fpath.ids, vm->fpath.depth*sizeof(int));
				node->fpath.depth = vm->fpath.depth;
				node->fpath.id = ++vm->fpath.id;
				vm->fpath.ids[vm->fpath.depth] = node->fpath.id;
				vm->fpath.depth++;

				offset += skip_gap(&source[offset]);

				// optional function name
				if (isnamefirst(source[offset])) {
					length = str_skip(&source[offset], isname);
					node->item = string(vm, substr(vm, &source[offset], 0, length));
					offset += length;
				}

				offset += skip_gap(&source[offset]);

				// argument locals list
				if (source[offset] == '(') {
					offset++;

					while (source[offset]) {
						if ((length = skip_gap(&source[offset])) > 0) {
							offset += length;
							continue;
						}
						if (source[offset] == ',') {
							offset++;
							continue;
						}
						if (source[offset] == ')') {
							offset++;
							break;
						}

						ensure(vm, isnamefirst(source[offset]), "expected parameter: %s", &source[offset]);
						length = str_skip(&source[offset], isname);

						node_t *param = node_allot(vm);
						param->type = NODE_NAME;
						param->item = string(vm, substr(vm, &source[offset], 0, length));
						vec_push_allot(vm, &node->keys, (item_t){.type = NODE, .node = param});

						offset += length;
					}
				}

				// block
				offset += parse_block(vm, &source[offset], node);

				assert(vm->fpath.depth > 0);
				vm->fpath.depth--;
			}
			else
			if (peek(&source[offset], "return")) {
				offset += 6;
				node->type = NODE_RETURN;
				node->control = true;

				offset += skip_gap(&source[offset]);

				if (!peek(&source[offset], "end")) {
					offset += parse(vm, &source[offset], RESULTS_ALL, PARSE_COMMA|PARSE_ANDOR);
					node->args = pop(vm).node;
				}
			}
			else
			if (peek(&source[offset], "break")) {
				offset += 5;
				node->type = NODE_OPCODE;
				node->opcode = OP_BREAK;
				node->control = true;
			}
			else
			if (peek(&source[offset], "continue")) {
				offset += 8;
				node->type = NODE_OPCODE;
				node->opcode = OP_CONTINUE;
				node->control = true;
			}
			else {
				node->item = string(vm, substr(vm, &source[offset], 0, length));
				offset += length;
			}
		}
	}
	else
	if (source[offset] == '"') {
		node->type = NODE_LITERAL;
		node->single = true;
		char *end = NULL;
		node->item = string(vm, strliteral(vm, &source[offset], &end));
		offset += end - &source[offset];
	}
	else
	if (isdigit(source[offset])) {
		node->type = NODE_LITERAL;
		node->single = true;
		char *a = NULL, *b = NULL;
		int64_t i = strtoll(&source[offset], &a, 0);
		double f = strtod(&source[offset], &b);
		node->item = (b > a) ? number(vm, f): integer(vm, i);
		offset += ((b > a) ? b: a) - &source[offset];
	}
	else
	// a vector[n] or map[s] to be set/get
	if (source[offset] == '[') {
		offset++;
		node->type = NODE_VEC;
		node->single = true;
		while (source[offset] && source[offset] != ']') {
			if ((length = skip_gap(&source[offset])) > 0) {
				offset += length;
				continue;
			}
			if (source[offset] == ',') {
				offset++;
				continue;
			}
			offset += parse(vm, &source[offset], RESULTS_ALL, PARSE_ANDOR);
			vec_push_allot(vm, &node->vals, pop(vm));
		}
		ensure(vm, source[offset] == ']', "expected closing bracket: %s", &source[offset]);
		offset++;
	}
	else
	if (source[offset] == '{') {
		offset++;
		node->type = NODE_MAP;
		node->single = true;

		while (source[offset] && source[offset] != '}') {
			if ((length = skip_gap(&source[offset])) > 0) {
				offset += length;
				continue;
			}
			if (source[offset] == ',') {
				offset++;
				continue;
			}
			const char* left = &source[offset];
			offset += parse(vm, &source[offset], RESULTS_DISCARD, PARSE_UNGREEDY);
			item_t pair = pop(vm);
			ensure(vm, pair.node->type == NODE_MULTI && vec_size(vm, pair.node->keys) == 1 && vec_size(vm, pair.node->vals) == 1,
				"expected key/val pair: %s", left);
			vec_push_allot(vm, &node->vals, pair);
		}
		ensure(vm, source[offset] == '}', "expected closing brace: %s", &source[offset]);
		offset++;
	}
	else {
		ensure(vm, 0, "what: %s", &source[offset]);
	}

	node_t *prev = node;

	while (source[offset]) {
		if ((length = skip_gap(&source[offset])) > 0) {
			offset += length;
			continue;
		}

		// function/opcode call arguments
		if (source[offset] == '(') {
			offset += parse_arglist(vm, &source[offset]);
			// Chaining calls ()()...() means the previous node may be
			// a nested vecmap[...] or already have its own arguments so
			// chain extra OP_CALLs.
			if (prev->index || prev->call || prev->args) {
				node_t* call = node_allot(vm);
				call->type = NODE_CALL_CHAIN;
				call->args = pop(vm).node;
				prev->chain = call;
				prev = call;
			} else {
				prev->call = 1;
				prev->args = pop(vm).node;
			}
			break;
		}

		// chained vector[n] or map[k] expressions
		if (source[offset] == '[') {
			offset++;
			offset += parse_node(vm, &source[offset]);
			prev->chain = pop(vm).node;
			prev = prev->chain;
			prev->index = true;
			offset += skip_gap(&source[offset]);
			ensure(vm, source[offset] == ']', "expected closing bracket: %s", &source[offset]);
			offset++;
			continue;
		}

		// chained map.field.subfield expressions
		if (source[offset] == '.' && isnamefirst(source[offset+1])) {
			offset++;
			offset += parse_node(vm, &source[offset]);
			prev->chain = pop(vm).node;
			prev = prev->chain;
			prev->field = true;
			continue;
		}

		// chained map:field:subfield expressions
		if (source[offset] == ':' && isnamefirst(source[offset+1])) {
			offset++;
			offset += parse_node(vm, &source[offset]);
			prev->chain = pop(vm).node;
			prev = prev->chain;
			prev->field = true;
			prev->method = true;
			continue;
		}
		break;
	}

	push(vm, (item_t){.type = NODE, .node = node});
	return offset;
}

static int parse(rela_vm* vm, const char *source, int results, int mode) {
	int length = 0;
	int offset = skip_gap(source);

	node_t *node = node_allot(vm);
	node->type = NODE_MULTI;
	node->results = results;

	while (source[offset]) {
		if ((length = skip_gap(&source[offset])) > 0){
			offset += length;
			continue;
		}

		// shunting yard
		operator_t *operations[STACK];
		node_t *arguments[STACK];

		int operation = 0;
		int argument = 0;

		while (source[offset]) {
			if ((length = skip_gap(&source[offset])) > 0) {
				offset += length;
				continue;
			}

			if (source[offset] == '(') {
				offset++;
				offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_COMMA|PARSE_ANDOR);
				arguments[argument++] = pop(vm).node;
				arguments[argument-1]->results = RESULTS_FIRST;
				offset += skip_gap(&source[offset]);
				ensure(vm, source[offset] == ')', "expected closing paren: %s", &source[offset]);
				offset++;
			}
			else {
				offset += parse_node(vm, &source[offset]);
				arguments[argument++] = pop(vm).node;
			}

			offset += skip_gap(&source[offset]);
			operator_t* compare = NULL;

			for (int i = 0; i < sizeof(operators) / sizeof(operator_t); i++) {
				operator_t *op = &operators[i];
				int oplen = strlen(op->name);
				if (!strncmp(op->name, &source[offset], oplen)) {
					// and/or needs a trailing space
					if (isalpha(op->name[oplen-1]) && !isspace(source[offset+oplen])) continue;
					compare = op;
					break;
				}
			}

			if (!compare) break;
			offset += strlen(compare->name);

			while (operation > 0 && operations[operation-1]->precedence >= compare->precedence) {
				operator_t *consume = operations[--operation];
				node_t *result = node_allot(vm);
				result->type   = NODE_OPERATOR;
				result->opcode = consume->opcode;
				result->single = consume->single;
				ensure(vm, argument >= consume->argc, "operator %s insufficient arguments", consume->name);
				for (int j = consume->argc; j > 0; --j) {
					vec_push_allot(vm, &result->vals, (item_t){.type = NODE, .node = arguments[argument-j]});
				}
				argument -= consume->argc;
				arguments[argument++] = result;
			}

			operations[operation++] = compare;
			if (compare->argc == 1 && argument > 0) break;
		}

		while (operation && argument) {
			operator_t *consume = operations[--operation];
			node_t *result = node_allot(vm);
			result->type   = NODE_OPERATOR;
			result->opcode = consume->opcode;
			result->single = consume->single;
			ensure(vm, argument >= consume->argc, "operator %s insufficient arguments", consume->name);
			for (int j = consume->argc; j > 0; --j) {
				vec_push_allot(vm, &result->vals, (item_t){.type = NODE, .node = arguments[argument-j]});
			}
			argument -= consume->argc;
			arguments[argument++] = result;
		}

		ensure(vm, !operation && argument == 1, "unbalanced expression: %s", &source[offset]);
		vec_push_allot(vm, &node->vals, (item_t){.type = NODE, .node = arguments[0]});

		offset += skip_gap(&source[offset]);

		if (source[offset] == '=') {
			ensure(vm, vec_size(vm, node->vals) > 0, "missing assignment name: %s", &source[offset]);

			offset++;
			for (int i = 0; i < vec_size(vm, node->vals); i++)
				vec_push_allot(vm, &node->keys, vec_get(vm, node->vals, i));

			vec_clear(vm, node->vals);
			continue;
		}

		if (source[offset] == ',' && (mode & PARSE_COMMA)) {
			offset++;
			continue;
		}

		break;
	}

	ensure(vm, vec_size(vm, node->vals) > 0, "missing assignment value: %s", &source[offset]);

	bool solo = !node->args && !vec_size(vm, node->keys) && vec_size(vm, node->vals) == 1;

	// node guarantees to handle it's own result limits
	if (solo && vec_get(vm, node->vals, 0).node->control) {
		push(vm, (item_t){.type = NODE, .node = vec_get(vm, node->vals, 0).node});
		return offset;
	}

	// node guarantees to return a single result
	if (solo && results != RESULTS_DISCARD && vec_get(vm, node->vals, 0).node->single) {
		push(vm, (item_t){.type = NODE, .node = vec_get(vm, node->vals, 0).node});
		return offset;
	}

	push(vm, (item_t){.type = NODE, .node = node});
	return offset;
}

static void process(rela_vm* vm, node_t *node, int flags, int index, int limit) {
	int flag_assign = flags & PROCESS_ASSIGN ? 1:0;

	// if we're assigning with chained expressions, only OP_SET|OP_ASSIGN the last one
	bool assigning = flag_assign && !node->chain;

	char tmp[STRTMP];

	// a multi-part expression: a[,b...] = node[,node...]
	// this is the entry point for most non-control non-opcode statements
	if (node->type == NODE_MULTI) {
		assert(!node->args);
		assert(vec_size(vm, node->vals));

		// substack frame
		if (node->results != RESULTS_ALL)
			compile(vm, OP_MARK, nil(vm));

		// stream the values onto the substack
		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0, -1);

		// OP_SET|OP_ASSIGN index values from the start of the current substack frame
		for (int i = 0; i < vec_size(vm, node->keys); i++) {
			node_t* subnode = vec_get(vm, node->keys, i).node;
			process(vm, subnode, PROCESS_ASSIGN, i, -1);
		}

		// end substack frame
		if (node->results != RESULTS_ALL)
			compile(vm, OP_LIMIT, integer(vm, node->results));
	}
	else
	if (node->type == NODE_NAME) {
		assert(!vec_size(vm, node->keys) && !vec_size(vm, node->vals));

		// function or function-like opcode call
		if (node->call) {
			assert(!assigning);

			// vecmap[fn()]
			if (node->index) {
				compile(vm, OP_MARK, nil(vm));
					if (node->args)
						process(vm, node->args, 0, 0, -1);
					compile(vm, OP_LIT, node->item);
					compile(vm, OP_FIND, nil(vm));
					compile(vm, OP_CALL, nil(vm));
				compile(vm, OP_LIMIT, integer(vm, 1));
				compile(vm, OP_GET, nil(vm));
			}

			// :fn()
			if (node->field && node->method) {
				compile(vm, OP_COPY, nil(vm));
				compile(vm, OP_LIT, node->item);
				compile(vm, OP_GET, nil(vm));
				compile(vm, OP_SHUNT, nil(vm));
				compile(vm, OP_SHUNT, nil(vm));
				compile(vm, OP_MARK, nil(vm));
					compile(vm, OP_SHIFT, nil(vm));
					if (node->args)
						process(vm, node->args, 0, 0, -1);
					compile(vm, OP_SHIFT, nil(vm));
					compile(vm, OP_CALL, nil(vm));
				compile(vm, OP_LIMIT, integer(vm, limit));
			}

			// .fn()
			if (node->field && !node->method) {
				compile(vm, OP_LIT, node->item);
				compile(vm, OP_GET, nil(vm));
				compile(vm, OP_SHUNT, nil(vm));
				compile(vm, OP_MARK, nil(vm));
					if (node->args)
						process(vm, node->args, 0, 0, -1);
					compile(vm, OP_SHIFT, nil(vm));
					compile(vm, OP_CALL, nil(vm));
				compile(vm, OP_LIMIT, integer(vm, limit));
			}

			// fn()
			if (!node->index && !node->field) {
				compile(vm, OP_MARK, nil(vm));
					if (node->args)
						process(vm, node->args, 0, 0, -1);
					compile(vm, OP_LIT, node->item);
					compile(vm, OP_FIND, nil(vm));
					compile(vm, OP_CALL, nil(vm));
				compile(vm, OP_LIMIT, integer(vm, limit));
			}
		}
		// variable reference
		else {
			compile(vm, OP_LIT, node->item);

			if (assigning) {
				if (node->index) {
					compile(vm, OP_FIND, nil(vm));
					compile(vm, OP_SET, nil(vm));
				}

				if (node->field) {
					compile(vm, OP_SET, nil(vm));
				}

				if (!node->index && !node->field) {
					compile(vm, OP_ASSIGN, integer(vm, index));
				}
			}
			else {
				if (node->index) {
					compile(vm, OP_FIND, nil(vm));
					compile(vm, OP_GET, nil(vm));
				}

				if (node->field) {
					compile(vm, OP_GET, nil(vm));
				}

				if (!node->index && !node->field) {
					compile(vm, OP_FIND, nil(vm));
				}
			}
		}

		if (node->chain) {
			process(vm, node->chain, flag_assign ? PROCESS_ASSIGN: 0, 0, 1);
		}
	}
	else
	// function with optional name assignment
	if (node->type == NODE_FUNCTION) {
		assert(!node->args);

		compile(vm, OP_MARK, nil(vm));
		int entry = compile(vm, OP_LIT, nil(vm));

		if (node->item.type) {
			compile(vm, OP_LIT, node->item);
			compile(vm, OP_ASSIGN, integer(vm, 0));
		}

		int jump = compile(vm, OP_JMP, nil(vm));
		compiled(vm, entry)->item = (item_t){.type = SUBROUTINE, .sub = vm->code.depth};

		compile(vm, OP_PID, integer(vm, node->fpath.id));
		for (int i = 0, l = node->fpath.depth; i < l; i++) {
			compile(vm, OP_PID, integer(vm, node->fpath.ids[i]));
		}

		for (int i = 0, l = vec_size(vm, node->keys); i < l; i++)
			process(vm, vec_get(vm, node->keys, i).node, PROCESS_ASSIGN, i, -1);

		compile(vm, OP_CLEAN, nil(vm));

		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0, 0);

		// if an explicit return expression is used, these instructions
		// will be dead code
		compile(vm, OP_CLEAN, nil(vm));
		compile(vm, OP_RETURN, nil(vm));
		compiled(vm, jump)->item = integer(vm, vm->code.depth);

		// value only returns if not function name() form
		compile(vm, OP_LIMIT, integer(vm, node->item.type ? 0:1));

		// function() ... end()
		if (node->call) {
			compile(vm, OP_SHUNT, nil(vm));
			compile(vm, OP_MARK, nil(vm));
				if (node->args)
					process(vm, node->args, 0, 0, -1);
				compile(vm, OP_SHIFT, nil(vm));
				compile(vm, OP_CALL, nil(vm));
			compile(vm, OP_LIMIT, integer(vm, limit));
		}
	}
	else
	// function/opcode call
	if (node->type == NODE_CALL_CHAIN) {
		compile(vm, OP_SHUNT, nil(vm));
		compile(vm, OP_MARK, nil(vm));
			if (node->args)
				process(vm, node->args, 0, 0, -1);
			compile(vm, OP_SHIFT, nil(vm));
			for (int i = 0; i < vec_size(vm, node->vals); i++)
				process(vm, vec_get(vm, node->vals, i).node, 0, 0, -1);
			compile(vm, OP_CALL, nil(vm));
		compile(vm, OP_LIMIT, integer(vm, limit));

		if (node->index) {
			compile(vm, assigning ? OP_SET: OP_GET, nil(vm));
		}

		if (node->chain) {
			process(vm, node->chain, flag_assign ? PROCESS_ASSIGN: 0, 0, 1);
		}
	}
	// inline opcode
	else
	if (node->type == NODE_OPCODE) {
		assert(node->opcode != OP_CALL);

		if (node->args)
			process(vm, node->args, 0, 0, -1);

		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0, -1);

		compile(vm, node->opcode, nil(vm));

		if (node->index) {
			compile(vm, assigning ? OP_SET: OP_GET, nil(vm));
		}

		if (node->chain) {
			process(vm, node->chain, flag_assign ? PROCESS_ASSIGN: 0, 0, 1);
		}
	}
	else
	if (node->type == NODE_OPERATOR && node->opcode == OP_AND) {
		assert(vec_size(vm, node->vals) == 2);
		process(vm, vec_get(vm, node->vals, 0).node, 0, 0, 1);
		int jump = compile(vm, OP_JFALSE, nil(vm));
		compile(vm, OP_DROP, nil(vm));
		process(vm, vec_get(vm, node->vals, 1).node, 0, 0, 1);
		compiled(vm, jump)->item = integer(vm, vm->code.depth);
	}
	else
	if (node->type == NODE_OPERATOR && node->opcode == OP_OR) {
		assert(vec_size(vm, node->vals) == 2);
		process(vm, vec_get(vm, node->vals, 0).node, 0, 0, 1);
		int jump = compile(vm, OP_JTRUE, nil(vm));
		compile(vm, OP_DROP, nil(vm));
		process(vm, vec_get(vm, node->vals, 1).node, 0, 0, 1);
		compiled(vm, jump)->item = integer(vm, vm->code.depth);
	}
	else
	if (node->type == NODE_OPERATOR) {
		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0, 1);

		compile(vm, node->opcode, nil(vm));

		if (node->index) {
			compile(vm, assigning ? OP_SET: OP_GET, nil(vm));
		}

		if (node->chain) {
			process(vm, node->chain, flag_assign ? PROCESS_ASSIGN: 0, 0, 1);
		}
	}
	else
	// string or number literal, optionally part of array chain a[b]["c"]
	if (node->type == NODE_LITERAL) {
		assert(!node->args && !vec_size(vm, node->keys) && !vec_size(vm, node->vals));

		char *dollar = node->item.type == STRING ? strchr(node->item.str, '$'): NULL;

		if (dollar && dollar < node->item.str + strlen(node->item.str) - 1) {
			assert(node->item.type == STRING);

			const char *str = node->item.str;
			const char *left = str;
			const char *right = str;

			bool started = false;

			while ((right = strchr(left, '$')) && right && *right) {
				const char *start = right+1;
				const char *finish = start;
				int length = 0;

				if (*start == '(') {
					start++;
					char *rparen = strchr(start, ')');
					ensure(vm, rparen, "string interpolation missing closing paren: %s", right);
					length = rparen-start;
					finish = rparen+1;
				}
				else {
					length = str_skip(start, isname);
					finish = &start[length];
				}

				if (right > left) {
					compile(vm, OP_LIT, string(vm, substr(vm, left, 0, right-left+(length ? 0:1))));
					if (started) compile(vm, OP_CONCAT, nil(vm));
					started = true;
				}

				left = finish;

				if (length) {
					const char *sub = substr(vm, start, 0, length);
					ensure(vm, length == parse(vm, sub, RESULTS_FIRST, PARSE_COMMA|PARSE_ANDOR), "string interpolation parsing failed");
					process(vm, pop(vm).node, 0, 0, -1);
					if (started) compile(vm, OP_CONCAT, nil(vm));
					started = true;
				}
			}

			if (strlen(left)) {
				compile(vm, OP_LIT, string(vm, substr(vm, left, 0, strlen(left))));
				if (started) compile(vm, OP_CONCAT, nil(vm));
				started = true;
			}
		}
		else {
			compile(vm, OP_LIT, node->item);
		}

		if (node->index) {
			compile(vm, assigning ? OP_SET: OP_GET, nil(vm));
		}

		if (node->chain) {
			process(vm, node->chain, flag_assign ? PROCESS_ASSIGN: 0, 0, 1);
		}

		ensure(vm, !assigning || node->item.type == STRING, "cannot assign %s",
			tmptext(vm, node->item, tmp, sizeof(tmp)));

		// special case allows: "complex-string" = value in map literals
		if (!node->index && assigning && node->item.type == STRING) {
			compile(vm, OP_ASSIGN, integer(vm, index));
		}
	}
	else
	// if expression ... [else ...] end
	// (returns a value for ternary style assignment)
	if (node->type == NODE_IF) {

		// conditions
		if (node->args)
			process(vm, node->args, 0, 0, -1);

		// if false, jump to else/end
		int jump = compile(vm, OP_JFALSE, nil(vm));
		compile(vm, OP_DROP, nil(vm));

		// success block
		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0, 0);

		// optional failure block
		if (vec_size(vm, node->keys)) {
			// jump success path past failure block
			int jump2 = compile(vm, OP_JMP, nil(vm));
			compiled(vm, jump)->item = integer(vm, vm->code.depth);
			compile(vm, OP_DROP, nil(vm));

			// failure block
			for (int i = 0; i < vec_size(vm, node->keys); i++)
				process(vm, vec_get(vm, node->keys, i).node, 0, 0, 0);

			compiled(vm, jump2)->item = integer(vm, vm->code.depth);
		}
		else {
			compiled(vm, jump)->item = integer(vm, vm->code.depth);
		}

		ensure(vm, !assigning, "cannot assign to if block");
	}
	else
	// while expression ... end
	if (node->type == NODE_WHILE) {
		assert(node->vals);

		compile(vm, OP_MARK, nil(vm));
		int loop = compile(vm, OP_LOOP, nil(vm));
		int begin = vm->code.depth;

		// condition(s)
		if (node->args)
			process(vm, node->args, 0, 0, -1);

		// if false, jump to end
		int iter = compile(vm, OP_JFALSE, nil(vm));
		compile(vm, OP_DROP, nil(vm));

		// do ... end
		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0, 0);

		// clean up
		compile(vm, OP_JMP, integer(vm, begin));
		compiled(vm, iter)->item = integer(vm, vm->code.depth);
		compiled(vm, loop)->item = integer(vm, vm->code.depth);
		compile(vm, OP_UNLOOP, nil(vm));
		compile(vm, OP_LIMIT, integer(vm, 0));

		ensure(vm, !assigning, "cannot assign to while block");
	}
	else
	// for k,v in container ... end
	if (node->type == NODE_FOR) {
		compile(vm, OP_MARK, nil(vm));

		// the iterable
		if (node->args)
			process(vm, node->args, 0, 0, -1);

		int loop = compile(vm, OP_LOOP, nil(vm));

		int begin = vm->code.depth;

		// OP_FOR expects a vector with key[,val] variable names
		if (!node->keys) node->keys = vec_allot(vm);
		compile(vm, OP_FOR, (item_t){.type = VECTOR, .vec = node->keys});

		// block
		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0, 0);

		// clean up
		compile(vm, OP_JMP, integer(vm, begin));
		compiled(vm, loop)->item = integer(vm, vm->code.depth);
		compile(vm, OP_UNLOOP, nil(vm));
		compile(vm, OP_LIMIT, integer(vm, 0));

		ensure(vm, !assigning, "cannot assign to for block");
	}
	else
	// return 0 or more values
	if (node->type == NODE_RETURN) {
		compile(vm, OP_CLEAN, nil(vm));

		if (node->args)
			process(vm, node->args, 0, 0, -1);

		compile(vm, OP_RETURN, nil(vm));

		ensure(vm, !assigning, "cannot assign to return");
	}
	else
	// literal vector [1,2,3]
	if (node->type == NODE_VEC) {
		assert(!node->args);
		compile(vm, OP_VECTOR, nil(vm));
		compile(vm, OP_MARK, nil(vm));

		for (int i = 0; i < vec_size(vm, node->vals); i++) {
			process(vm, vec_get(vm, node->vals, i).node, 0, 0, -1);
			compile(vm, OP_VPUSH, nil(vm));
		}

		compile(vm, OP_LIMIT, integer(vm, 0));
		compile(vm, OP_SHIFT, nil(vm));
	}
	else
	// literal map { a = 1, b = 2, c = nil }
	if (node->type == NODE_MAP) {
		compile(vm, OP_MARK, nil(vm));
		compile(vm, OP_MAP, nil(vm));
		assert(!node->args);

		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0, 0);

		compile(vm, OP_UNMAP, nil(vm));
		compile(vm, OP_LIMIT, integer(vm, 1));
	}
	else {
		ensure(vm, 0, "unexpected expression type: %d", node->type);
	}
}

static void source(rela_vm* vm, const char *source) {
	int offset = skip_gap(source);

	while (source[offset]) {
		offset += parse(vm, &source[offset], RESULTS_DISCARD, PARSE_COMMA|PARSE_ANDOR);
		process(vm, pop(vm).node, 0, 0, -1);
	}

	ensure(vm, !depth(vm), "parse unbalanced");
}

static item_t literal(rela_vm* vm) {
	return vm->code.cells[vm->routine->ip-1].item;
}

static int64_t literal_int(rela_vm* vm) {
	item_t lit = literal(vm);
	return lit.type == INTEGER ? lit.inum: 0;
}

static int* cache_slot(rela_vm* vm) {
	return &vm->code.cells[vm->routine->ip-1].cache;
}

static void op_stop (rela_vm* vm) {
}

static void op_print(rela_vm* vm) {
	int items = depth(vm);
	if (!items) return;

	item_t* item = stack_cell(vm, -items);

	char tmp[STRBUF];
	for (int i = 0; i < items; i++) {
		const char *str = tmptext(vm, *item++, tmp, sizeof(tmp));
		fprintf(stdout, "%s%s", i ? "\t": "", str);
	}
	fprintf(stdout, "\n");
	fflush(stdout);
}

static void op_clean(rela_vm* vm) {
	vm->routine->stack.depth -= depth(vm);
}

static void op_meta_set(rela_vm* vm) {
	item_t meta = pop(vm);
	item_t obj = pop(vm);

	if (obj.type == VECTOR) {
		obj.vec->meta = meta;
		return;
	}

	if (obj.type == MAP) {
		obj.map->meta = meta;
		return;
	}

	if (obj.type == USERDATA) {
		obj.data->meta = meta;
		return;
	}

	ensure(vm, false, "cannot set meta");
}

static void op_meta_get(rela_vm* vm) {
	item_t obj = pop(vm);

	if (obj.type == VECTOR) {
		push(vm, obj.vec->meta);
		return;
	}

	if (obj.type == MAP) {
		push(vm, obj.map->meta);
		return;
	}

	if (obj.type == USERDATA) {
		push(vm, obj.data->meta);
		return;
	}

	push(vm, nil(vm));
}

static void op_map(rela_vm* vm) {
	opush(vm, vm->routine->map);
	vm->routine->map = (item_t){.type = MAP, .map = map_allot(vm)};
}

static void op_unmap(rela_vm* vm) {
	push(vm, vm->routine->map);
	vm->routine->map = opop(vm);
}

static void op_mark(rela_vm* vm) {
	assert(vm->routine->marks.depth < sizeof(vm->routine->marks.cells)/sizeof(int));
	vm->routine->marks.cells[vm->routine->marks.depth++] = vm->routine->stack.depth;
}

static void limit(rela_vm* vm, int count) {
	assert(vm->routine->marks.depth > 0);
	int old_depth = vm->routine->marks.cells[--vm->routine->marks.depth];
	if (count >= 0) {
		int req_depth = old_depth + count;
		if (req_depth < vm->routine->stack.depth) {
			vm->routine->stack.depth = req_depth;
		} else
		while (req_depth > vm->routine->stack.depth) push(vm, nil(vm));
	}
}

static void op_limit(rela_vm* vm) {
	limit(vm, literal_int(vm));
}

static void arrive(rela_vm* vm, int ip) {
	cor_t* cor = vm->routine;

	assert(cor->frames.depth < sizeof(cor->frames.cells)/sizeof(frame_t));
	frame_t* frame = &cor->frames.cells[cor->frames.depth++];

	frame->loops = cor->loops.depth;
	frame->marks = cor->marks.depth;
	frame->ip = cor->ip;

	frame->locals.depth = 0;
	frame->path.depth = 0;

	frame->map = cor->map;
	cor->map = nil(vm);

	cor->ip = ip;
}

static void depart(rela_vm* vm) {
	cor_t* cor = vm->routine;

	assert(cor->frames.depth > 0);
	frame_t* frame = &cor->frames.cells[--cor->frames.depth];

	cor->ip = frame->ip;
	cor->marks.depth = frame->marks;
	cor->loops.depth = frame->loops;

	cor->map = frame->map;
}

static void op_coroutine(rela_vm* vm) {
	cor_t *cor = cor_allot(vm);

	ensure(vm, depth(vm) && item(vm, 0)->type == SUBROUTINE, "coroutine missing subroutine");

	int ip = item(vm, 0)->sub;

	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor});
	vm->routine = cor;

	cor->state = COR_RUNNING;
	arrive(vm, ip);
	op_mark(vm);

	cor->state = COR_SUSPENDED;

	vec_pop(vm, &vm->routines);
	vm->routine = vec_top(vm, &vm->routines).cor;

	op_clean(vm);

	push(vm, (item_t){.type = COROUTINE, .cor = cor});
}

static void op_resume(rela_vm* vm) {
	ensure(vm, depth(vm) && item(vm, 0)->type == COROUTINE, "resume missing coroutine");
	cor_t *cor = item(vm, 0)->cor;

	int items = depth(vm);
	cor_t* caller = vm->routine;

	if (cor->state == COR_DEAD) {
		caller->stack.depth -= items;
		push(vm, nil(vm));
		return;
	}

	cor->state = COR_RUNNING;

	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor});
	vm->routine = cor;

	for (int i = 1; i < items; i++) {
		int index = caller->stack.depth-items+i;
		push(vm, *stack_ref(vm, caller, index));
	}

	caller->stack.depth -= items;
}

static void op_yield(rela_vm* vm) {
	int items = depth(vm);

	cor_t* caller = vm->routine;
	caller->state = COR_SUSPENDED;

	vec_pop(vm, &vm->routines);
	vm->routine = vec_top(vm, &vm->routines).cor;

	for (int i = 0; i < items; i++) {
		int index = caller->stack.depth-items+i;
		push(vm, *stack_ref(vm, caller, index));
	}

	caller->stack.depth -= items;
}

static void op_global(rela_vm* vm) {
	push(vm, (item_t){.type = MAP, .map = vm->scope_global});
}

static void call(rela_vm* vm, item_t item) {
	if (item.type == CALLBACK) {
		item.cb((rela_vm*)vm);
		return;
	}

	int args = depth(vm);

	char tmp[STRTMP];
	ensure(vm, item.type == SUBROUTINE, "invalid function: %s (ip: %u)", tmptext(vm, item, tmp, sizeof(tmp)), vm->routine->ip);

	arrive(vm, item.sub);

	op_mark(vm);
	// subroutines need to know the base of their subframe,
	// which is the same as the current depth of the outer
	// frame mark
	vm->routine->marks.cells[vm->routine->marks.depth-1] -= args;
}

static void op_call(rela_vm* vm) {
	call(vm, pop(vm));
}

static void op_return(rela_vm* vm) {
	cor_t* cor = vm->routine;

	// subroutines leave only results in their subframe, which
	// migrate to the caller frame when depart() truncates the
	// marks stack
	depart(vm);

	if (!cor->ip) {
		cor->state = COR_DEAD;
		op_yield(vm);
		return;
	}
}

static void op_drop(rela_vm* vm) {
	pop(vm);
}

static void op_lit(rela_vm* vm) {
	push(vm, literal(vm));
}

static void op_loop(rela_vm* vm) {
	assert(vm->routine->loops.depth+2 < sizeof(vm->routine->loops.cells)/sizeof(int));
	vm->routine->loops.cells[vm->routine->loops.depth++] = vm->routine->marks.depth;
	vm->routine->loops.cells[vm->routine->loops.depth++] = literal_int(vm);
	vm->routine->loops.cells[vm->routine->loops.depth++] = 0;
}

static void op_unloop(rela_vm* vm) {
	assert(vm->routine->loops.depth > 2);
	--vm->routine->loops.depth;
	--vm->routine->loops.depth;
	ensure(vm, vm->routine->loops.cells[--vm->routine->loops.depth] == vm->routine->marks.depth, "mark stack mismatch (unloop)");
}

static void op_break(rela_vm* vm) {
	vm->routine->ip = vm->routine->loops.cells[vm->routine->loops.depth-2];
	vm->routine->marks.depth = vm->routine->loops.cells[vm->routine->loops.depth-3];
	while (depth(vm)) pop(vm);
}

static void op_continue(rela_vm* vm) {
	vm->routine->ip = vm->routine->loops.cells[vm->routine->loops.depth-2]-1;
	vm->routine->marks.depth = vm->routine->loops.cells[vm->routine->loops.depth-3];
	while (depth(vm)) pop(vm);
}

static void op_copy(rela_vm* vm) {
	push(vm, top(vm));
}

static void op_shunt(rela_vm* vm) {
	opush(vm, pop(vm));
}

static void op_shift(rela_vm* vm) {
	push(vm, opop(vm));
}

static void op_nil(rela_vm* vm) {
	push(vm, nil(vm));
}

static void op_true(rela_vm* vm) {
	push(vm, (item_t){.type = BOOLEAN, .flag = 1});
}

static void op_false(rela_vm* vm) {
	push(vm, (item_t){.type = BOOLEAN, .flag = 0});
}

static void op_jmp(rela_vm* vm) {
	vm->routine->ip = literal_int(vm);
}

static void op_jfalse(rela_vm* vm) {
	if (!truth(vm, top(vm))) op_jmp(vm);
}

static void op_jtrue(rela_vm* vm) {
	if (truth(vm, top(vm))) op_jmp(vm);
}

static void op_vector(rela_vm* vm) {
	opush(vm, (item_t){.type = VECTOR, .vec = vec_allot(vm)});
}

static void op_vpush(rela_vm* vm) {
	for (int i = 0, l = depth(vm); i < l; i++)
		vec_push(vm, otop(vm).vec, *item(vm, i));
	op_clean(vm);
}

static void op_unpack(rela_vm* vm) {
	vec_t* vec = pop_type(vm, VECTOR).vec;

	for (int i = 0, l = vec_size(vm, vec); i < l; i++)
		push(vm, vec_get(vm, vec, i));
}

static void op_pid(rela_vm* vm) {
	assert(vm->routine->frames.depth);
	frame_t* frame = &vm->routine->frames.cells[vm->routine->frames.depth-1];
	// depth++ range is capped at compile time
	frame->path.cells[frame->path.depth++] = literal(vm).inum;
}

static void op_type(rela_vm* vm) {
	item_t a = pop(vm);
	push(vm, string(vm, type_names[a.type]));
}

// locate a local variable cell in the current frame
static item_t* local(rela_vm* vm, const char* key) {
	cor_t* cor = vm->routine;

	if (!cor->frames.depth) return NULL;
	frame_t* frame = &cor->frames.cells[cor->frames.depth-1];

	for (int i = 0, l = frame->locals.depth; i < l; i++) {
		if (frame->locals.keys[i] == key) return &frame->locals.vals[i];
	}
	return NULL;
}

// locate a local variable in-scope in an outer frame
static item_t* uplocal(rela_vm* vm, const char* key) {
	cor_t* cor = vm->routine;
	if (cor->frames.depth < 2) return NULL;

	int index = cor->frames.depth;
	frame_t* lframe = &cor->frames.cells[--index];

	int* pids = lframe->path.cells;
	int depth = lframe->path.depth;
	assert(depth >= 1);

	while (depth > 1 && index > 0) {
		frame_t* uframe = &cor->frames.cells[--index];

		assert(uframe->path.depth);
		int pid = uframe->path.cells[0];

		// i=1 to skip checking outer recursive calls to current function
		for (int i = 1, l = depth; i < l; i++) {
			// only check this call stack frame if it belongs to another
			// function from the current function's compile-time scope chain
			if (pid == pids[i]) {
				for (int i = 0, l = uframe->locals.depth; i < l; i++) {
					if (uframe->locals.keys[i] == key) {
						return &uframe->locals.vals[i];
					}
				}
				break;
			}
		}
	}
	return NULL;
}

static void assign(rela_vm* vm, item_t key, item_t val) {
	cor_t* cor = vm->routine;

	// OP_ASSIGN is used for too many things: local variables, map literal keys, global keys
	map_t* map = cor->map.type == MAP ? cor->map.map: NULL;

	if (!map && cor->frames.depth) {
		assert(key.type == STRING);
		item_t* cell = local(vm, key.str);
		if (cell) { *cell = val; return; }
		frame_t* frame = &cor->frames.cells[cor->frames.depth-1];
		// todo: depth++ range limit at compile time
		ensure(vm, frame->locals.depth < LOCALS, "max %d locals per frame", LOCALS);
		frame->locals.keys[frame->locals.depth] = key.str;
		frame->locals.vals[frame->locals.depth++] = val;
		return;
	}
	map_set(vm, map ? map: vm->scope_global, key, val);
}

static item_t* find(rela_vm* vm, item_t key) {
	assert(key.type == STRING);
	item_t* cell = local(vm, key.str);
	if (!cell) cell = uplocal(vm, key.str);
	if (!cell) cell = map_ref(vm, vm->scope_global, key);
	if (!cell) cell = map_ref(vm, vm->scope_core, key);
	return cell;
}

static void op_assign(rela_vm* vm) {
	item_t key = pop(vm);

	int index = literal_int(vm);
	// indexed from the base of the current subframe
	item_t val = depth(vm) > index ? *item(vm, index): nil(vm);

	assign(vm, key, val);
}

static void op_find(rela_vm* vm) {
	item_t key = pop(vm);
	item_t* val = find(vm, key);

	char tmp[STRTMP];
	ensure(vm, val, "unknown name: %s", tmptext(vm, key, tmp, sizeof(tmp)));

	push(vm, *val);
}

static void op_for(rela_vm* vm) {
	assert(literal(vm).type == VECTOR);

	int var = 0;
	vec_t* vars = literal(vm).vec;

	item_t iter = top(vm);
	int step = vm->routine->loops.cells[vm->routine->loops.depth-1];

	if (iter.type == INTEGER) {
		if (step == iter.inum) {
			vm->routine->ip = vm->routine->loops.cells[vm->routine->loops.depth-2];
		}
		else {
			if (vec_size(vm, vars) > 1)
				assign(vm, vec_get(vm, vars, var++), integer(vm, step));

			if (vec_size(vm, vars) > 0)
				assign(vm, vec_get(vm, vars, var++), integer(vm, step));
		}
	}
	else
	if (iter.type == VECTOR) {
		if (step >= vec_size(vm, iter.vec)) {
			vm->routine->ip = vm->routine->loops.cells[vm->routine->loops.depth-2];
		}
		else {
			if (vec_size(vm, vars) > 1)
				assign(vm, vec_get(vm, vars, var++), integer(vm, step));

			if (vec_size(vm, vars) > 0)
				assign(vm, vec_get(vm, vars, var++), vec_get(vm, iter.vec, step));
		}
	}
	else
	if (iter.type == MAP) {
		if (step >= vec_size(vm, &iter.map->keys)) {
			vm->routine->ip = vm->routine->loops.cells[vm->routine->loops.depth-2];
		}
		else {
			if (vec_size(vm, vars) > 1)
				assign(vm, vec_get(vm, vars, var++), vec_get(vm, &iter.map->keys, step));

			if (vec_size(vm, vars) > 0)
				assign(vm, vec_get(vm, vars, var++), vec_get(vm, &iter.map->vals, step));
		}
	}
	else
	if (iter.type == SUBROUTINE || iter.type == CALLBACK) {
		item_t argv[1] = {integer(vm, step)};
		item_t retv[2] = {nil(vm), nil(vm)};
		method(vm, iter, 1, argv, 2, retv);

		if (retv[0].type == NIL) {
			vm->routine->ip = vm->routine->loops.cells[vm->routine->loops.depth-2];
		}
		else {
			int idx = 0;
			if (vec_size(vm, vars) > 1)
				assign(vm, vec_get(vm, vars, var++), retv[idx++]);
			if (vec_size(vm, vars) > 0)
				assign(vm, vec_get(vm, vars, var++), retv[idx++]);
		}
	}
	else
	if (iter.type == COROUTINE) {
		op_mark(vm);
		push(vm, iter);
		push(vm, integer(vm, step));
		op_resume(vm);
		while (vm->routine == iter.cor && tick(vm));

		if (!depth(vm) || item(vm, 0)->type == NIL) {
			vm->routine->ip = vm->routine->loops.cells[vm->routine->loops.depth-2];
		}
		else {
			int idx = 0;
			if (vec_size(vm, vars) > 1)
				assign(vm, vec_get(vm, vars, var++), depth(vm) > idx ? *item(vm, idx++): nil(vm));
			if (vec_size(vm, vars) > 0)
				assign(vm, vec_get(vm, vars, var++), depth(vm) > idx ? *item(vm, idx++): nil(vm));
		}

		limit(vm, 0);
	}
	else {
		vm->routine->ip = vm->routine->loops.cells[vm->routine->loops.depth-1];
	}

	vm->routine->loops.cells[vm->routine->loops.depth-1] = step+1;
}

static void set(rela_vm* vm, item_t dst, item_t key, item_t val) {
	if (dst.type == VECTOR && key.type == INTEGER && key.inum == vec_size(vm, dst.vec)) {
		vec_push(vm, dst.vec, val);
	}
	else
	if (dst.type == VECTOR && key.type == INTEGER) {
		vec_cell(vm, dst.vec, key.inum)[0] = val;
	}
	else
	if (dst.type == MAP) {
		map_set(vm, dst.map, key, val);
	}
	else {
		char tmpA[STRTMP];
		char tmpB[STRTMP];
		ensure(vm, 0, "cannot set %s (%s) in item %s (%s)",
			tmptext(vm, key, tmpA, sizeof(tmpA)), type_names[key.type],
			tmptext(vm, val, tmpB, sizeof(tmpB)), type_names[dst.type]
		);
	}
}

static void op_set(rela_vm* vm) {
	item_t key = pop(vm);
	item_t dst = pop(vm);

	int index = literal_int(vm);
	item_t val = depth(vm) ? *item(vm, index): nil(vm);
	set(vm, dst, key, val);
}

static item_t get(rela_vm* vm, item_t src, item_t key) {
	if (src.type == VECTOR && key.type == INTEGER) {
		return vec_get(vm, src.vec, key.inum);
	}
	else
	if (src.type == VECTOR && src.vec->meta.type != NIL && key.type == STRING) {
		item_t val = nil(vm);
		meta_get(vm, src.vec->meta, key.str, &val);
		return val;
	}
	if (src.type == MAP) {
		item_t val = nil(vm);
		map_get(vm, src.map, key, &val);
		if (val.type == NIL && src.map->meta.type != NIL && key.type == STRING) {
			meta_get(vm, src.map->meta, key.str, &val);
		}
		return val;
	}
	else {
		char tmpA[STRTMP];
		char tmpB[STRTMP];
		ensure(vm, 0, "cannot get %s (%s) from item %s (%s)",
			tmptext(vm, key, tmpA, sizeof(tmpA)), type_names[key.type],
			tmptext(vm, src, tmpB, sizeof(tmpB)), type_names[src.type]
		);
	}
	return nil(vm);
}

static void op_get(rela_vm* vm) {
	item_t* b = stack_cell(vm, -1);
	item_t* a = stack_cell(vm, -2);
	*a = get(vm, *a, *b);
	op_drop(vm);
}

static void op_add(rela_vm* vm) {
	item_t* b = stack_cell(vm, -1);
	item_t* a = stack_cell(vm, -2);
	*a = add(vm, *a, *b);
	op_drop(vm);
}

static void op_add_lit(rela_vm* vm) {
	item_t* a = stack_cell(vm, -1);
	*a = add(vm, *a, literal(vm));
}

static void op_neg(rela_vm* vm) {
	if (top(vm).type == INTEGER) {
		stack_cell(vm, -1)->inum *= -1;
	}
	else
	if (top(vm).type == FLOAT) {
		stack_cell(vm, -1)->fnum *= -1;
	}
	else {
		char tmp[STRTMP];
		ensure(vm, 0, "cannot negate %s", tmptext(vm, top(vm), tmp, sizeof(tmp)));
	}
}

static void op_sub(rela_vm* vm) {
	op_neg(vm);
	op_add(vm);
}

static void op_mul(rela_vm* vm) {
	item_t* b = stack_cell(vm, -1);
	item_t* a = stack_cell(vm, -2);
	*a = multiply(vm, *a, *b);
	op_drop(vm);
}

static void op_mul_lit(rela_vm* vm) {
	item_t* a = stack_cell(vm, -1);
	*a = multiply(vm, *a, literal(vm));
}

static void op_div(rela_vm* vm) {
	item_t* b = stack_cell(vm, -1);
	item_t* a = stack_cell(vm, -2);
	*a = divide(vm, *a, *b);
	op_drop(vm);
}

static void op_mod(rela_vm* vm) {
	item_t* b = stack_cell(vm, -1);
	item_t* a = stack_cell(vm, -2);
	*a = (item_t){.type = INTEGER, .inum = a->inum % b->inum};
	op_drop(vm);
}

static void op_eq(rela_vm* vm) {
	item_t* b = stack_cell(vm, -1);
	item_t* a = stack_cell(vm, -2);
	*a = (item_t){.type = BOOLEAN, .flag = equal(vm, *a, *b)};
	op_drop(vm);
}

static void op_not(rela_vm* vm) {
	push(vm, (item_t){.type = BOOLEAN, .flag = !truth(vm, pop(vm))});
}

static void op_ne(rela_vm* vm) {
	op_eq(vm);
	op_not(vm);
}

static void op_lt(rela_vm* vm) {
	item_t* b = stack_cell(vm, -1);
	item_t* a = stack_cell(vm, -2);
	*a = (item_t){.type = BOOLEAN, .flag = less(vm, *a, *b)};
	op_drop(vm);
}

static void op_gt(rela_vm* vm) {
	item_t* b = stack_cell(vm, -1);
	item_t* a = stack_cell(vm, -2);
	*a = (item_t){.type = BOOLEAN, .flag = !less(vm, *a, *b) && !equal(vm, *a, *b)};
	op_drop(vm);
}

static void op_lte(rela_vm* vm) {
	item_t* b = stack_cell(vm, -1);
	item_t* a = stack_cell(vm, -2);
	*a = (item_t){.type = BOOLEAN, .flag = less(vm, *a, *b) || equal(vm, *a, *b)};
	op_drop(vm);
}

static void op_gte(rela_vm* vm) {
	item_t* b = stack_cell(vm, -1);
	item_t* a = stack_cell(vm, -2);
	*a = (item_t){.type = BOOLEAN, .flag = !less(vm, *a, *b)};
	op_drop(vm);
}

static void op_concat(rela_vm* vm) {
	item_t b = pop(vm);
	item_t a = pop(vm);
	char tmpA[STRTMP];
	char tmpB[STRTMP];

	const char *as = tmptext(vm, a, tmpA, sizeof(tmpA));
	const char *bs = tmptext(vm, b, tmpB, sizeof(tmpB));
	int lenA = strlen(as);
	int lenB = strlen(bs);

	ensure(vm, lenA+lenB < STRBUF, "op_concat max length exceeded (%d bytes)", STRBUF-1);

	char buf[STRBUF];
	char* ap = buf+0;
	char* bp = buf+lenA;
	memcpy(ap, as, lenA);
	memcpy(bp, bs, lenB);
	buf[lenA+lenB] = 0;

	push(vm, string(vm, buf));
}

static void op_count(rela_vm* vm) {
	item_t a = pop(vm);
	push(vm, integer(vm, count(vm, a)));
}

static void op_acos(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = acos(a.fnum); push(vm, a); }
static void op_asin(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = asin(a.fnum); push(vm, a); }
static void op_atan(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = atan(a.fnum); push(vm, a); }
static void op_cos(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = cos(a.fnum); push(vm, a); }
static void op_sin(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = sin(a.fnum); push(vm, a); }
static void op_tan(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = tan(a.fnum); push(vm, a); }
static void op_cosh(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = cosh(a.fnum); push(vm, a); }
static void op_sinh(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = sinh(a.fnum); push(vm, a); }
static void op_tanh(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = tanh(a.fnum); push(vm, a); }
static void op_ceil(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = ceil(a.fnum); push(vm, a); }
static void op_floor(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = floor(a.fnum); push(vm, a); }
static void op_sqrt(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = sqrt(a.fnum); push(vm, a); }
static void op_log(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = log(a.fnum); push(vm, a); }
static void op_log10(rela_vm* vm) { item_t a = pop_type(vm, FLOAT); a.fnum = log10(a.fnum); push(vm, a); }

static void op_abs(rela_vm* vm) {
	item_t a = pop(vm);
	if (a.type == INTEGER) a.inum = a.inum < 0 ? -a.inum: a.inum;
	else if (a.type == FLOAT) a.fnum = a.fnum < 0.0 ? -a.fnum: a.fnum;
	else ensure(vm, 0, "op_abs invalid type");
	push(vm, a);
}

static void op_atan2(rela_vm* vm) {
	double y = pop_type(vm, FLOAT).fnum;
	double x = pop_type(vm, FLOAT).fnum;
	push(vm, number(vm, atan2(x,y)));
}

static void op_pow(rela_vm* vm) {
	double y = pop_type(vm, FLOAT).fnum;
	double x = pop_type(vm, FLOAT).fnum;
	push(vm, number(vm, pow(x,y)));
}

static void op_min(rela_vm* vm) {
	item_t a = pop(vm);
	while (depth(vm)) {
		item_t b = pop(vm);
		ensure(vm, a.type == b.type, "op_min mixed types");
		a = less(vm, a, b) ? a: b;
	}
	push(vm, a);
}

static void op_max(rela_vm* vm) {
	item_t a = pop(vm);
	while (depth(vm)) {
		item_t b = pop(vm);
		ensure(vm, a.type == b.type, "op_min mixed types");
		a = less(vm, a, b) ? b: a;
	}
	push(vm, a);
}

static void op_match(rela_vm* vm) {
#ifdef PCRE
	item_t pattern = pop_type(vm, STRING);
	item_t subject = pop_type(vm, STRING);

	const char *error;
	int erroffset;
	int ovector[STRTMP];
	pcre_extra *extra = NULL;

	pcre *re = pcre_compile(pattern.str, PCRE_DOTALL|PCRE_UTF8, &error, &erroffset, 0);
	ensure(vm, re, "pcre_compile: %s", pattern.str);

#ifdef PCRE_STUDY_JIT_COMPILE
	error = NULL;
	extra = pcre_study(re, PCRE_STUDY_JIT_COMPILE, &error);
	ensure(vm, extra && !error, "pcre_study: %s", pattern.str);
#endif

	int matches = pcre_exec(re, extra, subject.str, strlen(subject.str), 0, 0, ovector, sizeof(ovector));

	if (matches < 0) {
		if (extra)
			pcre_free_study(extra);
		pcre_free(re);
		return;
	}

	if (matches == 0) {
		matches = sizeof(ovector)/3;
	}

	for (int i = 0; i < matches; i++) {
		int offset = ovector[2*i];
		int length = ovector[2*i+1] - offset;
		push(vm, string(vm, substr(vm, subject.str, offset, length)));
	}

	if (extra)
		pcre_free_study(extra);
	pcre_free(re);

#else
	ensure(vm, 0, "matching not enabled; rebuild with -DPCRE");
#endif
}

static void op_sort(rela_vm* vm) {
	item_t a = pop_type(vm, VECTOR);
	if (vec_size(vm, a.vec) > 0)
		vec_sort(vm, a.vec, 0, vec_size(vm, a.vec)-1);
	push(vm, a);
}

static void op_assert(rela_vm* vm) {
	ensure(vm, depth(vm) && truth(vm, top(vm)), "assert");
}

static void op_fname(rela_vm* vm) {
	item_t key = literal(vm);
	item_t* val = find(vm, key);

	char tmp[STRTMP];
	ensure(vm, val, "unknown name: %s", tmptext(vm, key, tmp, sizeof(tmp)));

	push(vm, *val);
}

static void op_gname(rela_vm* vm) {
	item_t key = literal(vm);
	item_t src = pop(vm);
	push(vm, get(vm, src, key));
}

static void op_cfunc(rela_vm* vm) {
	item_t* cache = &vm->cache.cfunc[*cache_slot(vm)];
	if (cache->type == SUBROUTINE || cache->type == CALLBACK) {
		call(vm, *cache);
		return;
	}
	item_t key = literal(vm);
	item_t* val = find(vm, key);

	char tmp[STRTMP];
	ensure(vm, val, "unknown name: %s", tmptext(vm, key, tmp, sizeof(tmp)));

	*cache = *val;
	call(vm, *cache);
}

// compression of mark,lit,lit,assign,limit0
static void op_assignp(rela_vm* vm) {
	assign(vm, literal(vm), pop(vm));
}

// lit,assign0
static void op_assignl(rela_vm* vm) {
	// indexed from the base of the current subframe
	assign(vm, literal(vm), depth(vm) ? *item(vm, 0): nil(vm));
}

// dups
static void op_copies(rela_vm* vm) {
	for (int i = 0, l = literal_int(vm); i < l; i++) push(vm, top(vm));
}

// apply op direct to variable
static void op_update(rela_vm* vm) {
	item_t* val = find(vm, literal(vm));

	char tmp[STRTMP];
	ensure(vm, val, "unknown name: %s", tmptext(vm, literal(vm), tmp, sizeof(tmp)));

	push(vm, *val); tick(vm); *val = pop(vm);
}

typedef struct {
	const char *name;
	bool lib;
	rela_callback func;
} func_t;

static void nop(rela_vm* vm) {
}

func_t funcs[OPERATIONS] = {
	[OP_STOP]      = { .name = "stop",      .lib = false, .func = op_stop,      },
	[OP_PRINT]     = { .name = "print",     .lib = true,  .func = op_print,     },
	[OP_COROUTINE] = { .name = "coroutine", .lib = true,  .func = op_coroutine, },
	[OP_RESUME]    = { .name = "resume",    .lib = true,  .func = op_resume,    },
	[OP_YIELD]     = { .name = "yield",     .lib = true,  .func = op_yield,     },
	[OP_CALL]      = { .name = "call",      .lib = false, .func = op_call,      },
	[OP_RETURN]    = { .name = "return",    .lib = false, .func = op_return,    },
	[OP_GLOBAL]    = { .name = "global",    .lib = false, .func = op_global,    },
	[OP_META_SET]  = { .name = "setmeta",   .lib = true,  .func = op_meta_set,  },
	[OP_META_GET]  = { .name = "getmeta",   .lib = true,  .func = op_meta_get,  },
	[OP_VECTOR]    = { .name = "vector",    .lib = false, .func = op_vector,    },
	[OP_VPUSH]     = { .name = "vpush",     .lib = false, .func = op_vpush,     },
	[OP_MAP]       = { .name = "map",       .lib = false, .func = op_map,       },
	[OP_UNMAP]     = { .name = "unmap",     .lib = false, .func = op_unmap,     },
	[OP_MARK]      = { .name = "mark",      .lib = false, .func = op_mark,      },
	[OP_LIMIT]     = { .name = "limit",     .lib = false, .func = op_limit,     },
	[OP_LOOP]      = { .name = "loop",      .lib = false, .func = op_loop,      },
	[OP_UNLOOP]    = { .name = "unloop",    .lib = false, .func = op_unloop,    },
	[OP_CLEAN]     = { .name = "clean",     .lib = false, .func = op_clean,     },
	[OP_BREAK]     = { .name = "break",     .lib = false, .func = op_break,     },
	[OP_CONTINUE]  = { .name = "continue",  .lib = false, .func = op_continue,  },
	[OP_JMP]       = { .name = "jmp",       .lib = false, .func = op_jmp,       },
	[OP_JFALSE]    = { .name = "jfalse",    .lib = false, .func = op_jfalse,    },
	[OP_JTRUE]     = { .name = "jtrue",     .lib = false, .func = op_jtrue,     },
	[OP_FOR]       = { .name = "for",       .lib = false, .func = op_for,       },
	[OP_NIL]       = { .name = "nil",       .lib = false, .func = op_nil,       },
	[OP_COPY]      = { .name = "copy",      .lib = false, .func = op_copy,      },
	[OP_SHUNT]     = { .name = "shunt",     .lib = false, .func = op_shunt,     },
	[OP_SHIFT]     = { .name = "shift",     .lib = false, .func = op_shift,     },
	[OP_TRUE]      = { .name = "true",      .lib = false, .func = op_true,      },
	[OP_FALSE]     = { .name = "false",     .lib = false, .func = op_false,     },
	[OP_LIT]       = { .name = "lit",       .lib = false, .func = op_lit,       },
	[OP_ASSIGN]    = { .name = "assign",    .lib = false, .func = op_assign,    },
	[OP_FIND]      = { .name = "find",      .lib = false, .func = op_find,      },
	[OP_SET]       = { .name = "set",       .lib = false, .func = op_set,       },
	[OP_GET]       = { .name = "get",       .lib = false, .func = op_get,       },
	[OP_COUNT]     = { .name = "count",     .lib = false, .func = op_count,     },
	[OP_DROP]      = { .name = "drop",      .lib = false, .func = op_drop,      },
	[OP_ADD]       = { .name = "add",       .lib = false, .func = op_add,       },
	[OP_NEG]       = { .name = "neg",       .lib = false, .func = op_neg,       },
	[OP_SUB]       = { .name = "sub",       .lib = false, .func = op_sub,       },
	[OP_MUL]       = { .name = "mul",       .lib = false, .func = op_mul,       },
	[OP_DIV]       = { .name = "div",       .lib = false, .func = op_div,       },
	[OP_MOD]       = { .name = "mod",       .lib = false, .func = op_mod,       },
	[OP_NOT]       = { .name = "not",       .lib = false, .func = op_not,       },
	[OP_EQ]        = { .name = "eq",        .lib = false, .func = op_eq,        },
	[OP_NE]        = { .name = "ne",        .lib = false, .func = op_ne,        },
	[OP_LT]        = { .name = "lt",        .lib = false, .func = op_lt,        },
	[OP_LTE]       = { .name = "lte",       .lib = false, .func = op_lte,       },
	[OP_GT]        = { .name = "gt",        .lib = false, .func = op_gt,        },
	[OP_GTE]       = { .name = "gte",       .lib = false, .func = op_gte,       },
	[OP_AND]       = { .name = "and",       .lib = false, .func = nop,          },
	[OP_OR]        = { .name = "or",        .lib = false, .func = nop,          },
	[OP_CONCAT]    = { .name = "concat",    .lib = false, .func = op_concat,    },
	[OP_UNPACK]    = { .name = "unpack",    .lib = false, .func = op_unpack,    },
	[OP_MATCH]     = { .name = "match",     .lib = false, .func = op_match,     },
	[OP_SORT]      = { .name = "sort",      .lib = true,  .func = op_sort,      },
	[OP_PID]       = { .name = "pid",       .lib = false, .func = op_pid,       },
	[OP_ASSERT]    = { .name = "assert",    .lib = true,  .func = op_assert,    },
	[OP_TYPE]      = { .name = "type",      .lib = true,  .func = op_type,      },
	[OP_GC]        = { .name = "collect",   .lib = true,  .func = gc,           },
	// math
	[OP_SIN]       = { .name = "sin",       .lib = true,  .func = op_sin,       },
	[OP_COS]       = { .name = "cos",       .lib = true,  .func = op_cos,       },
	[OP_TAN]       = { .name = "tan",       .lib = true,  .func = op_tan,       },
	[OP_ASIN]      = { .name = "asin",      .lib = true,  .func = op_asin,      },
	[OP_ACOS]      = { .name = "acos",      .lib = true,  .func = op_acos,      },
	[OP_ATAN]      = { .name = "atan",      .lib = true,  .func = op_atan,      },
	[OP_COSH]      = { .name = "cosh",      .lib = true,  .func = op_cosh,      },
	[OP_SINH]      = { .name = "sinh",      .lib = true,  .func = op_sinh,      },
	[OP_TANH]      = { .name = "tanh",      .lib = true,  .func = op_tanh,      },
	[OP_CEIL]      = { .name = "ceil",      .lib = true,  .func = op_ceil,      },
	[OP_FLOOR]     = { .name = "floor",     .lib = true,  .func = op_floor,     },
	[OP_SQRT]      = { .name = "sqrt",      .lib = true,  .func = op_sqrt,      },
	[OP_ABS]       = { .name = "abs",       .lib = true,  .func = op_abs,       },
	[OP_ATAN2]     = { .name = "atan2",     .lib = true,  .func = op_atan2,     },
	[OP_LOG]       = { .name = "log",       .lib = true,  .func = op_log,       },
	[OP_LOG10]     = { .name = "log10",     .lib = true,  .func = op_log10,     },
	[OP_POW]       = { .name = "pow",       .lib = true,  .func = op_pow,       },
	[OP_MIN]       = { .name = "min",       .lib = true,  .func = op_min,       },
	[OP_MAX]       = { .name = "max",       .lib = true,  .func = op_max,       },
	// peephole
	[OPP_FNAME]    = { .name = "fname",     .lib = false, .func = op_fname,     },
	[OPP_GNAME]    = { .name = "gname",     .lib = false, .func = op_gname,     },
	[OPP_CFUNC]    = { .name = "cfunc",     .lib = false, .func = op_cfunc,     },
	[OPP_ASSIGNP]  = { .name = "assignp",   .lib = false, .func = op_assignp,   },
	[OPP_ASSIGNL]  = { .name = "assignl",   .lib = false, .func = op_assignl,   },
	[OPP_MUL_LIT]  = { .name = "litmul",    .lib = false, .func = op_mul_lit,   },
	[OPP_ADD_LIT]  = { .name = "litadd",    .lib = false, .func = op_add_lit,   },
	[OPP_COPIES]   = { .name = "copies",    .lib = false, .func = op_copies,    },
	[OPP_UPDATE]   = { .name = "update",    .lib = false, .func = op_update,    },
};

static void decompile(rela_vm* vm, code_t* c) {
	char tmp[STRTMP];
	const char *str = tmptext(vm, c->item, tmp, sizeof(tmp));
	fprintf(stderr, "%04ld  %3d  %-10s  %s\n", c - vm->code.cells, c->cache, funcs[c->op].name, str);
	fflush(stderr);
}

static void jit_byte(rela_vm* vm, unsigned char b) {
	unsigned char *p = vm->jit.code + vm->jit.depth;
	*p = b;
	vm->jit.depth++;
}

static void jit_int(rela_vm* vm, int i) {
	int* p = (int*)(vm->jit.code + vm->jit.depth);
	memcpy(p, &i, 4);
	vm->jit.depth += 4;
}

static void jit_addr(rela_vm* vm, void* a) {
	void** p = (void*)(vm->jit.code + vm->jit.depth);
	memcpy(p, &a, 8);
	vm->jit.depth += 8;
}

void hi(int i) {
	fprintf(stderr, "hi %d\n", i);
	fflush(stderr);
}

static void jit(rela_vm* vm) {
#ifdef __linux__
	vm->jit.limit = vm->code.depth * 128;

	vm->jit.code = mmap(0, vm->jit.limit, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ensure(vm, vm->jit.code, "mmap %d", errno);

	vm->jit.iptrx = calloc(vm->code.depth, sizeof(void*));
	ensure(vm, vm->jit.iptrx, "oom");

	// function prefix
	// callee-saved registers
	jit_byte(vm, 0x55); // push rbp
	jit_byte(vm, 0x53); // push rbx
	jit_byte(vm, 0x41); // push r15
	jit_byte(vm, 0x57);
	jit_byte(vm, 0x41); // push r14
	jit_byte(vm, 0x56);
	jit_byte(vm, 0x41); // push r13
	jit_byte(vm, 0x55);
	jit_byte(vm, 0x41); // push r12
	jit_byte(vm, 0x54);

	jit_byte(vm, 0x6a); // push qword 0
	jit_byte(vm, 0x00);

	jit_byte(vm, 0x48); // mov rbp, rsp
	jit_byte(vm, 0x89);
	jit_byte(vm, 0xe5);

//	for (int i = 0, l = vm->jit.depth; i < l; ) {
//		for (int j = i+8; i < j; i++) {
//			fprintf(stderr, "%02x ", vm->jit.code[i]);
//		}
//		fprintf(stderr, " ");
//		for (int j = i+8; i < j; i++) {
//			fprintf(stderr, "%02x ", vm->jit.code[i]);
//		}
//		fprintf(stderr, "\n");
//	}
//
//	fprintf(stderr, "%08lx\n", (size_t)hi);

	jit_byte(vm, 0x49); // mov r15,vm
	jit_byte(vm, 0xbf);
	jit_addr(vm, vm);
	jit_byte(vm, 0x49); // mov r14,&routine
	jit_byte(vm, 0xbe);
	jit_addr(vm, &vm->routine);
	jit_byte(vm, 0x49); // mov r13,vm->jit.iptrx
	jit_byte(vm, 0xbd);
	jit_addr(vm, vm->jit.iptrx);

//	jit_byte(vm, 0xeb);
//	jit_byte(vm, 0x12);
//
//	// <debug>
//	int debug = vm->code.depth;
//	jit_byte(vm, 0x48);
//	jit_byte(vm, 0xb8);
//	jit_addr(vm, hi);
//	jit_byte(vm, 0xff);
//	jit_byte(vm, 0xd0);
//	jit_byte(vm, 0xc3);
//	// </debug>

	for (int i = 0, l = vm->code.depth; i < l; i++) {
		vm->jit.iptrx[i] = &vm->jit.code[vm->jit.depth];
		code_t code = vm->code.cells[i];

//		jit_byte(vm, 0xbf);
//		jit_int(vm, i);
//		jit_byte(vm, 0x48);
//		jit_byte(vm, 0xb8);
//		jit_addr(vm, hi);
//		jit_byte(vm, 0xff);
//		jit_byte(vm, 0xd0);

		switch (code.op) {
			case OP_STOP:
				// function suffix
				jit_byte(vm, 0x58); // align rsp
				jit_byte(vm, 0x41); // pop r15
				jit_byte(vm, 0x5f);
				jit_byte(vm, 0x41); // pop r14
				jit_byte(vm, 0x5e);
				jit_byte(vm, 0x41); // pop r13
				jit_byte(vm, 0x5d);
				jit_byte(vm, 0x41); // pop r12
				jit_byte(vm, 0x5c);
				jit_byte(vm, 0x5b); // pop rbx
				jit_byte(vm, 0x5d); // pop rbp
				jit_byte(vm, 0xc3); // ret
				break;

//			case OP_PRINT:
//			case OP_GLOBAL:
//			case OP_META_SET:
//			case OP_META_GET:
//			case OP_VECTOR:
//			case OP_VPUSH:
//			case OP_MAP:
//			case OP_UNMAP:
//			case OP_MARK:
//			case OP_LIMIT:
//			case OP_CLEAN:
//			case OP_NIL:
//			case OP_COPY:
//			case OP_SHUNT:
//			case OP_SHIFT:
//			case OP_TRUE:
//			case OP_FALSE:
//			case OP_LIT:
//			case OP_ASSIGN:
//			case OP_FIND:
//			case OP_SET:
//			case OP_GET:
//			case OP_COUNT:
//			case OP_DROP:
//			case OP_ADD:
//			case OP_NEG:
//			case OP_SUB:
//			case OP_MUL:
//			case OP_DIV:
//			case OP_MOD:
//			case OP_NOT:
//			case OP_EQ:
//			case OP_NE:
//			case OP_LT:
//			case OP_LTE:
//			case OP_GT:
//			case OP_GTE:
//			case OP_AND:
//			case OP_OR:
//			case OP_CONCAT:
//			case OP_UNPACK:
//			case OP_MATCH:
//			case OP_SORT:
//			case OP_PID:
//			case OP_ASSERT:
//			case OP_TYPE:
//			case OP_GC:
//			case OP_SIN:
//			case OP_COS:
//			case OP_TAN:
//			case OP_ASIN:
//			case OP_ACOS:
//			case OP_ATAN:
//			case OP_COSH:
//			case OP_SINH:
//			case OP_TANH:
//			case OP_CEIL:
//			case OP_FLOOR:
//			case OP_SQRT:
//			case OP_ABS:
//			case OP_ATAN2:
//			case OP_LOG:
//			case OP_LOG10:
//			case OP_POW:
//			case OP_MIN:
//			case OP_MAX:
//			case OPP_FNAME:
//			case OPP_GNAME:
//			case OPP_ASSIGNP:
//			case OPP_ASSIGNL:
			case OPP_MUL_LIT:
				// fetch routine
				jit_byte(vm, 0x4d); // mov r12,[r14]
				jit_byte(vm, 0x8b);
				jit_byte(vm, 0x26);
				// lea r8,[r12+stack.cells]
				jit_byte(vm, 0x4d);
				jit_byte(vm, 0x8d);
				jit_byte(vm, 0x84);
				jit_byte(vm, 0x24);
				jit_int(vm, offsetof(cor_t, stack.cells));
				// mov r9,[r12+stack.depth]
				jit_byte(vm, 0x4d);
				jit_byte(vm, 0x8b);
				jit_byte(vm, 0x8c);
				jit_byte(vm, 0x24);
				jit_int(vm, offsetof(cor_t, stack.depth));
				// dec r9
				jit_byte(vm, 0x49);
				jit_byte(vm, 0xff);
				jit_byte(vm, 0xc9);
				// shl r9,4
				assert(sizeof(item_t) == 16);
				jit_byte(vm, 0x49);
				jit_byte(vm, 0xc1);
				jit_byte(vm, 0xe1);
				jit_byte(vm, 0x04);
				// add r9,r8
				jit_byte(vm, 0x4d);
				jit_byte(vm, 0x01);
				jit_byte(vm, 0xc1);
				// add r9,offsetinum
				jit_byte(vm, 0x49);
				jit_byte(vm, 0x81);
				jit_byte(vm, 0xc1);
				jit_int(vm, offsetof(item_t, inum));
				// mov rax,[r9]
				jit_byte(vm, 0x49);
				jit_byte(vm, 0x8b);
				jit_byte(vm, 0x01);
				// imul rax,n
				jit_byte(vm, 0x48);
				jit_byte(vm, 0x69);
				jit_byte(vm, 0xc0);
				jit_int(vm, code.item.inum);
				// mov [r9],rax
				jit_byte(vm, 0x49);
				jit_byte(vm, 0x89);
				jit_byte(vm, 0x01);
				break;
//			case OPP_ADD_LIT:
//			case OPP_COPIES:
//			case OPP_UPDATE:

			case OP_JMP:
				// mov rax,[r13+ip]
				jit_byte(vm, 0x49);
				jit_byte(vm, 0x8b);
				jit_byte(vm, 0x85);
				jit_int(vm, code.item.inum*8);
				// jmp rax
				jit_byte(vm, 0xff);
				jit_byte(vm, 0xe0);
				break;

			case OP_COROUTINE:
			case OP_RESUME:
			case OP_YIELD:
			case OP_CALL:
			case OP_RETURN:
			case OP_LOOP:
			case OP_UNLOOP:
			case OP_BREAK:
			case OP_CONTINUE:
//			case OP_JMP:
			case OP_JFALSE:
			case OP_JTRUE:
			case OP_FOR:
			case OPP_CFUNC:
				// fetch routine
				jit_byte(vm, 0x4d); // mov r12,[r14]
				jit_byte(vm, 0x8b);
				jit_byte(vm, 0x26);
				// set ip
				jit_byte(vm, 0x41); // mov dword [r12+ipoffset],n
				jit_byte(vm, 0xc7);
				jit_byte(vm, 0x84);
				jit_byte(vm, 0x24);
				jit_int(vm, offsetof(cor_t, ip));
				jit_int(vm, i+1);
				// arg1 vm
				jit_byte(vm, 0x4c); // mov rdi,r15
				jit_byte(vm, 0x89);
				jit_byte(vm, 0xff);
				// call
				jit_byte(vm, 0x48); // mov rax,fn
				jit_byte(vm, 0xb8);
				jit_addr(vm, funcs[code.op].func);
				jit_byte(vm, 0xff); // call rax
				jit_byte(vm, 0xd0);
				// fetch routine
				jit_byte(vm, 0x4d); // mov r12,[r14]
				jit_byte(vm, 0x8b);
				jit_byte(vm, 0x26);
				// index iptrx
				jit_byte(vm, 0x41); // mov eax,[r12+ipoffset]
				jit_byte(vm, 0x8b);
				jit_byte(vm, 0x84);
				jit_byte(vm, 0x24);
				jit_int(vm, offsetof(cor_t, ip));
				jit_byte(vm, 0xc1); // shl eax,3
				jit_byte(vm, 0xe0);
				jit_byte(vm, 0x03);
				jit_byte(vm, 0x4c); // add rax,r13
				jit_byte(vm, 0x01);
				jit_byte(vm, 0xe8);
				// jmp ip
				jit_byte(vm, 0xff); // jmp [rax]
				jit_byte(vm, 0x20);
				break;

			default:
				// fetch routine
				jit_byte(vm, 0x4d); // mov r12,[r14]
				jit_byte(vm, 0x8b);
				jit_byte(vm, 0x26);
				// set ip
				jit_byte(vm, 0x41); // mov dword [r12+ipoffset],n
				jit_byte(vm, 0xc7);
				jit_byte(vm, 0x84);
				jit_byte(vm, 0x24);
				jit_int(vm, offsetof(cor_t, ip));
				jit_int(vm, i+1);
				// arg1 vm
				jit_byte(vm, 0x4c); // mov rsi,r15
				jit_byte(vm, 0x89);
				jit_byte(vm, 0xff);
				// call
				jit_byte(vm, 0x48); // mov qword rax,fn
				jit_byte(vm, 0xb8);
				jit_addr(vm, funcs[code.op].func);
				jit_byte(vm, 0xff); // call rax
				jit_byte(vm, 0xd0);
				break;
		}
	}

	// function suffix
	jit_byte(vm, 0x58); // align rsp
	jit_byte(vm, 0x41); // pop r15
	jit_byte(vm, 0x5f);
	jit_byte(vm, 0x41); // pop r14
	jit_byte(vm, 0x5e);
	jit_byte(vm, 0x41); // pop r13
	jit_byte(vm, 0x5d);
	jit_byte(vm, 0x41); // pop r12
	jit_byte(vm, 0x5c);
	jit_byte(vm, 0x5b); // pop rbx
	jit_byte(vm, 0x5d); // pop rbp
	jit_byte(vm, 0xc3); // ret

	fprintf(stderr, "code %u jit %u %u\n", vm->code.depth, vm->jit.depth, vm->jit.limit);

	ensure(vm, 0 == mprotect(vm->jit.code, vm->jit.limit, PROT_READ | PROT_EXEC), "mprotect %d", errno);
#endif
}

#ifdef TRACE
// tick() tracing tmptext() may call meta-methods
// with tick() recursion. only output the trace at
// the top level.
bool tracing;
#endif

static bool tick(rela_vm* vm) {
	int ip = vm->routine->ip++;
	assert(ip >= 0 && ip < vm->code.depth);
	int opcode = vm->code.cells[ip].op;

	#ifdef TRACE
		if (!tracing) {
			tracing = true;
			code_t* c = &vm->code.cells[ip];
			char tmpA[STRTMP];
			const char *str = tmptext(vm, c->item, tmpA, sizeof(tmpA));
			for (int i = 0, l = vm->routine->marks.depth; i < l; i++)
				fprintf(stderr, "  ");
			fprintf(stderr, "%04ld  %-10s  %-10s", c - vm->code.cells, funcs[c->op].name, str);
			fflush(stderr);
			tracing = false;
		}
	#endif

	switch (opcode) {
		// <order-important>
		case OP_STOP: { return false; break; }
		case OP_JMP: { op_jmp(vm); break; }
		case OP_FOR: { op_for(vm); break; }
		case OP_PID: { op_pid(vm); break; }
		case OP_LIT: { op_lit(vm); break; }
		case OP_MARK: { op_mark(vm); break; }
		case OP_LIMIT: { op_limit(vm); break; }
		case OP_CLEAN: { op_clean(vm); break; }
		case OP_RETURN: { op_return(vm); break; }
		case OPP_FNAME: { op_fname(vm); break; }
		case OPP_CFUNC: { op_cfunc(vm); break; }
		case OPP_ASSIGNL: { op_assignl(vm); break; }
		case OPP_ASSIGNP: { op_assignp(vm); break; }
		case OPP_MUL_LIT: { op_mul_lit(vm); break; }
		case OPP_ADD_LIT: { op_add_lit(vm); break; }
		// </order-important>
		default: funcs[opcode].func(vm); break;
	}

	#ifdef TRACE
		if (!tracing) {
			tracing = true;
			fprintf(stderr, "[");
			for (int i = 0, l = vm->routine->stack.depth; i < l; i++) {
				if (i == l-depth(vm))
					fprintf(stderr, "|");
				char tmpB[STRTMP];
				fprintf(stderr, "%s%s",
					tmptext(vm, *stack_cell(vm, i), tmpB, sizeof(tmpB)),
					(i < l-1 ? ", ":"")
				);
			}
			fprintf(stderr, "]\n");
			fflush(stderr);
			tracing = false;
		}
	#endif

	return true;
}

static void method(rela_vm* vm, item_t func, int argc, item_t* argv, int retc, item_t* retv) {
	ensure(vm, func.type == SUBROUTINE || func.type == CALLBACK, "invalid method");

	cor_t* cor = vm->routine;
	int frame = cor->frames.depth;

	op_mark(vm);

	for (int i = 0; i < argc; i++) push(vm, argv[i]);

	call(vm, func);

	if (func.type == SUBROUTINE) {
		while (tick(vm)) {
			if (vm->routine != cor) continue;
			if (frame < cor->frames.depth) continue;
			break;
		}
	}

	for (int i = 0; i < retc; i++) {
		retv[i] = i < depth(vm) ? *item(vm, i): nil(vm);
	}

	limit(vm, 0);
}

static void destroy(rela_vm* vm) {
	if (setjmp(vm->jmp)) {
		fprintf(stderr, "%s\n", vm->err);
		return;
	}

	vm->code.depth = 0;
	vm->scope_core = NULL;
	reset(vm);

	free(vm->code.cells);
	free(vm->routines.items);
	free(vm->modules.entries.items);
	free(vm->modules.names.items);

	while (vm->stringsA.depth > 0) free(vm->stringsA.cells[--vm->stringsA.depth]);
	while (vm->stringsB.depth > 0) free(vm->stringsB.cells[--vm->stringsB.depth]);
	free(vm->stringsA.cells);
	free(vm->stringsB.cells);

	pool_clear(vm, &vm->maps);
	pool_clear(vm, &vm->vecs);
	pool_clear(vm, &vm->cors);
	pool_clear(vm, &vm->data);

	free(vm);
}

// Public API

rela_vm* rela_create(const char* src, size_t registrations, const rela_register* registry, void* custom) {
	rela_module main = (rela_module){.name = "main", .source = src};
	return rela_create_ex(1, &main, registrations, registry, custom);
}

rela_vm* rela_create_ex(size_t modules, const rela_module* modistry, size_t registrations, const rela_register* registry, void* custom) {
	rela_vm* vm = calloc(sizeof(rela_vm),1);
	if (!vm) return NULL;

	vm->vecs.page = 1024;
	vm->vecs.object = sizeof(vec_t);
	vm->maps.page = 512;
	vm->maps.object = sizeof(map_t);
	vm->cors.page = 128;
	vm->cors.object = sizeof(cor_t);
	vm->data.page = 128;
	vm->data.object = sizeof(data_t);

	vm->custom = custom;
	vm->scope_core = map_allot(vm);

	if (setjmp(vm->jmp)) {
		fprintf(stderr, "%s\n", vm->err);
		destroy(vm);
		return NULL;
	}

	item_t lib = nil(vm);
	if (!map_get(vm, vm->scope_core, string(vm, "lib"), &lib)) {
		lib = (item_t){.type = MAP, .map = map_allot(vm)};
		map_set(vm, vm->scope_core, string(vm, "lib"), lib);
	}

	for (int opcode = OP_STOP; opcode < OPERATIONS; opcode++) {
		func_t* fn = &funcs[opcode];
		ensure(vm, fn->name && fn->func, "%d", opcode);
		if (!fn->lib) continue;
		map_set(vm, lib.map, string(vm, fn->name), (item_t){.type = CALLBACK, .cb = funcs[opcode].func});
	}

	map_set(vm, vm->scope_core, string(vm, "print"), (item_t){.type = CALLBACK, .cb = op_print});

	vm->code.start = vm->code.depth;

	for (int i = 0; i < registrations; i++) {
		const rela_register* reg = &registry[i];
		map_set(vm, vm->scope_core,
			string(vm, (char*)reg->name),
			(item_t){.type = CALLBACK, .cb = reg->func}
		);
	}

	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor_allot(vm)});
	vm->routine = vec_top(vm, &vm->routines).cor;
	op_mark(vm);

	for (int i = 0, l = modules; i < l; i++) {
		vec_push(vm, &vm->modules.names, string(vm, modistry[i].name));
		vec_push(vm, &vm->modules.entries, integer(vm, vm->code.depth));
		source(vm, modistry[i].source);
		assert(!vm->routine->stack.depth);
		compile(vm, OP_STOP, nil(0));
	}

	limit(vm, 0);
	vec_pop(vm, &vm->routines);
	vm->routine = NULL;

	while (vm->nodes.cells && vm->nodes.depth > 0)
		free(vm->nodes.cells[--vm->nodes.depth]);
	free(vm->nodes.cells);
	vm->nodes.cells = NULL;

	memmove(&vm->stringsB, &vm->stringsA, sizeof(string_region_t));
	memset(&vm->stringsA, 0, sizeof(string_region_t));

	for (int i = 0, l = vm->code.depth; i < l; i++) {
		code_t* code = &vm->code.cells[i];
		if (code->op == OPP_CFUNC) code->cache = vm->cache.cfuncs++;
	}

	jit(vm);
	gc(vm);
	return vm;
}

void* rela_custom(rela_vm* vm) {
	return vm->custom;
}

int rela_run(rela_vm* vm) {
	return rela_run_ex(vm, 1, (int[]){0});
}

int rela_run_ex(rela_vm* vm, int modules, int* modlist) {
	vm->cache.cfunc = calloc(vm->cache.cfuncs, sizeof(item_t));
	ensure(vm, vm->cache.cfunc, "oom");

	int wtf = setjmp(vm->jmp);
	if (wtf) {
		fprintf(stderr, "%s (", vm->err);
		fprintf(stderr, "ip %d", vec_size(vm, &vm->routines) ? vm->routine->ip: -1);
		fprintf(stderr, ")\n");
		reset(vm);
		return wtf;
	}

	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor_allot(vm)});
	vm->routine = vec_top(vm, &vm->routines).cor;
	vm->scope_global = map_allot(vm);

	for (int mod = 0; mod < modules; mod++) {
		ensure(vm, modlist[mod] < vec_size(vm, &vm->modules.entries), "invalid module %d", modlist[mod]);
		vm->routine->ip = vec_get(vm, &vm->modules.entries, modlist[mod]).inum;
		#ifdef __linux__
//			void (*call)() = vm->jit.iptrx[vm->routine->ip];
			void (*call)() = (void*)(vm->jit.code);
			call();
		#else
			while (tick(vm));
		#endif
	}
	reset(vm);
	return 0;
}

void rela_destroy(rela_vm* vm) {
	destroy(vm);
}

void rela_decompile(rela_vm* vm) {
	for (int i = 0, l = vm->code.depth; i < l; i++) decompile(vm, &vm->code.cells[i]);
}

void rela_collect(rela_vm* vm) {
	gc(vm);
}

const char* rela_error(rela_vm* vm) {
	return vm->err;
}

size_t rela_depth(rela_vm* vm) {
	return depth(vm);
}

void rela_push(rela_vm* vm, rela_item opaque) {
	push(vm, *((item_t*)&opaque));
}

rela_item rela_nil(rela_vm* vm) {
	push(vm, nil(vm));
	return rela_pop(vm);
}

rela_item rela_make_bool(rela_vm* vm, bool b) {
	push(vm, (item_t){.type = BOOLEAN, .flag = b});
	return rela_pop(vm);
}

rela_item rela_make_number(rela_vm* vm, double val) {
	push(vm, (item_t){.type = FLOAT, .fnum = val});
	return rela_pop(vm);
}

rela_item rela_make_integer(rela_vm* vm, int64_t val) {
	push(vm, integer(vm, val));
	return rela_pop(vm);
}

rela_item rela_make_string(rela_vm* vm, const char* str) {
	push(vm, string(vm, str));
	return rela_pop(vm);
}

rela_item rela_make_data(rela_vm* vm, void* ptr) {
	data_t* data = data_allot(vm); data->ptr = ptr;
	push(vm, (item_t){.type = USERDATA, .data = data});
	return rela_pop(vm);
}

rela_item rela_make_vector(rela_vm* vm) {
	push(vm, (item_t){.type = VECTOR, .vec = vec_allot(vm)});
	return rela_pop(vm);
}

rela_item rela_make_map(rela_vm* vm) {
	push(vm, (item_t){.type = MAP, .map = map_allot(vm)});
	return rela_pop(vm);
}

rela_item rela_make_callback(rela_vm* vm, rela_callback cb) {
	push(vm, (item_t){.type = CALLBACK, .cb = cb});
	return rela_pop(vm);
}

rela_item rela_pop(rela_vm* vm) {
	rela_item opaque;
	*((item_t*)&opaque) = pop(vm);
	return opaque;
}

rela_item rela_pick(rela_vm* vm, int pos) {
	rela_item opaque;
	*((item_t*)&opaque) = *item(vm, pos);
	return opaque;
}

rela_item rela_top(rela_vm* vm) {
	rela_item opaque;
	*((item_t*)&opaque) = top(vm);
	return opaque;
}

bool rela_is_nil(rela_vm* vm, rela_item opaque) {
	item_t item = *((item_t*)&opaque);
	return item.type == NIL;
}

bool rela_is_bool(rela_vm* vm, rela_item opaque) {
	item_t item = *((item_t*)&opaque);
	return item.type == BOOLEAN;
}

bool rela_is_number(rela_vm* vm, rela_item opaque) {
	item_t item = *((item_t*)&opaque);
	return item.type == FLOAT || item.type == INTEGER;
}

bool rela_is_integer(rela_vm* vm, rela_item opaque) {
	item_t item = *((item_t*)&opaque);
	return item.type == INTEGER;
}

bool rela_is_string(rela_vm* vm, rela_item opaque) {
	item_t item = *((item_t*)&opaque);
	return item.type == STRING;
}

bool rela_is_data(rela_vm* vm, rela_item opaque) {
	item_t item = *((item_t*)&opaque);
	return item.type == USERDATA;
}

bool rela_is_vector(rela_vm* vm, rela_item opaque) {
	item_t item = *((item_t*)&opaque);
	return item.type == VECTOR;
}

bool rela_is_map(rela_vm* vm, rela_item opaque) {
	item_t item = *((item_t*)&opaque);
	return item.type == MAP;
}

bool rela_truth(rela_vm* vm, rela_item opaque) {
	item_t item = *((item_t*)&opaque);
	return truth(vm, item);
}

size_t rela_count(rela_vm* vm, rela_item opaque) {
	item_t item = *((item_t*)&opaque);
	push(vm, item);
	op_count(vm);
	return pop(vm).inum;
}

rela_item rela_vector_get(rela_vm* vm, rela_item vec, int index) {
	item_t ivec = *((item_t*)&vec);
	push(vm, get(vm, ivec, integer(vm, index)));
	return rela_pop(vm);
}

void rela_vector_set(rela_vm* vm, rela_item vec, int index, rela_item val) {
	item_t ivec = *((item_t*)&vec);
	item_t ival = *((item_t*)&val);
	set(vm, ivec, integer(vm, index), ival);
}

rela_item rela_map_get(rela_vm* vm, rela_item map, rela_item key) {
	item_t imap = *((item_t*)&map);
	item_t ikey = *((item_t*)&key);
	push(vm, get(vm, imap, ikey));
	return rela_pop(vm);
}

rela_item rela_map_get_named(rela_vm* vm, rela_item map, const char* field) {
	return rela_map_get(vm, map, rela_make_string(vm, field));
}

void rela_map_set(rela_vm* vm, rela_item map, rela_item key, rela_item val) {
	item_t imap = *((item_t*)&map);
	item_t ikey = *((item_t*)&key);
	item_t ival = *((item_t*)&key);
	set(vm, imap, ikey, ival);
}

rela_item rela_map_key(rela_vm* vm, rela_item con, int index) {
	item_t icon = *((item_t*)&con);
	push(vm, icon);
	map_t* map = pop_type(vm, MAP).map;
	push(vm, vec_size(vm, &map->keys) > index ? vec_get(vm, &map->keys, index): nil(vm));
	return rela_pop(vm);
}

const char* rela_to_text(rela_vm* vm, rela_item opaque, char* tmp, size_t size) {
	item_t item = *((item_t*)&opaque);
	return tmptext(vm, item, tmp, size);
}

bool rela_to_bool(rela_vm* vm, rela_item opaque) {
	char tmp[STRTMP];
	item_t item = *((item_t*)&opaque);
	ensure(vm, item.type == BOOLEAN, "item is not a boolean: %s", tmptext(vm, item, tmp, sizeof(tmp)));
	return item.flag;
}

double rela_to_number(rela_vm* vm, rela_item opaque) {
	char tmp[STRTMP];
	item_t item = *((item_t*)&opaque);
	ensure(vm, item.type == FLOAT || item.type == INTEGER, "item is not a number: %s", tmptext(vm, item, tmp, sizeof(tmp)));
	return item.type == FLOAT ? item.fnum: item.inum;
}

int64_t rela_to_integer(rela_vm* vm, rela_item opaque) {
	char tmp[STRTMP];
	item_t item = *((item_t*)&opaque);
	ensure(vm, item.type == INTEGER, "item is not an integer: %s", tmptext(vm, item, tmp, sizeof(tmp)));
	return item.inum;
}

const char* rela_to_string(rela_vm* vm, rela_item opaque) {
	char tmp[STRTMP];
	item_t item = *((item_t*)&opaque);
	ensure(vm, item.type == STRING, "item is not a string: %s", tmptext(vm, item, tmp, sizeof(tmp)));
	return item.str;
}

void* rela_to_data(rela_vm* vm, rela_item opaque) {
	char tmp[STRTMP];
	item_t item = *((item_t*)&opaque);
	ensure(vm, item.type == USERDATA, "item is not userdata: %s", tmptext(vm, item, tmp, sizeof(tmp)));
	return item.data->ptr;
}

void rela_meta_set(rela_vm* vm, rela_item data, rela_item meta) {
	item_t ditem = *((item_t*)&data);
	item_t mitem = *((item_t*)&meta);
	push(vm, ditem);
	push(vm, mitem);
	op_meta_set(vm);
}

rela_item rela_core(rela_vm* vm) {
	push(vm, (item_t){.type = MAP, .map = vm->scope_core});
	return rela_pop(vm);
}

rela_item rela_global(rela_vm* vm) {
	push(vm, (item_t){.type = MAP, .map = vm->scope_global});
	return rela_pop(vm);
}

