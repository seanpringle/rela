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
#include <sys/stat.h>
#include <assert.h>
#include <setjmp.h>
#include <signal.h>

#ifdef PCRE
#include <pcre.h>
#endif

enum opcode_t {
	OP_NOP=1, OP_PRINT, OP_COROUTINE, OP_RESUME, OP_YIELD, OP_CALL, OP_RETURN, OP_GLOBAL, OP_MAP, OP_VECTOR,
	OP_UNMAP, OP_MARK, OP_LIMIT, OP_LOOP, OP_UNLOOP, OP_CLEAN, OP_BREAK, OP_CONTINUE, OP_JMP, OP_JFALSE,
	OP_JTRUE, OP_AND, OP_OR, OP_FOR, OP_NIL, OP_SHUNT, OP_SHIFT, OP_TRUE, OP_FALSE, OP_LIT, OP_ASSIGN,
	OP_FIND, OP_SET, OP_GET, OP_COUNT, OP_DROP, OP_ADD, OP_NEG, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NOT,
	OP_EQ, OP_NE, OP_LT, OP_GT, OP_LTE, OP_GTE, OP_CONCAT, OP_MATCH, OP_SLURP, OP_SORT, OP_ASSERT, OP_PID,
	OPP_MARKS, OPP_FNAME, OPP_GNAME, OPP_CNAME, OPP_A2LIT, OPP_ASSIGNL, OPP_UNMAP, OP_GC,
	OPERATIONS
};

enum type_t {
	NIL = 0, INTEGER, FLOAT, STRING, BOOLEAN, VECTOR, MAP, SUBROUTINE, COROUTINE, CALLBACK, USERDATA, NODE,
	TYPES
};

const char* type_names[TYPES] = {
	[NIL] = "nil",
	[INTEGER] = "integer",
	[FLOAT] = "float",
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
	NODE_BUILTIN, NODE_VEC, NODE_MAP, NODE_FOR
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
} vec_t; // vector

typedef struct _map_t {
	vec_t keys;
	vec_t vals;
} map_t;

typedef struct _cor_t {
	vec_t stack;  // arguments, results, opcode working
	vec_t other;  // temporary data moved off stack
	vec_t maps;   // map literals during construction
	vec_t locals; // local variable key/val pairs
	vec_t calls;  // call/return stack frames
	vec_t loops;  // loop state data
	vec_t marks;  // mark/limits of stack subframes
	vec_t paths;  // compile-time scope path
	int ip;
	int state;
} cor_t; // coroutine

typedef struct{
	enum opcode_t op;
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
	vec_t* keys;
	vec_t* vals;
	int results;
	struct {
		int id;
		int ids[8];
		int depth;
	} fpath;
} node_t; // AST

typedef struct {
	char *name;
	int opcode;
} keyword_t;

keyword_t keywords[] = {
	{ .name = "global",  .opcode = OP_GLOBAL },
	{ .name = "true",    .opcode = OP_TRUE   },
	{ .name = "false",   .opcode = OP_FALSE  },
	{ .name = "nil",     .opcode = OP_NIL    },
	{ .name = "assert",  .opcode = OP_ASSERT },
	{ .name = "collect", .opcode = OP_GC     },
	{ .name = "sort",    .opcode = OP_SORT   },
	{ .name = "slurp",   .opcode = OP_SLURP  },
};

typedef struct {
	char *name;
	int precedence;
	int opcode;
	int argc;
} operator_t;

// order is important; longer names matched first
operator_t operators[] = {
	{ .name = "and", .precedence = 0, .opcode = OP_AND,   .argc = 2 },
	{ .name = "or",  .precedence = 0, .opcode = OP_OR,    .argc = 2 },
	{ .name = "==",  .precedence = 1, .opcode = OP_EQ,    .argc = 2 },
	{ .name = "!=",  .precedence = 1, .opcode = OP_NE,    .argc = 2 },
	{ .name = ">=",  .precedence = 1, .opcode = OP_GTE,   .argc = 2 },
	{ .name = ">",   .precedence = 1, .opcode = OP_GT,    .argc = 2 },
	{ .name = "<=",  .precedence = 1, .opcode = OP_LTE,   .argc = 2 },
	{ .name = "<",   .precedence = 1, .opcode = OP_LT,    .argc = 2 },
	{ .name = "~",   .precedence = 1, .opcode = OP_MATCH, .argc = 2 },
	{ .name = "+",   .precedence = 3, .opcode = OP_ADD,   .argc = 2 },
	{ .name = "-",   .precedence = 3, .opcode = OP_SUB,   .argc = 2 },
	{ .name = "*",   .precedence = 4, .opcode = OP_MUL,   .argc = 2 },
	{ .name = "/",   .precedence = 4, .opcode = OP_DIV,   .argc = 2 },
	{ .name = "%",   .precedence = 4, .opcode = OP_MOD,   .argc = 2 },
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
	// routines[0] == main coroutine, always set
	// routines[1...n] resume/yield chain
	vec_t routines;

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

	// interned strings
	string_region_t stringsA;
	string_region_t stringsB;

	// compile-time scope tree
	struct {
		int id;
		int ids[8];
		int depth;
	} fpath;

	jmp_buf jmp;
	char err[STRTMP];
	void* custom;
} rela_vm;

typedef void (*opcb)(rela_vm*);
typedef int (*strcb)(int);

typedef struct {
	const char *name;
	opcb func;
} func_t;

static item_t nil(rela_vm* vm);
static bool equal(rela_vm* vm, item_t a, item_t b);
static bool less(rela_vm* vm, item_t a, item_t b);
static const char* tmptext(rela_vm* vm, item_t item, char* tmp, int size);
static cor_t* routine(rela_vm* vm);
static int parse(rela_vm* vm, const char *source, int results, int mode);
static int parse_block(rela_vm* vm, const char *source, node_t *node);
static int parse_branch(rela_vm* vm, const char *source, node_t *node);
static int parse_arglist(rela_vm* vm, const char *source);
static int parse_node(rela_vm* vm, const char *source);

#define ensure(vm,c,...) if (!(c)) { snprintf(vm->err, sizeof(vm->err), __VA_ARGS__); raise(SIGUSR1); /*longjmp(vm->jmp, 1);*/ }

#define RESULTS_DISCARD 0
#define RESULTS_FIRST 1
#define RESULTS_ALL -1

#define PARSE_GREEDY 1
#define PARSE_UNGREEDY 2

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

	if (index < vm->stringsA.depth && vm->stringsA.cells[index] == str) {
		vm->stringsA.mark[index] = true;
	}
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
	gc_mark_vec(vm, &cor->maps);
	gc_mark_vec(vm, &cor->locals);
	gc_mark_vec(vm, &cor->calls);
	gc_mark_vec(vm, &cor->loops);
	gc_mark_vec(vm, &cor->marks);
	gc_mark_vec(vm, &cor->paths);
}

