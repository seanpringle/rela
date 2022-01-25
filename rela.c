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

#ifndef NDEBUG
#include <signal.h>
#endif

#ifdef PCRE
#include <pcre.h>
#endif

enum opcode_t {
	// <order important>
	OP_STOP=0, OP_JMP, OP_FOR, OP_PID, OP_LIT, OP_MARK, OP_LIMIT, OP_CLEAN, OP_RETURN,
	OPP_FNAME, OPP_CFUNC, OPP_ASSIGNL, OPP_ASSIGNP, OPP_MUL_LIT, OPP_ADD_LIT, OPP_GNAME, OPP_COPIES,
	// </order important>
	OP_PRINT, OP_COROUTINE, OP_RESUME, OP_YIELD, OP_CALL, OP_GLOBAL, OP_MAP, OP_VECTOR,
	OP_UNMAP, OP_LOOP, OP_UNLOOP, OP_BREAK, OP_CONTINUE, OP_JFALSE,
	OP_JTRUE, OP_NIL, OP_SHUNT, OP_SHIFT, OP_TRUE, OP_FALSE, OP_ASSIGN, OP_AND, OP_OR,
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

typedef struct {
	enum type_t type;
	union {
		int flag;
		int sub;
		int64_t inum;
		double fnum;
		const char* str;
		struct _vec_t* vec;
		struct _map_t* map;
		struct _cor_t* cor;
		struct _node_t* node;
		rela_callback cb;
		void* data;
	};
} item_t;

typedef struct _vec_t {
	item_t* items;
	int count;
	int buffer;
} vec_t; // vector

typedef struct _map_t {
	vec_t keys;
	vec_t vals;
} map_t;

#define LOCALS 16
#define PATH 8

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
	vec_t stack;  // arguments, results, opcode working
	vec_t other;  // temporary data moved off stack
	int ip;
	int state;
	struct {
		frame_t cells[32];
		int depth;
	} frames;
	struct {
		int cells[32];
		int depth;
	} marks;
	struct {
		int cells[32];
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
	bool index;
	bool field;
	bool control;
	bool single;
	vec_t* keys;
	vec_t* vals;
	int results;
	struct {
		int id;
		int ids[PATH];
		int depth;
	} fpath;
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

static int pool_index(pool_t* pool, void* ptr) {
	for (int page = 0; page < pool->depth/pool->page; page++) {
		if (pool->pages[page] <= ptr && ptr < pool->pages[page] + pool->object*pool->page) {
			return page*pool->page + (ptr - pool->pages[page])/pool->object;
		}
	}
	return -1;
}

static void* pool_ptr(pool_t* pool, int index) {
	assert(index < pool->depth);
	int page = index/pool->page;
	int cell = index%pool->page;
	return pool->pages[page] + (cell * pool->object);
}

static void* pool_allot_index(pool_t* pool, int index) {
	assert(index < pool->depth);
	pool->used[index] = true;

	void* ptr = pool_ptr(pool, index);
	memset(ptr, 0, pool->object);

	pool->next = index+1;
	pool->extant++;

	return ptr;
}

static void* pool_alloc(pool_t* pool) {
	for (int i = pool->next; i < pool->depth; i++) {
		if (!pool->used[i]) return pool_allot_index(pool, i);
	}
	for (int i = 0; i < pool->next; i++) {
		if (!pool->used[i]) return pool_allot_index(pool, i);
	}

	int index = pool->depth;
	int pages = pool->depth/pool->page + 1;

	pool->depth += pool->page;

	pool->used = realloc(pool->used, sizeof(bool) * pool->depth);
	memset(pool->used + sizeof(bool)*index, 0, sizeof(bool)*pool->page);

	pool->mark = realloc(pool->mark, sizeof(bool) * pool->depth);
	memset(pool->mark + sizeof(bool)*index, 0, sizeof(bool)*pool->page);

	pool->pages = realloc(pool->pages, pages * sizeof(void*));
	pool->pages[pages-1] = malloc(pool->object * pool->page);

	return pool_allot_index(pool, index);
}

static void pool_free(pool_t* pool, void* ptr) {
	int index = pool_index(pool, ptr);
	if (index >= 0) {
		memset(ptr, 0, pool->object);
		pool->used[index] = false;
		pool->extant--;
	}
	assert(pool->extant >= 0);
}

static void pool_clear(pool_t* pool) {
	for (int i = 0, l = pool->depth/pool->page; i < l; i++) {
		free(pool->pages[i]);
	}
	free(pool->pages);
	free(pool->used);
	free(pool->mark);
	memset(pool, 0, sizeof(pool_t));
}

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
} rela_vm;

typedef int (*strcb)(int);

static item_t nil(rela_vm* vm);
static bool equal(rela_vm* vm, item_t a, item_t b);
static bool less(rela_vm* vm, item_t a, item_t b);
static const char* tmptext(rela_vm* vm, item_t item, char* tmp, int size);
static int parse(rela_vm* vm, const char *source, int results, int mode);
static int parse_block(rela_vm* vm, const char *source, node_t *node);
static int parse_branch(rela_vm* vm, const char *source, node_t *node);
static int parse_arglist(rela_vm* vm, const char *source);
static int parse_node(rela_vm* vm, const char *source);

#ifdef NDEBUG
void explode(rela_vm* vm) { longjmp(vm->jmp, 1); }
#else
void explode(rela_vm* vm) { raise(SIGUSR1); }
#endif

#define ensure(vm,c,...) if (!(c)) { snprintf(vm->err, sizeof(vm->err), __VA_ARGS__); explode(vm); }

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

static void gc_mark_item(rela_vm* vm, item_t item) {
	if (item.type == STRING) gc_mark_str(vm, item.str);
	if (item.type == VECTOR) gc_mark_vec(vm, item.vec);
	if (item.type == MAP) gc_mark_map(vm, item.map);
	if (item.type == COROUTINE) gc_mark_cor(vm, item.cor);
}

static void gc_mark_str(rela_vm* vm, const char* str) {
	int index = str_lower_bound(vm, &vm->stringsA, str);
	if (index < vm->stringsA.depth && vm->stringsA.cells[index] == str) vm->stringsA.mark[index] = true;
}

static void gc_mark_vec(rela_vm* vm, vec_t* vec) {
	if (!vec) return;
	int index = pool_index(&vm->vecs, vec);
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
	int index = pool_index(&vm->maps, map);
	if (index >= 0) {
		if (vm->maps.mark[index]) return;
		vm->maps.mark[index] = true;
	}
	gc_mark_vec(vm, &map->keys);
	gc_mark_vec(vm, &map->vals);
}

static void gc_mark_cor(rela_vm* vm, cor_t* cor) {
	if (!cor) return;
	int index = pool_index(&vm->cors, cor);
	if (index >= 0) {
		if (vm->cors.mark[index]) return;
		vm->cors.mark[index] = true;
	}
	gc_mark_vec(vm, &cor->stack);
	gc_mark_vec(vm, &cor->other);
	gc_mark_item(vm, cor->map);
	for (int i = 0, l = cor->frames.depth; i < l; i++) {
		frame_t* frame = &cor->frames.cells[i];
		for (int j = 0; j < frame->locals.depth; j++) {
			gc_mark_item(vm, frame->locals.vals[j]);
		}
	}
}

// A naive mark-and-sweep collector that is never called implicitly
// at run-time. Can be explicitly triggered with "collect()" via
// script or with rela_collect() via callback.
static void gc(rela_vm* vm) {
	memset(vm->maps.mark, 0, sizeof(bool)*vm->maps.depth);
	memset(vm->vecs.mark, 0, sizeof(bool)*vm->vecs.depth);
	memset(vm->cors.mark, 0, sizeof(bool)*vm->cors.depth);
	vm->stringsA.mark = calloc(sizeof(bool),vm->stringsA.depth);

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
			vec_t* vec = pool_ptr(&vm->vecs, i);
			free(vec->items);
			pool_free(&vm->vecs, vec);
		}
	}

	for (int i = 0, l = vm->maps.depth; i < l; i++) {
		if (vm->maps.used[i] && !vm->maps.mark[i]) {
			map_t* map = pool_ptr(&vm->maps, i);
			free(map->keys.items);
			free(map->vals.items);
			pool_free(&vm->maps, map);
		}
	}

	for (int i = 0, l = vm->cors.depth; i < l; i++) {
		if (vm->cors.used[i] && !vm->cors.mark[i]) {
			cor_t* cor = pool_ptr(&vm->cors, i);
			free(cor->stack.items);
			free(cor->other.items);
			pool_free(&vm->cors, cor);
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
	return pool_alloc(&vm->vecs);
}

static map_t* map_allot(rela_vm* vm) {
	return pool_alloc(&vm->maps);
}

static cor_t* cor_allot(rela_vm* vm) {
	return pool_alloc(&vm->cors);
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

static void vec_shrink(rela_vm* vm, vec_t* vec, int size) {
	ensure(vm, vec->count >= size, "vec_pop underflow");
	vec->count = size;
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
	if (item) {
		assert(item->type != NIL);
		*val = *item;
		return true;
	}
	return false;
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
	memmove(cpy, str, len);

	vm->stringsA.cells = realloc(vm->stringsA.cells, ++vm->stringsA.depth * sizeof(char*));

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
	while (vec_size(vm, &vm->routines)) vec_pop(vm, &vm->routines);
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

static bool equal(rela_vm* vm, item_t a, item_t b) {
	if (a.type == b.type) {
		if (a.type == INTEGER) return a.inum == b.inum;
		if (a.type == FLOAT) return fabs(a.fnum - b.fnum) < DBL_EPSILON*10;
		if (a.type == STRING) return a.str == b.str; // .str must use strintern
		if (a.type == BOOLEAN) return a.flag == b.flag;
		if (a.type == VECTOR) return a.vec == b.vec;
		if (a.type == MAP) return a.map == b.map;
		if (a.type == SUBROUTINE) return a.sub == b.sub;
		if (a.type == COROUTINE) return a.cor == b.cor;
		if (a.type == USERDATA) return a.data == b.data;
		if (a.type == NODE) return a.node == b.node;
		if (a.type == NIL) return true;
	}
	return false;
}

static bool less(rela_vm* vm, item_t a, item_t b) {
	if (a.type == b.type) {
		if (a.type == INTEGER) return a.inum < b.inum;
		if (a.type == FLOAT) return a.fnum < b.fnum;
		if (a.type == STRING) return a.str != b.str && strcmp(a.str, b.str) < 0;
		if (a.type == VECTOR) return vec_size(vm, a.vec) < vec_size(vm, b.vec);
		if (a.type == MAP) return vec_size(vm, &a.map->keys) < vec_size(vm, &b.map->keys);
	}
	return false;
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

static int count(rela_vm* vm, item_t a) {
	if (a.type == INTEGER) return a.inum;
	if (a.type == FLOAT) return floor(a.fnum);
	if (a.type == STRING) return strlen(a.str);
	if (a.type == VECTOR) return vec_size(vm, a.vec);
	if (a.type == MAP) return vec_size(vm, &a.map->keys);
	return 0;
}

static item_t add(rela_vm* vm, item_t a, item_t b) {
	if (a.type == INTEGER && b.type == INTEGER) return (item_t){.type = INTEGER, .inum = a.inum + b.inum};
	if (a.type == INTEGER && b.type == FLOAT) return (item_t){.type = INTEGER, .inum = a.inum + b.fnum};
	if (a.type == FLOAT && b.type == INTEGER) return (item_t){.type = FLOAT, .fnum = a.fnum + b.inum};
	if (a.type == FLOAT && b.type == FLOAT) return (item_t){.type = FLOAT, .fnum = a.fnum + b.fnum};
	return nil(vm);
}

static item_t multiply(rela_vm* vm, item_t a, item_t b) {
	if (a.type == INTEGER && b.type == INTEGER) return (item_t){.type = INTEGER, .inum = a.inum * b.inum};
	if (a.type == INTEGER && b.type == FLOAT) return (item_t){.type = INTEGER, .inum = a.inum * b.fnum};
	if (a.type == FLOAT && b.type == INTEGER) return (item_t){.type = FLOAT, .fnum = a.fnum * b.inum};
	if (a.type == FLOAT && b.type == FLOAT) return (item_t){.type = FLOAT, .fnum = a.fnum * b.fnum};
	return nil(vm);
}

static item_t divide(rela_vm* vm, item_t a, item_t b) {
	if (a.type == INTEGER && b.type == INTEGER) return (item_t){.type = INTEGER, .inum = a.inum / b.inum};
	if (a.type == INTEGER && b.type == FLOAT) return (item_t){.type = INTEGER, .inum = a.inum / b.fnum};
	if (a.type == FLOAT && b.type == INTEGER) return (item_t){.type = FLOAT, .fnum = a.fnum / b.inum};
	if (a.type == FLOAT && b.type == FLOAT) return (item_t){.type = FLOAT, .fnum = a.fnum / b.fnum};
	return nil(vm);
}

static const char* tmptext(rela_vm* vm, item_t a, char* tmp, int size) {
	if (a.type == STRING) return a.str;

	assert(a.type >= 0 && a.type < TYPES);

	if (a.type == NIL) snprintf(tmp, size, "nil");
	if (a.type == INTEGER) snprintf(tmp, size, "%ld", a.inum);
	if (a.type == FLOAT) snprintf(tmp, size, "%f", a.fnum);
	if (a.type == BOOLEAN) snprintf(tmp, size, "%s", a.flag ? "true": "false");
	if (a.type == SUBROUTINE) snprintf(tmp, size, "%s(%d)", type_names[a.type], a.sub);
	if (a.type == COROUTINE) snprintf(tmp, size, "%s", type_names[a.type]);
	if (a.type == CALLBACK) snprintf(tmp, size, "%s", type_names[a.type]);
	if (a.type == USERDATA) snprintf(tmp, size, "%s", type_names[a.type]);
	if (a.type == NODE) snprintf(tmp, size, "%s", type_names[a.type]);

	char subtmpA[STRTMP];
	char subtmpB[STRTMP];

	if (a.type == VECTOR) {
		int len = snprintf(tmp, size, "[");
		for (int i = 0, l = vec_size(vm, a.vec); len < size && i < l; i++) {
			if (len < size) len += snprintf(tmp+len, size-len, "%s",
				tmptext(vm, vec_get(vm, a.vec, i), subtmpA, sizeof(subtmpA)));
			if (len < size && i < l-1) len += snprintf(tmp+len, size-len, ", ");
		}
		if (len < size) len += snprintf(tmp+len, size-len, "]");
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

	// peephole
	if (vm->code.depth > 0) {
		code_t* last = compiled(vm, -1);

		// remove implicit return block dead code
		if (op == OP_CLEAN && last->op == OP_CLEAN) {
			return vm->code.depth-1;
		}
		if (op == OP_CLEAN && last->op == OP_RETURN) {
			return vm->code.depth-1;
		}
		if (op == OP_RETURN && last->op == OP_RETURN) {
			return vm->code.depth-1;
		}

		// lit,find -> fname (duplicate vars, single lookup array[#array])
		if (op == OP_FIND && last->op == OP_LIT) {
			for(;;) {
				// fname,copies=n (n+1 stack items: one original + n copies)
				if (vm->code.depth > 2) {
					code_t* prior1 = compiled(vm, -2);
					code_t* prior2 = compiled(vm, -3);
					if (prior1->op == OPP_COPIES && prior2->op == OPP_FNAME && equal(vm, last->item, prior2->item)) {
						prior1->item.inum++;
						vm->code.depth--;
						break;
					}
				}
				// fname,copies=1 (two stack items: one original + one copy)
				if (vm->code.depth > 1) {
					code_t* prior = compiled(vm, -2);
					if (prior->op == OPP_FNAME && equal(vm, last->item, prior->item)) {
						last->op = OPP_COPIES;
						last->item = integer(vm, 1);
						break;
					}
				}
				last->op = OPP_FNAME;
				break;
			}
			return vm->code.depth-1;
		}

		// lit,get -> gname
		if (op == OP_GET && last->op == OP_LIT) {
			last->op = OPP_GNAME;
			return vm->code.depth-1;
		}

		// fname,call -> cfunc
		if (op == OP_CALL && last->op == OPP_FNAME) {
			last->op = OPP_CFUNC;
			return vm->code.depth-1;
		}

		// lit,assign0 -> assignl
		if (op == OP_ASSIGN && item.type == INTEGER && item.inum == 0 && last->op == OP_LIT) {
			last->op = OPP_ASSIGNL;
			return vm->code.depth-1;
		}

		// mark,lit,assignl,limit0 -> lit,assignp (map { litkey = litval }, var = lit)
		if (op == OP_LIMIT && item.type == INTEGER && item.inum == 0 && vm->code.depth > 3) {
			bool mark = vm->code.cells[vm->code.depth-3].op == OP_MARK;
			bool lit1 = vm->code.cells[vm->code.depth-2].op == OP_LIT;
			bool assignl = last->op == OPP_ASSIGNL;
			if (mark && lit1 && assignl) {
				item_t key = last->item;
				vm->code.cells[vm->code.depth-3] = vm->code.cells[vm->code.depth-2];
				vm->code.cells[vm->code.depth-2] = (code_t){.op = OPP_ASSIGNP, .item = key};
				vm->code.depth--;
				return vm->code.depth-1;
			}
		}

		// lit,neg
		if (op == OP_NEG && last->op == OP_LIT && last->item.type == INTEGER) {
			last->item.inum = -last->item.inum;
			return vm->code.depth-1;
		}

		// lit,neg
		if (op == OP_NEG && last->op == OP_LIT && last->item.type == FLOAT) {
			last->item.fnum = -last->item.fnum;
			return vm->code.depth-1;
		}

		// lit,add
		if (op == OP_ADD && last->op == OP_LIT) {
			last->op = OPP_ADD_LIT;
			return vm->code.depth-1;
		}

		// lit,mul
		if (op == OP_MUL && last->op == OP_LIT) {
			last->op = OPP_MUL_LIT;
			return vm->code.depth-1;
		}
	}

	vm->code.cells[vm->code.depth++] = (code_t){.op = op, .item = item};
	return vm->code.depth-1;
}

static vec_t* stack(rela_vm* vm) {
	return &vm->routine->stack;
}

static item_t* stack_cell(rela_vm* vm, int index) {
	vec_t* stk = stack(vm);
	if (index < 0) index = stk->count + index;
	assert(index >= 0 && index < stk->count);
	return &stk->items[index];
}

static int depth(rela_vm* vm) {
	int base = vm->routine->marks.depth ? vm->routine->marks.cells[vm->routine->marks.depth-1]: 0;
	return vec_size(vm, stack(vm)) - base;
}

static void push(rela_vm* vm, item_t item) {
	vec_push(vm, stack(vm), item);
}

static item_t pop(rela_vm* vm) {
	return vec_pop(vm, stack(vm));
}

static item_t top(rela_vm* vm) {
	return *stack_cell(vm, -1);
}

static item_t* item(rela_vm* vm, int i) {
	int base = vm->routine->marks.cells[vm->routine->marks.depth-1];
	return vec_cell(vm, stack(vm), i >= 0 ? base+i: i);
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

static int peek(const char *source, char *name) {
	int len = strlen(name);
	return !strncmp(source, name, len) && !isname(source[len]);
}

static node_t* node_allot(rela_vm* vm) {
	vm->nodes.cells = realloc(vm->nodes.cells, (vm->nodes.depth+1)*sizeof(node_t*));
	vm->nodes.cells[vm->nodes.depth] = calloc(sizeof(node_t),1);
	return vm->nodes.cells[vm->nodes.depth++];
}

static int parse_block(rela_vm* vm, const char *source, node_t *node) {
	int length = 0;
	int found_end = 0;
	int offset = skip_gap(source);

	// don't care about this, but Lua habits...
	if (peek(&source[offset], "do")) {
		offset += 2;
	}

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
	if (peek(&source[offset], "then")) {
		offset += 4;
	}

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

				// key[,val] local variable names
				offset += skip_gap(&source[offset]);
				ensure(vm, isnamefirst(source[offset]), "expected variable: %s", &source[offset]);

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

				offset += skip_gap(&source[offset]);
				if (peek(&source[offset], "in")) offset += 2;

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

				if (!peek(&source[offset], "end"))
				{
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
			if ((length = skip_gap(&source[offset])) > 0)
			{
				offset += length;
				continue;
			}
			if (source[offset] == ',')
			{
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
			if ((length = skip_gap(&source[offset])) > 0)
			{
				offset += length;
				continue;
			}
			if (source[offset] == ',')
			{
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
		operator_t *operations[32];
		node_t *arguments[32];

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
//					compile(vm, OP_MARK, nil(vm));
						if (node->args)
							process(vm, node->args, 0, 0, -1);
						compile(vm, OP_LIT, node->item);
						compile(vm, OP_FIND, nil(vm));
						compile(vm, OP_CALL, nil(vm));
//					compile(vm, OP_LIMIT, integer(vm, -1));
				compile(vm, OP_LIMIT, integer(vm, 1));
				compile(vm, OP_GET, nil(vm));
			}

			// .fn()
			if (node->field) {
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

		for (int i = 0; i < vec_size(vm, node->keys); i++)
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

//		compile(vm, OP_MARK, nil(vm));
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
//		compile(vm, OP_LIMIT, integer(vm, 0));

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
		compile(vm, OP_MARK, nil(vm));
		assert(!node->args);

		for (int i = 0; i < vec_size(vm, node->vals); i++) {
			process(vm, vec_get(vm, node->vals, i).node, 0, 0, -1);
		}

		compile(vm, OP_VECTOR, nil(vm));
		compile(vm, OP_LIMIT, integer(vm, 1));
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

	while (source[offset])
		offset += parse(vm, &source[offset], RESULTS_DISCARD, PARSE_COMMA|PARSE_ANDOR);

	for (int i = 0, l = depth(vm); i < l; i++)
		process(vm, item(vm, i)->node, 0, 0, -1);

	while (depth(vm)) pop(vm);
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

	item_t* item = vec_cell(vm, stack(vm), -items);

	char tmp[STRBUF];
	for (int i = 0; i < items; i++) {
		const char *str = tmptext(vm, *item++, tmp, sizeof(tmp));
		fprintf(stdout, "%s%s", i ? "\t": "", str);
	}
	fprintf(stdout, "\n");
	fflush(stdout);
}

static void op_clean(rela_vm* vm) {
	stack(vm)->count -= depth(vm);
}

static void op_map(rela_vm* vm) {
	vec_push(vm, &vm->routine->other, vm->routine->map);
	vm->routine->map = (item_t){.type = MAP, .map = map_allot(vm)};
}

static void op_unmap(rela_vm* vm) {
	push(vm, vm->routine->map);
	vm->routine->map = vec_pop(vm, &vm->routine->other);
}

static void op_mark(rela_vm* vm) {
	assert(vm->routine->marks.depth < sizeof(vm->routine->marks.cells)/sizeof(int));
	vm->routine->marks.cells[vm->routine->marks.depth++] = vec_size(vm, stack(vm));
}

static void op_unmark(rela_vm* vm) {
	assert(vm->routine->marks.depth > 0);
	vm->routine->marks.depth--;
}

static void limit(rela_vm* vm, int count) {
	assert(vm->routine->marks.depth > 0);
	int old_depth = vm->routine->marks.cells[--vm->routine->marks.depth];
	if (count >= 0) {
		int req_depth = old_depth + count;
		if (req_depth < vec_size(vm, stack(vm))) vec_shrink(vm, stack(vm), req_depth);
		else while (req_depth > vec_size(vm, stack(vm))) push(vm, nil(vm));
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

	int items = depth(vm);

	for (int i = 1; i < items; i++)
		vec_push(vm, &cor->stack, *item(vm, i));

	op_clean(vm);
	cor->state = COR_SUSPENDED;

	vec_pop(vm, &vm->routines);
	vm->routine = vec_top(vm, &vm->routines).cor;

	op_clean(vm);

	push(vm, (item_t){.type = COROUTINE, .cor = cor});
}

static void op_resume(rela_vm* vm) {
	ensure(vm, depth(vm) && item(vm, 0)->type == COROUTINE, "resume missing coroutine");

	cor_t *cor = item(vm, 0)->cor;

	if (cor->state == COR_DEAD) {
		push(vm, nil(vm));
		return;
	}

	cor->state = COR_RUNNING;

	int items = depth(vm);

	for (int i = 1; i < items; i++)
		vec_push(vm, &cor->stack, *item(vm, i));

	for (int i = 0; i < items; i++)
		pop(vm);

	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor});
	vm->routine = cor;
}

static void op_yield(rela_vm* vm) {
	int items = depth(vm);

	cor_t *src = vm->routine;
	vec_pop(vm, &vm->routines);
	vm->routine = vec_top(vm, &vm->routines).cor;
	cor_t *dst = vm->routine;

	for (int i = 0; i < items; i++)
		push(vm, vec_get(vm, &src->stack, vec_size(vm, &src->stack) - items + i));

	for (int i = 0; i < items; i++)
		vec_pop(vm, &src->stack);

	src->state = COR_SUSPENDED;
	dst->marks.depth += items;
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
	assert(vm->routine->loops.depth > 1);
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

static void op_shunt(rela_vm* vm) {
	vec_push(vm, &vm->routine->other, pop(vm));
}

static void op_shift(rela_vm* vm) {
	push(vm, vec_pop(vm, &vm->routine->other));
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
	int items = depth(vm);

	vec_t* vec = vec_allot(vm);

	for (int i = 0; i < items; i++)
		vec_push(vm, vec, vec_get(vm, stack(vm), vec_size(vm, stack(vm)) - items + i));

	for (int i = 0; i < items; i++)
		vec_pop(vm, stack(vm));

	push(vm, (item_t){.type = VECTOR, .vec = vec});
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
		if (frame->locals.keys[i] == key) {
			return &frame->locals.vals[i];
		}
	}
	return NULL;
}

// locate a local variable cell in an outer frame
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
	map_t* map = vm->routine->map.type == MAP ? vm->routine->map.map: NULL;

	if (!map && cor->frames.depth) {
		assert(key.type == STRING);
		item_t* cell = local(vm, key.str);
		if (cell) {
			*cell = val;
			return;
		}
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
	assert(vec_size(vm, vars) >= 1);

	item_t iter = top(vm);
	int step = vm->routine->loops.cells[vm->routine->loops.depth-1];

	if (iter.type == INTEGER) {
		if (step == iter.inum) {
			vm->routine->ip = vm->routine->loops.cells[vm->routine->loops.depth-2];
		}
		else {
			if (vec_size(vm, vars) > 1)
				assign(vm, vec_get(vm, vars, var++), integer(vm, step));

			assign(vm, vec_get(vm, vars, var++), integer(vm,step));
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

			assign(vm, vec_get(vm, vars, var++), vec_get(vm, &iter.map->vals, step));
		}
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
	if (src.type == MAP) {
		item_t val = nil(vm);
		map_get(vm, src.map, key, &val);
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
		vec_cell(vm, stack(vm), -1)->inum *= -1;
	}
	else
	if (top(vm).type == FLOAT) {
		vec_cell(vm, stack(vm), -1)->fnum *= -1;
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
	push(vm, (item_t){.type = BOOLEAN, .flag = truth(vm, pop(vm)) == 0});
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
	if (a.type == INTEGER) a.inum = abs(a.inum);
	else if (a.type == FLOAT) a.fnum = abs(a.fnum);
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
	if (val) {
		push(vm, *val);
		return;
	}

	char tmp[STRTMP];
	ensure(vm, false, "unknown name: %s", tmptext(vm, key, tmp, sizeof(tmp)));
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
	if (val) {
		*cache = *val;
		call(vm, *cache);
		return;
	}
	char tmp[STRTMP];
	ensure(vm, false, "unknown name: %s", tmptext(vm, key, tmp, sizeof(tmp)));
}

// compression of mark,lit,lit,assign,limit0
static void op_assignp(rela_vm* vm) {
	item_t key = literal(vm);
	item_t val = pop(vm);
	assign(vm, key, val);
}

// lit,assign0
static void op_assignl(rela_vm* vm) {
	item_t key = literal(vm);
	// indexed from the base of the current subframe
	item_t val = depth(vm) ? *item(vm, 0): nil(vm);
	assign(vm, key, val);
}

// dups
static void op_copies(rela_vm* vm) {
	for (int i = 0, l = literal_int(vm); i < l; i++) push(vm, top(vm));
}

typedef struct {
	const char *name;
	bool lib;
	rela_callback func;
} func_t;

static void nop(rela_vm* vm) {
}

func_t funcs[OPERATIONS] = {
	[OP_STOP]      = { .name = "stop",      .lib = false, .func = op_stop      },
	[OP_PRINT]     = { .name = "print",     .lib = true,  .func = op_print     },
	[OP_COROUTINE] = { .name = "coroutine", .lib = true,  .func = op_coroutine },
	[OP_RESUME]    = { .name = "resume",    .lib = true,  .func = op_resume    },
	[OP_YIELD]     = { .name = "yield",     .lib = true,  .func = op_yield     },
	[OP_CALL]      = { .name = "call",      .lib = false, .func = op_call      },
	[OP_RETURN]    = { .name = "return",    .lib = false, .func = op_return    },
	[OP_GLOBAL]    = { .name = "global",    .lib = false, .func = op_global    },
	[OP_VECTOR]    = { .name = "vector",    .lib = true,  .func = op_vector    },
	[OP_MAP]       = { .name = "map",       .lib = false, .func = op_map       },
	[OP_UNMAP]     = { .name = "unmap",     .lib = false, .func = op_unmap     },
	[OP_MARK]      = { .name = "mark",      .lib = false, .func = op_mark      },
	[OP_LIMIT]     = { .name = "limit",     .lib = false, .func = op_limit     },
	[OP_LOOP]      = { .name = "loop",      .lib = false, .func = op_loop      },
	[OP_UNLOOP]    = { .name = "unloop",    .lib = false, .func = op_unloop    },
	[OP_CLEAN]     = { .name = "clean",     .lib = false, .func = op_clean     },
	[OP_BREAK]     = { .name = "break",     .lib = false, .func = op_break     },
	[OP_CONTINUE]  = { .name = "continue",  .lib = false, .func = op_continue  },
	[OP_JMP]       = { .name = "jmp",       .lib = false, .func = op_jmp       },
	[OP_JFALSE]    = { .name = "jfalse",    .lib = false, .func = op_jfalse    },
	[OP_JTRUE]     = { .name = "jtrue",     .lib = false, .func = op_jtrue     },
	[OP_FOR]       = { .name = "for",       .lib = false, .func = op_for       },
	[OP_NIL]       = { .name = "nil",       .lib = false, .func = op_nil       },
	[OP_SHUNT]     = { .name = "shunt",     .lib = false, .func = op_shunt     },
	[OP_SHIFT]     = { .name = "shift",     .lib = false, .func = op_shift     },
	[OP_TRUE]      = { .name = "true",      .lib = false, .func = op_true      },
	[OP_FALSE]     = { .name = "false",     .lib = false, .func = op_false     },
	[OP_LIT]       = { .name = "lit",       .lib = false, .func = op_lit       },
	[OP_ASSIGN]    = { .name = "assign",    .lib = false, .func = op_assign    },
	[OP_FIND]      = { .name = "find",      .lib = false, .func = op_find      },
	[OP_SET]       = { .name = "set",       .lib = false, .func = op_set       },
	[OP_GET]       = { .name = "get",       .lib = false, .func = op_get       },
	[OP_COUNT]     = { .name = "count",     .lib = false, .func = op_count     },
	[OP_DROP]      = { .name = "drop",      .lib = false, .func = op_drop      },
	[OP_ADD]       = { .name = "add",       .lib = false, .func = op_add       },
	[OP_NEG]       = { .name = "neg",       .lib = false, .func = op_neg       },
	[OP_SUB]       = { .name = "sub",       .lib = false, .func = op_sub       },
	[OP_MUL]       = { .name = "mul",       .lib = false, .func = op_mul       },
	[OP_DIV]       = { .name = "div",       .lib = false, .func = op_div       },
	[OP_MOD]       = { .name = "mod",       .lib = false, .func = op_mod       },
	[OP_NOT]       = { .name = "not",       .lib = false, .func = op_not       },
	[OP_EQ]        = { .name = "eq",        .lib = false, .func = op_eq        },
	[OP_NE]        = { .name = "ne",        .lib = false, .func = op_ne        },
	[OP_LT]        = { .name = "lt",        .lib = false, .func = op_lt        },
	[OP_LTE]       = { .name = "lte",       .lib = false, .func = op_lte       },
	[OP_GT]        = { .name = "gt",        .lib = false, .func = op_gt        },
	[OP_GTE]       = { .name = "gte",       .lib = false, .func = op_gte       },
	[OP_AND]       = { .name = "and",       .lib = false, .func = nop          },
	[OP_OR]        = { .name = "or",        .lib = false, .func = nop          },
	[OP_CONCAT]    = { .name = "concat",    .lib = false, .func = op_concat    },
	[OP_UNPACK]    = { .name = "unpack",    .lib = false, .func = op_unpack    },
	[OP_MATCH]     = { .name = "match",     .lib = false, .func = op_match     },
	[OP_SORT]      = { .name = "sort",      .lib = true,  .func = op_sort      },
	[OP_PID]       = { .name = "pid",       .lib = false, .func = op_pid       },
	[OP_ASSERT]    = { .name = "assert",    .lib = true,  .func = op_assert    },
	[OP_TYPE]      = { .name = "type",      .lib = true,  .func = op_type      },
	[OP_GC]        = { .name = "collect",   .lib = true,  .func = gc           },
	// math
	[OP_SIN]       = { .name = "sin",       .lib = true, .func = op_sin       },
	[OP_COS]       = { .name = "cos",       .lib = true, .func = op_cos       },
	[OP_TAN]       = { .name = "tan",       .lib = true, .func = op_tan       },
	[OP_ASIN]      = { .name = "asin",      .lib = true, .func = op_asin      },
	[OP_ACOS]      = { .name = "acos",      .lib = true, .func = op_acos      },
	[OP_ATAN]      = { .name = "atan",      .lib = true, .func = op_atan      },
	[OP_COSH]      = { .name = "cosh",      .lib = true, .func = op_cosh      },
	[OP_SINH]      = { .name = "sinh",      .lib = true, .func = op_sinh      },
	[OP_TANH]      = { .name = "tanh",      .lib = true, .func = op_tanh      },
	[OP_CEIL]      = { .name = "ceil",      .lib = true, .func = op_ceil      },
	[OP_FLOOR]     = { .name = "floor",     .lib = true, .func = op_floor     },
	[OP_SQRT]      = { .name = "sqrt",      .lib = true, .func = op_sqrt      },
	[OP_ABS]       = { .name = "abs",       .lib = true, .func = op_abs       },
	[OP_ATAN2]     = { .name = "atan2",     .lib = true, .func = op_atan2     },
	[OP_LOG]       = { .name = "log",       .lib = true, .func = op_log       },
	[OP_LOG10]     = { .name = "log10",     .lib = true, .func = op_log10     },
	[OP_POW]       = { .name = "pow",       .lib = true, .func = op_pow       },
	[OP_MIN]       = { .name = "min",       .lib = true, .func = op_min       },
	[OP_MAX]       = { .name = "max",       .lib = true, .func = op_max       },
	// peephole
	[OPP_FNAME]    = { .name = "fname",     .lib = false, .func = op_fname     },
	[OPP_GNAME]    = { .name = "gname",     .lib = false, .func = op_gname     },
	[OPP_CFUNC]    = { .name = "cfunc",     .lib = false, .func = op_cfunc     },
	[OPP_ASSIGNP]  = { .name = "assignp",   .lib = false, .func = op_assignp   },
	[OPP_ASSIGNL]  = { .name = "assignl",   .lib = false, .func = op_assignl   },
	[OPP_COPIES]   = { .name = "copies",    .lib = false, .func = op_copies    },
	[OPP_MUL_LIT]  = { .name = "litmul",    .lib = false, .func = op_mul_lit   },
	[OPP_ADD_LIT]  = { .name = "litadd",    .lib = false, .func = op_add_lit   },
};

static void decompile(rela_vm* vm, code_t* c) {
	char tmp[STRTMP];
	const char *str = tmptext(vm, c->item, tmp, sizeof(tmp));
	fprintf(stderr, "%04ld  %3d  %-10s  %s\n", c - vm->code.cells, c->cache, funcs[c->op].name, str);
	fflush(stderr);
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

	pool_clear(&vm->maps);
	pool_clear(&vm->vecs);
	pool_clear(&vm->cors);

	free(vm);
}

// Public API

rela_vm* rela_create(const char* src, size_t registrations, const rela_register* registry, void* custom) {
	rela_module main = (rela_module){.name = "main", .source = src};
	return rela_create_ex(1, &main, registrations, registry, custom);
}

rela_vm* rela_create_ex(size_t modules, const rela_module* modistry, size_t registrations, const rela_register* registry, void* custom) {
	rela_vm* vm = calloc(sizeof(rela_vm),1);
	if (!vm) exit(1);

	vm->vecs.page = 1024;
	vm->vecs.object = sizeof(vec_t);
	vm->maps.page = 512;
	vm->maps.object = sizeof(map_t);
	vm->cors.page = 128;
	vm->cors.object = sizeof(cor_t);

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
		assert(!vec_size(vm, stack(vm)));
		compile(vm, OP_STOP, nil(0));
	}

	op_unmark(vm);
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

	int wtf = setjmp(vm->jmp);
	if (wtf) {
		char tmp[STRTMP];
		fprintf(stderr, "%s (", vm->err);
		fprintf(stderr, "ip %d ", vec_size(vm, &vm->routines) ? vm->routine->ip: -1);
		fprintf(stderr, "stack %s", vec_size(vm, &vm->routines) ? tmptext(vm, (item_t){.type = VECTOR, .vec = stack(vm)}, tmp, sizeof(tmp)): "(no routine)");
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

		for (bool run = true; run; ) {
			int ip = vm->routine->ip++;
			assert(ip >= 0 && ip < vm->code.depth);
			int opcode = vm->code.cells[ip].op;

			#ifdef TRACE
				code_t* c = &vm->code.cells[ip];
				char tmpA[STRTMP];
				const char *str = tmptext(vm, c->item, tmpA, sizeof(tmpA));
				for (int i = 0, l = vm->routine->marks.depth; i < l; i++)
					fprintf(stderr, "  ");
				fprintf(stderr, "%04ld  %-10s  %-10s", c - vm->code.cells, funcs[c->op].name, str);
				fflush(stderr);
			#endif

			switch (opcode) {
				// <order-important>
				case OP_STOP: { run = false; break; }
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
				fprintf(stderr, "[");
				for (int i = 0, l = vec_size(vm, stack(vm)); i < l; i++) {
					if (i == l-depth(vm))
						fprintf(stderr, "|");
					char tmpB[STRTMP];
					fprintf(stderr, "%s%s",
						tmptext(vm, vec_get(vm, stack(vm), i), tmpB, sizeof(tmpB)),
						(i < l-1 ? ", ":"")
					);
				}
				fprintf(stderr, "]\n");
				fflush(stderr);
			#endif
		}
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

rela_item rela_make_data(rela_vm* vm, void* data) {
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
	return item.data;
}