// A naive mark-and-sweep collector: simple, robust and never
// stops the world at inconvenient times because Rela is a
// slacker and doesn't bother to implicitly trigger a GC at
// runtime :-)
// Can be explicitly triggered with "collect()" via script
// or with rela_collect() via callback.
static void gc(rela_vm* vm) {
	memset(vm->maps.mark, 0, sizeof(bool)*vm->maps.depth);
	memset(vm->vecs.mark, 0, sizeof(bool)*vm->vecs.depth);
	memset(vm->cors.mark, 0, sizeof(bool)*vm->cors.depth);
	vm->stringsA.mark = calloc(sizeof(bool),vm->stringsA.depth);

	gc_mark_map(vm, vm->scope_core);
	gc_mark_map(vm, vm->scope_global);

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
			free(cor->maps.items);
			free(cor->locals.items);
			free(cor->calls.items);
			free(cor->loops.items);
			free(cor->marks.items);
			free(cor->paths.items);
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
	ensure(vm, index >= 0 && index <= vec->count, "vec_ins out of bounds");

	if (!vec->items) {
		assert(vec->count == 0);
		vec->items = malloc(sizeof(item_t) * 8);
	}

	vec->count++;

	int size = vec->count;
	bool power_of_2 = size > 0 && (size & (size - 1)) == 0;

	if (power_of_2 && size >= 8)
		vec->items = realloc(vec->items, sizeof(item_t) * (size * 2));

	if (index < vec->count-1)
		memmove(&vec->items[index+1], &vec->items[index], (vec->count - index - 1) * sizeof(item_t));

	vec->items[index].type = NIL;

	return &vec->items[index];
}

static void vec_del(rela_vm* vm, vec_t* vec, int index) {
	ensure(vm, index >= 0 && index < vec->count, "vec_del out of bounds");
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
	gc(vm);
}

static item_t nil(rela_vm* vm) {
	return (item_t){.type = NIL, .str = NULL};
}

static item_t integer(rela_vm* vm, int64_t i) {
	return (item_t){.type = INTEGER, .inum = i};
}

static item_t string(rela_vm* vm, const char* s) {
	return (item_t){.type = STRING, .str = strintern(vm, s) };
}

static bool equal(rela_vm* vm, item_t a, item_t b) {
	if (a.type == b.type && a.type == INTEGER) return a.inum == b.inum;
	if (a.type == b.type && a.type == FLOAT) return fabs(a.fnum - b.fnum) < DBL_EPSILON*10;
	if (a.type == b.type && a.type == STRING) return a.str == b.str; // .str must use strintern
	if (a.type == b.type && a.type == BOOLEAN) return a.flag == b.flag;
	if (a.type == b.type && a.type == VECTOR) return a.vec == b.vec;
	if (a.type == b.type && a.type == MAP) return a.map == b.map;
	if (a.type == b.type && a.type == SUBROUTINE) return a.sub == b.sub;
	if (a.type == b.type && a.type == COROUTINE) return a.cor == b.cor;
	if (a.type == b.type && a.type == USERDATA) return a.data == b.data;
	if (a.type == b.type && a.type == NODE) return a.node == b.node;
	if (a.type == b.type && a.type == NIL) return true;
	return false;
}

static bool less(rela_vm* vm, item_t a, item_t b) {
	if (a.type == b.type && a.type == INTEGER) return a.inum < b.inum;
	if (a.type == b.type && a.type == FLOAT) return a.fnum < b.fnum;
	if (a.type == b.type && a.type == STRING) return a.str != b.str && strcmp(a.str, b.str) < 0;
	if (a.type == b.type && a.type == VECTOR) return vec_size(vm, a.vec) < vec_size(vm, b.vec);
	if (a.type == b.type && a.type == MAP) return vec_size(vm, &a.map->keys) < vec_size(vm, &b.map->keys);
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

		// marks,mark -> marks+1
		if (op == OP_MARK && last->op == OPP_MARKS) {
			last->item.inum++;
			return vm->code.depth-1;
		}

		// mark,mark -> marks2
		if (op == OP_MARK && last->op == OP_MARK) {
			last->op = OPP_MARKS;
			last->item = integer(vm, 2);
			return vm->code.depth-1;
		}

		// lit,find -> fname
		if (op == OP_FIND && last->op == OP_LIT) {
			last->op = OPP_FNAME;
			return vm->code.depth-1;
		}

		// lit,get -> gname
		if (op == OP_GET && last->op == OP_LIT) {
			last->op = OPP_GNAME;
			return vm->code.depth-1;
		}

		// fname,call -> cname
		if (op == OP_CALL && last->op == OPP_FNAME) {
			last->op = OPP_CNAME;
			return vm->code.depth-1;
		}

		// unmap,limit0 -> unmapl
		if (op == OP_LIMIT && item.type == INTEGER && item.inum == 1 && last->op == OP_UNMAP) {
			last->op = OPP_UNMAP;
			return vm->code.depth-1;
		}

		// mark,lit,lit,assign,limit0 -> a2lit
		if (op == OP_LIMIT && item.type == INTEGER && item.inum == 0 && vm->code.depth > 4) {
			bool mark = vm->code.cells[vm->code.depth-4].op == OP_MARK;
			bool lit1 = vm->code.cells[vm->code.depth-3].op == OP_LIT;
			bool lit2 = vm->code.cells[vm->code.depth-2].op == OP_LIT;
			bool assign = last->op == OP_ASSIGN && last->item.inum == 0;
			if (mark && lit1 && lit2 && assign) {
				item_t key = vm->code.cells[vm->code.depth-2].item;
				vm->code.cells[vm->code.depth-4] = vm->code.cells[vm->code.depth-3];
				vm->code.cells[vm->code.depth-3] = (code_t){.op = OPP_A2LIT, .item = key};
				vm->code.depth-=2;
				return vm->code.depth-1;
			}
		}

		// assign0,limit0 -> assignl
		if (op == OP_LIMIT && item.type == INTEGER && item.inum == 0) {
			bool assign = last->op == OP_ASSIGN && last->item.type == INTEGER && last->item.inum == 0;
			if (assign) {
				last->op = OPP_ASSIGNL;
				return vm->code.depth-1;
			}
		}

		// mark,lit,limit-1 -> lit
		if (op == OP_LIMIT && item.type == INTEGER && item.inum == 0 && vm->code.depth > 3) {
			bool mark = vm->code.cells[vm->code.depth-2].op == OP_MARK;
			bool marks = vm->code.cells[vm->code.depth-2].op == OPP_MARKS;
			bool lit = vm->code.cells[vm->code.depth-1].op == OP_LIT;
			if (mark && lit) {
				vm->code.cells[vm->code.depth-2] = vm->code.cells[vm->code.depth-1];
				vm->code.depth--;
				return vm->code.depth-1;
			}
			if (marks && lit) {
				vm->code.cells[vm->code.depth-2].item.inum--;
				vm->code.depth--;
				return vm->code.depth-1;
			}
		}
	}

	vm->code.cells[vm->code.depth++] = (code_t){.op = op, .item = item};
	return vm->code.depth-1;
}

static cor_t* routine(rela_vm* vm) {
	return vec_top(vm, &vm->routines).cor;
}

static vec_t* stack(rela_vm* vm) {
	return &routine(vm)->stack;
}

static int depth(rela_vm* vm) {
	int base = vec_size(vm, &routine(vm)->marks) ? vec_cell(vm, &routine(vm)->marks, -1)->inum: 0;
	return vec_size(vm, stack(vm)) - base;
}

static void push(rela_vm* vm, item_t item) {
	vec_push(vm, stack(vm), item);
}

static item_t pop(rela_vm* vm) {
	return vec_pop(vm, stack(vm));
}

static item_t top(rela_vm* vm) {
	return *vec_cell(vm, stack(vm), -1);
}

static item_t* item(rela_vm* vm, int i) {
	int base = vec_cell(vm, &routine(vm)->marks, -1)->inum;
	return vec_cell(vm, stack(vm), i >= 0 ? base+i: i);
}

static item_t pop_type(rela_vm* vm, int type) {
	item_t i = pop(vm);
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

		offset += parse(vm, &source[offset], RESULTS_DISCARD, PARSE_GREEDY);
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

		offset += parse(vm, &source[offset], RESULTS_DISCARD, PARSE_GREEDY);
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

			offset += parse(vm, &source[offset], RESULTS_DISCARD, PARSE_GREEDY);
			vec_push_allot(vm, &node->keys, pop(vm));
		}
	}

	ensure(vm, found_else || found_end, "expected keyword 'else' or 'end': %s", &source[offset]);

	if (vec_size(vm, node->vals))
		vec_cell(vm, node->vals, vec_size(vm, node->vals)-1)->node->results = 1;

	if (vec_size(vm, node->keys))
		vec_cell(vm, node->keys, vec_size(vm, node->keys)-1)->node->results = 1;

	return offset;
}

static int parse_arglist(rela_vm* vm, const char *source) {
	int mark = depth(vm);
	int offset = skip_gap(source);

	if (source[offset] == '(') {
		offset++;
		offset += skip_gap(&source[offset]);

		if (source[offset] != ')') {
			offset += parse(vm, &source[offset], RESULTS_ALL, PARSE_GREEDY);
			offset += skip_gap(&source[offset]);
		}

		ensure(vm, source[offset] == ')', "expected closing paren: %s", &source[offset]);
		offset++;
	}
	else {
		offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_GREEDY);
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
		outer->args = pop(vm).node;
		outer->results = 1;
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

				// conditions
				offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_GREEDY);
				node->args = pop(vm).node;

				// block, optional else
				offset += parse_branch(vm, &source[offset], node);
			}
			else
			if (peek(&source[offset], "while")) {
				offset += 5;
				node->type = NODE_WHILE;

				// conditions
				offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_GREEDY);
				node->args = pop(vm).node;

				// block
				offset += parse_block(vm, &source[offset], node);
			}
			else
			if (peek(&source[offset], "for")) {
				offset += 3;
				node->type = NODE_FOR;

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
				offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_GREEDY);
				node->args = pop(vm).node;

				// block
				offset += parse_block(vm, &source[offset], node);
			}
			else
			if (peek(&source[offset], "function")) {
				offset += 8;
				node->type = NODE_FUNCTION;

				ensure(vm, vm->fpath.depth < sizeof(vm->fpath.ids)/sizeof(vm->fpath.ids[0]), "reached function nest limit(%ld)", sizeof(vm->fpath.ids)/sizeof(vm->fpath.ids[0]));

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

				offset += skip_gap(&source[offset]);

				if (!peek(&source[offset], "end"))
				{
					offset += parse(vm, &source[offset], RESULTS_ALL, PARSE_GREEDY);
					node->args = pop(vm).node;
				}
			}
			else
			if (peek(&source[offset], "break")) {
				offset += 5;
				node->type = NODE_OPCODE;
				node->opcode = OP_BREAK;
			}
			else
			if (peek(&source[offset], "continue")) {
				offset += 8;
				node->type = NODE_OPCODE;
				node->opcode = OP_CONTINUE;
			}
			else
			if (peek(&source[offset], "coroutine")) {
				offset += 9;
				node->type = NODE_BUILTIN;
				node->opcode = OP_COROUTINE;
				offset += parse_arglist(vm, &source[offset]);
				node->args = pop(vm).node;
				node->results = 1;
			}
			else
			if (peek(&source[offset], "yield")) {
				offset += 5;
				node->type = NODE_BUILTIN;
				node->opcode = OP_YIELD;
				offset += parse_arglist(vm, &source[offset]);
				node->args = pop(vm).node;
				node->results = -1;
			}
			else
			if (peek(&source[offset], "resume")) {
				offset += 6;
				node->type = NODE_BUILTIN;
				node->opcode = OP_RESUME;
				offset += parse_arglist(vm, &source[offset]);
				node->args = pop(vm).node;
				node->results = -1;
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
		char *end = NULL;
		node->item = string(vm, strliteral(vm, &source[offset], &end));
		offset += end - &source[offset];
	}
	else
	if (isdigit(source[offset])) {
		node->type = NODE_LITERAL;
		char *a = NULL, *b = NULL;
		int64_t i = strtoll(&source[offset], &a, 0);
		double f = strtod(&source[offset], &b);
		node->item = (b > a) ? (item_t){.type = FLOAT, .fnum = f}: (item_t){.type = INTEGER, .inum = i};
		offset += ((b > a) ? b: a) - &source[offset];
	}
	else
	// a vector[n] or map[s] to be set/get
	if (source[offset] == '[') {
		offset++;
		node->type = NODE_VEC;
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
			offset += parse_node(vm, &source[offset]);
			vec_push_allot(vm, &node->vals, pop(vm));
		}
		ensure(vm, source[offset] == ']', "expected closing bracket: %s", &source[offset]);
		offset++;
	}
	else
	if (source[offset] == '{') {
		offset++;
		node->type = NODE_MAP;

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
				call->type = NODE_OPCODE;
				call->opcode = OP_CALL;
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
		if (source[offset] == '.') {
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
				offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_GREEDY);
				arguments[argument++] = pop(vm).node;
				arguments[argument-1]->results = 1;
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
				if (!strncmp(op->name, &source[offset], strlen(op->name))) {
					compare = op;
					break;
				}
			}

			if (!compare) break;
			offset += strlen(compare->name);

			while (operation > 0 && operations[operation-1]->precedence >= compare->precedence) {
				operator_t *consume = operations[--operation];
				node_t *result = node_allot(vm);
				result->type   = NODE_OPCODE;
				result->opcode = consume->opcode;
				ensure(vm, argument >= consume->argc, "operator %s insufficient arguments", consume->name);
				for (int j = consume->argc; j > 0; --j) {
					vec_push_allot(vm, &result->vals, (item_t){.type = NODE, .node = arguments[argument-j]});
				}
				argument -= consume->argc;
				arguments[argument++] = result;
			}

			operations[operation++] = compare;
		}

		while (operation && argument) {
			operator_t *consume = operations[--operation];
			node_t *result = node_allot(vm);
			result->type   = NODE_OPCODE;
			result->opcode = consume->opcode;
			ensure(vm, argument >= consume->argc, "operator %s insufficient arguments", consume->name);
			for (int j = consume->argc; j > 0; --j) {
				vec_push_allot(vm, &result->vals, (item_t){.type = NODE, .node = arguments[argument-j]});
			}
			argument -= consume->argc;
			arguments[argument++] = result;
		}

		ensure(vm, !operation && argument == 1, "unbalanced expression");
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

		if (source[offset] == ',' && mode == PARSE_GREEDY) {
			offset++;
			continue;
		}

		break;
	}

	ensure(vm, vec_size(vm, node->vals) > 0, "missing assignment value: %s", &source[offset]);

	push(vm, (item_t){.type = NODE, .node = node});
	return offset;
}

static void process(rela_vm* vm, node_t *node, int flags, int index) {
	int flag_assign = flags & PROCESS_ASSIGN ? 1:0;

	// if we're assigning with chained expressions, only OP_SET|OP_ASSIGN the last one
	bool assigning = flag_assign && !node->chain;

	char tmp[STRTMP];

	// a multi-part expression: a[,b...] = node[,node...]
	// this is the entry point for most non-control non-opcode statements
	if (node->type == NODE_MULTI) {
		assert(!node->args);
		assert(vec_size(vm, node->vals));

		// stack frame
		compile(vm, OP_MARK, nil(vm));

		// stream the values onto the stack
		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0);

		// OP_SET|OP_ASSIGN index values from the start of the current stack frame
		for (int i = 0; i < vec_size(vm, node->keys); i++) {
			node_t* subnode = vec_get(vm, node->keys, i).node;
			process(vm, subnode, PROCESS_ASSIGN, i);
		}

		// end stack frame
		compile(vm, OP_LIMIT, integer(vm, node->results));
	}
	else
	if (node->type == NODE_NAME) {
		assert(!vec_size(vm, node->keys) && !vec_size(vm, node->vals));

		// function or function-like opcode call
		if (node->call) {

			// vecmap[fn()]
			if (node->index) {
				compile(vm, OP_MARK, nil(vm));
				if (node->args)
					process(vm, node->args, 0, 0);
				compile(vm, OP_LIT, node->item);
				compile(vm, OP_FIND, nil(vm));
				compile(vm, OP_CALL, nil(vm));
				compile(vm, OP_LIMIT, integer(vm, 1));
				compile(vm, assigning ? OP_SET: OP_GET, nil(vm));
			}

			// .fn()
			if (node->field) {
				compile(vm, OP_LIT, node->item);
				compile(vm, assigning ? OP_SET: OP_GET, nil(vm));
				if (node->args) {
					compile(vm, OP_SHUNT, nil(vm));
					process(vm, node->args, 0, 0);
					compile(vm, OP_SHIFT, nil(vm));
				}
				compile(vm, OP_CALL, nil(vm));
			}

			// fn()
			if (!node->index && !node->field) {
				if (node->args)
					process(vm, node->args, 0, 0);

				compile(vm, OP_LIT, node->item);

				if (assigning) {
					compile(vm, OP_ASSIGN, integer(vm, index));
				}
				else {
					compile(vm, OP_FIND, nil(vm));
				}

				compile(vm, OP_CALL, nil(vm));
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
			process(vm, node->chain, flag_assign ? PROCESS_ASSIGN: 0, 0);
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
			process(vm, vec_get(vm, node->keys, i).node, PROCESS_ASSIGN, i);

		compile(vm, OP_CLEAN, nil(vm));

		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0);

		// if an explicit return expression is used, these instructions
		// will be dead code
		compile(vm, OP_CLEAN, nil(vm));
		compile(vm, OP_RETURN, nil(vm));
		compiled(vm, jump)->item = integer(vm, vm->code.depth);
		// will sub dead code

		compile(vm, OP_LIMIT, integer(vm, 1));
	}
	else
	// inline opcode
	if (node->type == NODE_OPCODE) {
		if (node->opcode == OP_CALL)
			compile(vm, OP_SHUNT, nil(vm));

		if (node->args)
			process(vm, node->args, 0, 0);

		if (node->opcode == OP_CALL)
			compile(vm, OP_SHIFT, nil(vm));

		if (node->opcode == OP_AND || node->opcode == OP_OR) {
			process(vm, vec_get(vm, node->vals, 0).node, 0, 0);
			int jump = compile(vm, node->opcode, nil(vm));
			process(vm, vec_get(vm, node->vals, 1).node, 0, 0);
			compiled(vm, jump)->item = integer(vm, vm->code.depth);
		}
		else {
			for (int i = 0; i < vec_size(vm, node->vals); i++)
				process(vm, vec_get(vm, node->vals, i).node, 0, 0);

			compile(vm, node->opcode, nil(vm));
		}

		if (node->index) {
			compile(vm, assigning ? OP_SET: OP_GET, nil(vm));
		}

		if (node->chain) {
			process(vm, node->chain, flag_assign ? PROCESS_ASSIGN: 0, 0);
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
					ensure(vm, length == parse(vm, sub, RESULTS_FIRST, PARSE_GREEDY), "string interpolation parsing failed");
					process(vm, pop(vm).node, 0, 0);
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
			process(vm, node->chain, flag_assign ? PROCESS_ASSIGN: 0, 0);
		}

		ensure(vm, !assigning || node->item.type == STRING, "cannot assign %s",
			tmptext(vm, node->item, tmp, sizeof(tmp)));

		// special case allows: "complex-string" = value in map literals
		if (!node->index && assigning && node->item.type == STRING) {
			compile(vm, OP_ASSIGN, integer(vm, index));
		}
	}
	else
	// a built-in function-like keyword with arguments
	if (node->type == NODE_BUILTIN) {
		assert(!vec_size(vm, node->keys) && !vec_size(vm, node->vals));

		compile(vm, OP_MARK, nil(vm));

		if (node->args)
			process(vm, node->args, 0, 0);

		compile(vm, node->opcode, nil(vm));
		compile(vm, OP_LIMIT, integer(vm, node->results));

		ensure(vm, !assigning, "cannot assign to keyword");
	}
	else
	// if expression ... [else ...] end
	// (returns a value for ternary style assignment)
	if (node->type == NODE_IF) {

		// conditions
		if (node->args)
			process(vm, node->args, 0, 0);

		// if false, jump to else/end
		int jump = compile(vm, OP_JFALSE, nil(vm));
		compile(vm, OP_DROP, nil(vm));

		// success block
		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0);

		// optional failure block
		if (vec_size(vm, node->keys)) {
			// jump success path past failure block
			int jump2 = compile(vm, OP_JMP, nil(vm));
			compiled(vm, jump)->item = integer(vm, vm->code.depth);
			compile(vm, OP_DROP, nil(vm));

			// failure block
			for (int i = 0; i < vec_size(vm, node->keys); i++)
				process(vm, vec_get(vm, node->keys, i).node, 0, 0);

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
			process(vm, node->args, 0, 0);

		// if false, jump to end
		int iter = compile(vm, OP_JFALSE, nil(vm));
		compile(vm, OP_DROP, nil(vm));

		// do ... end
		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0);

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
			process(vm, node->args, 0, 0);

		// loop counter
		compile(vm, OP_LIT, integer(vm, 0));

		compile(vm, OP_MARK, nil(vm));
		int loop = compile(vm, OP_LOOP, nil(vm));

		int begin = vm->code.depth;

		// OP_FOR expects a vector with key[,val] variable names
		if (!node->keys) node->keys = vec_allot(vm);
		compile(vm, OP_FOR, (item_t){.type = VECTOR, .vec = node->keys});

		// block
		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0);

		// clean up
		compile(vm, OP_JMP, integer(vm, begin));
		vec_push_allot(vm, &node->keys, integer(vm, vm->code.depth));
		compiled(vm, loop)->item = integer(vm, vm->code.depth);
		compile(vm, OP_UNLOOP, nil(vm));
		compile(vm, OP_LIMIT, integer(vm, 0));
		compile(vm, OP_LIMIT, integer(vm, 0));

		ensure(vm, !assigning, "cannot assign to for block");
	}
	else
	// return 0 or more values
	if (node->type == NODE_RETURN) {
		compile(vm, OP_CLEAN, nil(vm));

		if (node->args)
			process(vm, node->args, 0, 0);

		compile(vm, OP_RETURN, nil(vm));

		ensure(vm, !assigning, "cannot assign to return");
	}
	else
	// literal vector [1,2,3]
	if (node->type == NODE_VEC) {
		compile(vm, OP_MARK, nil(vm));

		if (node->args)
			process(vm, node->args, 0, 0);

		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0);

		compile(vm, OP_VECTOR, nil(vm));
		compile(vm, OP_LIMIT, integer(vm, 1));
	}
	else
	// literal map { a = 1, b = 2, c = nil }
	if (node->type == NODE_MAP) {
		compile(vm, OP_MARK, nil(vm));
		compile(vm, OP_MAP, nil(vm));

		if (node->args)
			process(vm, node->args, 0, 0);

		for (int i = 0; i < vec_size(vm, node->vals); i++)
			process(vm, vec_get(vm, node->vals, i).node, 0, 0);

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
		offset += parse(vm, &source[offset], RESULTS_DISCARD, PARSE_GREEDY);

	for (int i = 0, l = depth(vm); i < l; i++)
		process(vm, item(vm, i)->node, 0, 0);

	while (depth(vm)) pop(vm);
}

static item_t literal(rela_vm* vm) {
	return vm->code.cells[routine(vm)->ip-1].item;
}

static int64_t literal_int(rela_vm* vm) {
	item_t lit = literal(vm);
	return lit.type == INTEGER ? lit.inum: 0;
}

static void op_nop (rela_vm* vm) {
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

static void op_map(rela_vm* vm) {
	vec_push(vm, &routine(vm)->maps, (item_t){.type = MAP, .map = map_allot(vm)});
}

static void op_unmap(rela_vm* vm) {
	push(vm, (item_t){.type = MAP, .map = vec_pop(vm, &routine(vm)->maps).map});
}

#define FRAME 5
#define LOCALS 0
#define PATHS 1
#define LOOPS 2
#define MARKS 3
#define IP 4

static void arrive(rela_vm* vm, int ip) {
	cor_t* cor = routine(vm);

	vec_push(vm, &cor->calls, integer(vm, vec_size(vm, &cor->locals)));
	vec_push(vm, &cor->calls, integer(vm, vec_size(vm, &cor->paths)));
	vec_push(vm, &cor->calls, integer(vm, vec_size(vm, &cor->loops)));
	vec_push(vm, &cor->calls, integer(vm, vec_size(vm, &cor->marks)));
	vec_push(vm, &cor->calls, integer(vm, cor->ip));

	cor->ip = ip;

	int maps = vec_size(vm, &cor->maps);
	for (int i = 0, l = maps; i < l; i++)
		vec_push(vm, &cor->other, vec_pop(vm, &cor->maps));
	vec_push(vm, &cor->other, integer(vm, maps));
}

static void depart(rela_vm* vm) {
	cor_t* cor = routine(vm);

	cor->ip = vec_pop(vm, &cor->calls).inum;
	for (int i = vec_size(vm, &cor->marks),  l = vec_pop(vm, &cor->calls).inum; i > l; --i) vec_pop(vm, &cor->marks);
	for (int i = vec_size(vm, &cor->loops),  l = vec_pop(vm, &cor->calls).inum; i > l; --i) vec_pop(vm, &cor->loops);
	for (int i = vec_size(vm, &cor->paths),  l = vec_pop(vm, &cor->calls).inum; i > l; --i) vec_pop(vm, &cor->paths);
	for (int i = vec_size(vm, &cor->locals), l = vec_pop(vm, &cor->calls).inum; i > l; --i) vec_pop(vm, &cor->locals);

	int maps = vec_pop(vm, &cor->other).inum;
	for (int i = 0, l = maps; i < l; i++)
		vec_push(vm, &cor->maps, vec_pop(vm, &cor->other));
}

static void op_coroutine(rela_vm* vm) {
	cor_t *cor = cor_allot(vm);

	int ip = pop_type(vm, SUBROUTINE).sub;

	cor->state = COR_RUNNING;
	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor});

	arrive(vm, ip);

	vec_pop(vm, &vm->routines);
	cor->state = COR_SUSPENDED;

	push(vm, (item_t){.type = COROUTINE, .cor = cor});
}

static void op_resume(rela_vm* vm) {
	cor_t *cor = pop_type(vm, COROUTINE).cor;

	if (cor->state == COR_DEAD) {
		push(vm, nil(vm));
		return;
	}

	cor->state = COR_RUNNING;

	int items = depth(vm);

	for (int i = 1; i < items; i++)
		vec_push(vm, &cor->stack, *item(vm, i));

	for (int i = 1; i < items; i++)
		pop(vm);

	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor});
}

static void op_yield(rela_vm* vm) {
	int items = depth(vm);

	cor_t *src = routine(vm);
	vec_pop(vm, &vm->routines);
	cor_t *dst = routine(vm);

	for (int i = 0; i < items; i++)
		push(vm, vec_get(vm, &src->stack, vec_size(vm, &src->stack) - items + i));

	for (int i = 0; i < items; i++)
		vec_pop(vm, &src->stack);

	src->state = COR_SUSPENDED;
	vec_cell(vm, &dst->marks, -1)->inum += items;
}

static void op_global(rela_vm* vm) {
	push(vm, (item_t){.type = MAP, .map = vm->scope_global});
}

static void call(rela_vm* vm, item_t item) {
	if (item.type == CALLBACK) {
		item.cb((rela_vm*)vm);
		return;
	}

	char tmp[STRTMP];
	ensure(vm, item.type == SUBROUTINE, "invalid function: %s (ip: %u)", tmptext(vm, item, tmp, sizeof(tmp)), routine(vm)->ip);

	arrive(vm, item.sub);
}

static void op_call(rela_vm* vm) {
	call(vm, pop(vm));
}

static void op_clean(rela_vm* vm) {
	while (depth(vm)) pop(vm);
}

static void op_return(rela_vm* vm) {
	cor_t* cor = routine(vm);

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
	vec_push(vm, &routine(vm)->loops, integer(vm, vec_size(vm, &routine(vm)->marks)));
	vec_push(vm, &routine(vm)->loops, integer(vm, literal_int(vm)));
}

static void op_unloop(rela_vm* vm) {
	vec_pop(vm, &routine(vm)->loops);
	ensure(vm, vec_pop(vm, &routine(vm)->loops).inum == vec_size(vm, &routine(vm)->marks), "mark stack mismatch (unloop)");
}

static void op_mark(rela_vm* vm) {
	vec_push(vm, &routine(vm)->marks, integer(vm, vec_size(vm, stack(vm))));
}

static void op_unmark(rela_vm* vm) {
	vec_pop(vm, &routine(vm)->marks);
}

static void limit(rela_vm* vm, int count) {
	int old_depth = vec_pop(vm, &routine(vm)->marks).inum;
	int req_depth = old_depth + count;
	if (count >= 0) {
		while (req_depth < vec_size(vm, stack(vm))) pop(vm);
		while (req_depth > vec_size(vm, stack(vm))) push(vm, nil(vm));
	}
}

static void op_limit(rela_vm* vm) {
	limit(vm, literal_int(vm));
}

static void op_break(rela_vm* vm) {
	routine(vm)->ip = vec_cell(vm, &routine(vm)->loops, -1)->inum;
	while (vec_size(vm, &routine(vm)->marks) > vec_cell(vm, &routine(vm)->loops, -FRAME+MARKS)->inum)
		vec_pop(vm, &routine(vm)->marks);
	while (depth(vm)) pop(vm);
}

static void op_continue(rela_vm* vm) {
	routine(vm)->ip = vec_cell(vm, &routine(vm)->loops, -1)->inum-1;
	while (vec_size(vm, &routine(vm)->marks) > vec_cell(vm, &routine(vm)->loops, -FRAME+MARKS)->inum)
		vec_pop(vm, &routine(vm)->marks);
	while (depth(vm)) pop(vm);
}

static void op_shunt(rela_vm* vm) {
	vec_push(vm, &routine(vm)->other, pop(vm));
}

static void op_shift(rela_vm* vm) {
	push(vm, vec_pop(vm, &routine(vm)->other));
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
	routine(vm)->ip = literal_int(vm);
}

static void op_jfalse(rela_vm* vm) {
	if (!truth(vm, top(vm))) op_jmp(vm);
}

static void op_jtrue(rela_vm* vm) {
	if (truth(vm, top(vm))) op_jmp(vm);
}

static void op_and(rela_vm* vm) {
	if (!truth(vm, top(vm))) op_jmp(vm);
	else pop(vm);
}

static void op_or(rela_vm* vm) {
	if (truth(vm, top(vm))) op_jmp(vm);
	else pop(vm);
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

static void op_pid(rela_vm* vm) {
	vec_push(vm, &routine(vm)->paths, literal(vm));
}

// Locate a local variable cell by key somewhere on cor->locals
static item_t* locate(rela_vm* vm, item_t key, int frames) {
	cor_t* cor = routine(vm);

	if (vec_size(vm, &cor->calls)) {
		assert(vec_size(vm, &cor->calls)%FRAME==0);

		int frame_last = vec_size(vm, &cor->calls)-FRAME;
		int frame = frame_last;

		int pids_base = vec_cell(vm, &cor->calls, frame+PATHS)->inum;
		item_t* pids = vec_cell(vm, &cor->paths, pids_base);
		int depth = vec_size(vm, &cor->paths) - pids_base;

		while (frames > 0 && frame >= 0) {
			int paths_base = vec_cell(vm, &cor->calls, frame+PATHS)->inum;
			int pid = vec_cell(vm, &cor->paths, paths_base)->inum;

			bool check = false;
			for (int i = 0; !check && i < depth; i++) {
				if (pid == pids[i].inum) check = true;
			}

			// if this call stack frame belongs to a function outside the current
			// function's compile-time scope chain, skip it
			if (check) {
				int local_base = vec_cell(vm, &cor->calls, frame)->inum;
				int local_last = frame < frame_last ? vec_cell(vm, &cor->calls, frame+FRAME)->inum: vec_size(vm, &cor->locals);
				int local = local_base;

				while (local < local_last) {
					if (equal(vm, vec_get(vm, &cor->locals, local), key)) {
						return vec_cell(vm, &cor->locals, local+1);
					}
					local += 2;
				}
			}

			frame -= FRAME;
			frames--;
		}
	}
	return NULL;
}

static void assign(rela_vm* vm, item_t key, item_t val) {
	cor_t* cor = routine(vm);

	// OP_ASSIGN is used for too many things: local variables, map literal keys, global keys
	map_t* map = vec_size(vm, &routine(vm)->maps) ? vec_top(vm, &routine(vm)->maps).map: NULL;

	if (!map && vec_size(vm, &cor->calls)) {
		item_t* local = locate(vm, key, 1);
		if (local) {
			*local = val;
		}
		else {
			vec_push(vm, &cor->locals, key);
			vec_push(vm, &cor->locals, val);
		}
		return;
	}

	map_set(vm, map ? map: vm->scope_global, key, val);
}

static bool find(rela_vm* vm, item_t key, item_t* val) {
	item_t* local = locate(vm, key, 100);

	if (local) {
		*val = *local;
		return true;
	}

	if (map_get(vm, vm->scope_global, key, val)) return true;
	if (map_get(vm, vm->scope_core, key, val)) return true;

	return false;
}

static void op_assign(rela_vm* vm) {
	item_t key = pop(vm);

	int index = literal_int(vm);
	// indexed from the base of the current marked frame
	item_t val = depth(vm) ? *item(vm, index): nil(vm);

	assign(vm, key, val);
}

static void op_find(rela_vm* vm) {
	item_t key = pop(vm);
	item_t val = nil(vm);

	char tmp[STRTMP];
	ensure(vm, find(vm, key, &val), "unknown name: %s", tmptext(vm, key, tmp, sizeof(tmp)));

	push(vm, val);
}

static void op_for(rela_vm* vm) {
	assert(literal(vm).type == VECTOR);

	int var = 0;
	vec_t* vars = literal(vm).vec;
	assert(vec_size(vm, vars) >= 2);

	int quit = vec_get(vm, vars, vec_size(vm, vars)-1).inum;

	item_t item = pop(vm);
	item_t iter = top(vm);

	if (iter.type == INTEGER) {
		int step = item.inum;

		if (step == iter.inum) {
			routine(vm)->ip = quit;
		}
		else {
			if (vec_size(vm, vars) > 2)
				assign(vm, vec_get(vm, vars, var++), integer(vm, step));

			assign(vm, vec_get(vm, vars, var++), integer(vm,step));
			push(vm, integer(vm, ++step));
		}
	}
	else
	if (iter.type == VECTOR) {
		int step = item.inum;

		if (step >= vec_size(vm, iter.vec)) {
			routine(vm)->ip = quit;
		}
		else {
			if (vec_size(vm, vars) > 2)
				assign(vm, vec_get(vm, vars, var++), integer(vm, step));

			assign(vm, vec_get(vm, vars, var++), vec_get(vm, iter.vec, step));
			push(vm, integer(vm, ++step));
		}
	}
	else
	if (iter.type == MAP) {
		int step = item.inum;

		if (step >= vec_size(vm, &iter.map->keys)) {
			routine(vm)->ip = quit;
		}
		else {
			if (vec_size(vm, vars) > 2)
				assign(vm, vec_get(vm, vars, var++), vec_get(vm, &iter.map->keys, step));

			assign(vm, vec_get(vm, vars, var++), vec_get(vm, &iter.map->vals, step));
			push(vm, integer(vm, ++step));
		}
	}
	else {
		routine(vm)->ip = quit;
	}
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
	item_t key = pop(vm);
	item_t src = pop(vm);
	push(vm, get(vm, src, key));
}

static void op_add(rela_vm* vm) {
	item_t b = pop(vm);
	item_t a = pop(vm);
	push(vm, add(vm, a, b));
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
	item_t b = pop(vm);
	item_t a = pop(vm);
	push(vm, multiply(vm, a, b));
}

static void op_div(rela_vm* vm) {
	item_t b = pop(vm);
	item_t a = pop(vm);
	push(vm, divide(vm, a, b));
}

static void op_mod(rela_vm* vm) {
	item_t b = pop_type(vm, INTEGER);
	item_t a = pop_type(vm, INTEGER);
	push(vm, (item_t){.type = INTEGER, .inum = a.inum % b.inum});
}

static void op_eq(rela_vm* vm) {
	item_t b = pop(vm);
	item_t a = pop(vm);
	push(vm, (item_t){.type = BOOLEAN, .flag = equal(vm, a, b)});
}

static void op_not(rela_vm* vm) {
	push(vm, (item_t){.type = BOOLEAN, .flag = truth(vm, pop(vm)) == 0});
}

static void op_ne(rela_vm* vm) {
	op_eq(vm);
	op_not(vm);
}

static void op_lt(rela_vm* vm) {
	item_t b = pop(vm);
	item_t a = pop(vm);
	push(vm, (item_t){.type = BOOLEAN, .flag = less(vm, a, b)});
}

static void op_gt(rela_vm* vm) {
	item_t b = pop(vm);
	item_t a = pop(vm);
	push(vm, (item_t){.type = BOOLEAN, .flag = !less(vm, a, b) && !equal(vm, a, b)});
}

static void op_lte(rela_vm* vm) {
	item_t b = pop(vm);
	item_t a = pop(vm);
	push(vm, (item_t){.type = BOOLEAN, .flag = less(vm, a, b) || equal(vm, a, b)});
}

static void op_gte(rela_vm* vm) {
	item_t b = pop(vm);
	item_t a = pop(vm);
	push(vm, (item_t){.type = BOOLEAN, .flag = !less(vm, a, b)});
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

static void op_match(rela_vm* vm) {
#ifdef PCRE
	item_t pattern = pop_type(vm, STRING);
	item_t subject = pop_type(vm, STRING);

	const char *error;
	int erroffset;
	int ovector[STRTMP];
	pcre_extra *extra = NULL;

	pcre *re = pcre_compile(pattern.str, PCRE_DOTALL|PCRE_UTF8, &error, &erroffset, 0);
	ensure(vm, re, "pcre_compile");

#ifdef PCRE_STUDY_JIT_COMPILE
	error = NULL;
	extra = pcre_study(re, PCRE_STUDY_JIT_COMPILE, &error);
	ensure(vm, extra && !error, "pcre_study");
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

static void op_slurp(rela_vm* vm) {
	item_t path = pop(vm);
	push(vm, nil(vm));

	struct stat st;
	if (stat(path.str, &st) == 0) {
		FILE *file = fopen(path.str, "r");

		if (file) {
			size_t bytes = st.st_size;
			void *ptr = malloc(bytes + 1);

			size_t read = 0;
			for (int i = 0; i < 3; i++) {
				read += fread(ptr + read, 1, bytes - read, file);
				if (read == bytes) break;
			}
			((char*)ptr)[bytes] = 0;

			ensure(vm, read == bytes, "fread failed");

			pop(vm);
			push(vm, string(vm, ptr));
			free(ptr);
		}
		fclose(file);
	}
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

static void op_marks(rela_vm* vm) {
	for (int i = 0, l = literal_int(vm); i < l; i++) op_mark(vm);
}

static void op_fname(rela_vm* vm) {
	item_t key = literal(vm);
	item_t val = nil(vm);

	char tmp[STRTMP];
	ensure(vm, find(vm, key, &val), "unknown name: %s", tmptext(vm, key, tmp, sizeof(tmp)));

	push(vm, val);
}

static void op_gname(rela_vm* vm) {
	item_t key = literal(vm);
	item_t src = pop(vm);
	push(vm, get(vm, src, key));
}

static void op_cfunc(rela_vm* vm) {
	op_fname(vm);
	op_call(vm);
}

// compression of mark,lit,lit,assign,limit0
static void op_a2lit(rela_vm* vm) {
	item_t key = literal(vm);
	item_t val = pop(vm);
	assign(vm, key, val);
}

// assign0,limit0
static void op_assignl(rela_vm* vm) {
	item_t key = pop(vm);
	item_t val = pop(vm);
	assign(vm, key, val);
	limit(vm, 0);
}

// unmap,limit1
static void op_unmapl(rela_vm* vm) {
	op_unmap(vm);
	limit(vm, 1);
}

func_t funcs[OPERATIONS] = {
	[OP_NOP]       = { .name = "nop",       .func = op_nop       },
	[OP_PRINT]     = { .name = "print",     .func = op_print     },
	[OP_COROUTINE] = { .name = "coroutine", .func = op_coroutine },
	[OP_RESUME]    = { .name = "resume",    .func = op_resume    },
	[OP_YIELD]     = { .name = "yield",     .func = op_yield     },
	[OP_CALL]      = { .name = "call",      .func = op_call      },
	[OP_RETURN]    = { .name = "return",    .func = op_return    },
	[OP_GLOBAL]    = { .name = "global",    .func = op_global    },
	[OP_VECTOR]    = { .name = "vector",    .func = op_vector    },
	[OP_MAP]       = { .name = "map",       .func = op_map       },
	[OP_UNMAP]     = { .name = "unmap",     .func = op_unmap     },
	[OP_MARK]      = { .name = "mark",      .func = op_mark      },
	[OP_LIMIT]     = { .name = "limit",     .func = op_limit     },
	[OP_LOOP]      = { .name = "loop",      .func = op_loop      },
	[OP_UNLOOP]    = { .name = "unloop",    .func = op_unloop    },
	[OP_CLEAN]     = { .name = "clean",     .func = op_clean     },
	[OP_BREAK]     = { .name = "break",     .func = op_break     },
	[OP_CONTINUE]  = { .name = "continue",  .func = op_continue  },
	[OP_AND]       = { .name = "and",       .func = op_and       },
	[OP_OR]        = { .name = "or",        .func = op_or        },
	[OP_JMP]       = { .name = "jmp",       .func = op_jmp       },
	[OP_JFALSE]    = { .name = "jfalse",    .func = op_jfalse    },
	[OP_JTRUE]     = { .name = "jtrue",     .func = op_jtrue     },
	[OP_FOR]       = { .name = "for",       .func = op_for       },
	[OP_NIL]       = { .name = "nil",       .func = op_nil       },
	[OP_SHUNT]     = { .name = "shunt",     .func = op_shunt     },
	[OP_SHIFT]     = { .name = "shift",     .func = op_shift     },
	[OP_TRUE]      = { .name = "true",      .func = op_true      },
	[OP_FALSE]     = { .name = "false",     .func = op_false     },
	[OP_LIT]       = { .name = "lit",       .func = op_lit       },
	[OP_ASSIGN]    = { .name = "assign",    .func = op_assign    },
	[OP_FIND]      = { .name = "find",      .func = op_find      },
	[OP_SET]       = { .name = "set",       .func = op_set       },
	[OP_GET]       = { .name = "get",       .func = op_get       },
	[OP_COUNT]     = { .name = "count",     .func = op_count     },
	[OP_DROP]      = { .name = "drop",      .func = op_drop      },
	[OP_ADD]       = { .name = "add",       .func = op_add       },
	[OP_NEG]       = { .name = "neg",       .func = op_neg       },
	[OP_SUB]       = { .name = "sub",       .func = op_sub       },
	[OP_MUL]       = { .name = "mul",       .func = op_mul       },
	[OP_DIV]       = { .name = "div",       .func = op_div       },
	[OP_MOD]       = { .name = "mod",       .func = op_mod       },
	[OP_NOT]       = { .name = "not",       .func = op_not       },
	[OP_EQ]        = { .name = "eq",        .func = op_eq        },
	[OP_NE]        = { .name = "ne",        .func = op_ne        },
	[OP_LT]        = { .name = "lt",        .func = op_lt        },
	[OP_LTE]       = { .name = "lte",       .func = op_lte       },
	[OP_GT]        = { .name = "gt",        .func = op_gt        },
	[OP_GTE]       = { .name = "gte",       .func = op_gte       },
	[OP_CONCAT]    = { .name = "concat",    .func = op_concat    },
	[OP_MATCH]     = { .name = "match",     .func = op_match     },
	[OP_SLURP]     = { .name = "slurp",     .func = op_slurp     },
	[OP_SORT]      = { .name = "sort",      .func = op_sort      },
	[OP_PID]       = { .name = "pid",       .func = op_pid       },
	[OP_ASSERT]    = { .name = "assert",    .func = op_assert    },
	[OP_GC]        = { .name = "collect",   .func = gc           },
	// peephole
	[OPP_MARKS]    = { .name = "marks",     .func = op_marks     },
	[OPP_FNAME]    = { .name = "fname",     .func = op_fname     },
	[OPP_GNAME]    = { .name = "gname",     .func = op_gname     },
	[OPP_CNAME]    = { .name = "cfunc",     .func = op_cfunc     },
	[OPP_A2LIT]    = { .name = "a2lit",     .func = op_a2lit     },
	[OPP_ASSIGNL]  = { .name = "assignl",   .func = op_assignl   },
	[OPP_UNMAP]    = { .name = "unmapl",    .func = op_unmapl    },
};

static void decompile(rela_vm* vm, code_t* c) {
	char tmp[STRTMP];
	const char *str = tmptext(vm, c->item, tmp, sizeof(tmp));
	fprintf(stderr, "%04ld  %-10s  %s\n", c - vm->code.cells, funcs[c->op].name, str);
	fflush(stderr);
}

static int run(rela_vm* vm) {
	int wtf = setjmp(vm->jmp);
	if (wtf) {
		char tmp[STRTMP];
		fprintf(stderr, "%s (", vm->err);
		fprintf(stderr, "ip %d ", vec_size(vm, &vm->routines) ? routine(vm)->ip: -1);
		fprintf(stderr, "stack %s", vec_size(vm, &vm->routines) ? tmptext(vm, (item_t){.type = VECTOR, .vec = stack(vm)}, tmp, sizeof(tmp)): "(no routine)");
		fprintf(stderr, ")\n");
		reset(vm);
		return wtf;
	}

	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor_allot(vm)});
	routine(vm)->ip = vm->code.start;

	vm->scope_global = map_allot(vm);

	for (;;) {
		int ip = routine(vm)->ip++;
		if (ip < 0 || ip >= vm->code.depth) break;
		funcs[vm->code.cells[ip].op].func(vm);
//		char tmp[STRTMP];
//		fprintf(stderr, "ip %d ", ip);
//		fprintf(stderr, "stack %s", tmptext(vm, (item_t){.type = VECTOR, .vec = stack(vm)}, tmp, sizeof(tmp)));
//		fprintf(stderr, "\n");
	}

	reset(vm);
	return 0;
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

	while (vm->stringsA.depth > 0) free(vm->stringsA.cells[--vm->stringsA.depth]);
	while (vm->stringsB.depth > 0) free(vm->stringsB.cells[--vm->stringsB.depth]);
	free(vm->stringsA.cells);
	free(vm->stringsB.cells);

	pool_clear(&vm->maps);
	pool_clear(&vm->vecs);
	pool_clear(&vm->cors);

	free(vm);
}

static rela_vm* create(const char* src, void* custom, size_t registrations, const rela_register* registry) {
	rela_vm* vm = calloc(sizeof(rela_vm),1);
	if (!vm) exit(1);

	vm->maps.page = 1024;
	vm->maps.object = sizeof(map_t);
	vm->vecs.page = 8192;
	vm->vecs.object = sizeof(vec_t);
	vm->cors.page = 128;
	vm->cors.object = sizeof(cor_t);

	vm->custom = custom;
	vm->scope_core = map_allot(vm);

	if (setjmp(vm->jmp)) {
		fprintf(stderr, "%s\n", vm->err);
		destroy(vm);
		return NULL;
	}

	map_set(vm, vm->scope_core, string(vm, "print"), (item_t){.type = SUBROUTINE, .sub = vm->code.depth});
	compile(vm, OP_PRINT, nil(vm));
	compile(vm, OP_RETURN, nil(vm));

	vm->code.start = vm->code.depth;

	for (int i = 0; i < registrations; i++) {
		const rela_register* reg = &registry[i];
		map_set(vm, vm->scope_core,
			string(vm, (char*)reg->name),
			(item_t){.type = CALLBACK, .cb = reg->func}
		);
	}

	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor_allot(vm)});
	op_mark(vm);
	routine(vm)->ip = vm->code.depth;
	push(vm, string(vm, (char*)src)); // shouldn't intern the src

	op_slurp(vm);
	ensure(vm, top(vm).type == STRING, "cannot read %s", src);

	source(vm, pop_type(vm, STRING).str);
	assert(!vec_size(vm, stack(vm)));

	op_unmark(vm);
	vec_pop(vm, &vm->routines);

	while (vm->nodes.cells && vm->nodes.depth > 0)
		free(vm->nodes.cells[--vm->nodes.depth]);
	free(vm->nodes.cells);
	vm->nodes.cells = NULL;

	memmove(&vm->stringsB, &vm->stringsA, sizeof(string_region_t));
	memset(&vm->stringsA, 0, sizeof(string_region_t));

	gc(vm);
	return vm;
}

rela_vm* rela_create(const char* src, void* custom, size_t registrations, const rela_register* registry) {
	return (rela_vm*) create(src, custom, registrations, registry);
}

void* rela_custom(rela_vm* vm) {
	return vm->custom;
}

int rela_run(rela_vm* vm) {
	return run(vm);
}

void rela_destroy(rela_vm* vm) {
	destroy(vm);
}

void rela_decompile(rela_vm* vm) {
	for (int i = 0, l = vm->code.depth; i < l; i++)
		decompile(vm, &vm->code.cells[i]);
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

