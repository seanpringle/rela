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

#ifdef PCRE
#include <pcre.h>
#endif

enum opcode_t {
	OP_NOP=1, OP_PRINT, OP_COROUTINE, OP_RESUME, OP_YIELD, OP_CALL, OP_RETURN, OP_GLOBAL, OP_LOCAL, OP_SCOPE,
	OP_SMUDGE, OP_LITSTACK, OP_UNSCOPE, OP_LITSCOPE, OP_MARK, OP_MARKS, OP_LIMIT, OP_LOOP, OP_UNLOOP, OP_REPLY,
	OP_BREAK, OP_CONTINUE, OP_JMP, OP_JFALSE, OP_JTRUE, OP_AND, OP_OR, OP_FOR, OP_NIL, OP_SHUNT, OP_SHIFT,
	OP_TRUE, OP_FALSE, OP_LIT, OP_ASSIGN, OP_FIND, OP_SET, OP_GET, OP_COUNT, OP_DROP, OP_ADD, OP_NEG, OP_SUB,
	OP_MUL, OP_DIV, OP_MOD, OP_NOT, OP_EQ, OP_NE, OP_LT, OP_GT, OP_LTE, OP_GTE, OP_CONCAT, OP_MATCH, OP_SLURP,
	OP_SORT, OP_ASSERT,
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
		char* str;
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
} vec_t;

typedef struct _map_t {
	vec_t keys;
	vec_t vals;
	bool smudged; // temporarily hidden
	bool immutable; // keys may only be written once
} map_t;

typedef struct _cor_t {
	vec_t stack;
	vec_t other;
	vec_t scopes;
	vec_t calls;
	vec_t loops;
	vec_t marks;
	int ip;
	int state;
} cor_t;

typedef struct{
	enum opcode_t op;
	item_t item;
} code_t;

typedef struct _node_t {
	int type;
	int opcode;
	int call;
	item_t item;
	struct _node_t *args;
	struct _node_t *chain;
	bool index;
	bool field;
	vec_t keys;
	vec_t vals;
	int results;
} node_t;

typedef struct {
	char *name;
	int opcode;
} keyword_t;

keyword_t keywords[] = {
	{ .name = "global", .opcode = OP_GLOBAL },
	{ .name = "local", .opcode = OP_LOCAL },
	{ .name = "true", .opcode = OP_TRUE },
	{ .name = "false", .opcode = OP_FALSE },
	{ .name = "nil", .opcode = OP_NIL },
	{ .name = "assert", .opcode = OP_ASSERT },
	{ .name = "sort", .opcode = OP_SORT },
	{ .name = "slurp", .opcode = OP_SLURP },
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

typedef struct {
	char *name;
	int opcode;
} modifier_t;

modifier_t modifiers[] = {
	{ .name = "#", .opcode = OP_COUNT },
	{ .name = "-", .opcode = OP_NEG   },
	{ .name = "!", .opcode = OP_NOT   },
};

typedef struct {
	void* ptr;
	size_t bytes;
} allocation_t;

typedef struct _rela_vm {
	void* custom;
	size_t memory_limit;
	size_t memory_usage;

	map_t scope_core;
	map_t* scope_global;
	vec_t routines;

	struct {
		code_t* cells;
		int depth;
		int start;
	} code;

	struct {
		allocation_t* cells;
		int depth;
		int limit;
		int start;
	} allocations;

	struct {
		char* cells[1024];
		int depth;
		int start;
	} strings;

	vec_t scope_cache;

	jmp_buf jmp;
	char err[100];
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
static char* tmptext(rela_vm* vm, item_t item, char* tmp, int size);
static cor_t* routine(rela_vm* vm);
static int parse(rela_vm* vm, char *source, int results, int mode);
static int parse_block(rela_vm* vm, char *source, node_t *node);
static int parse_branch(rela_vm* vm, char *source, node_t *node);
static int parse_arglist(rela_vm* vm, char *source);
static int parse_node(rela_vm* vm, char *source);

#define ensure(vm,c,...) if (!(c)) { snprintf(vm->err, sizeof(vm->err), __VA_ARGS__); longjmp(vm->jmp, 1); }

#define RESULTS_DISCARD 0
#define RESULTS_FIRST 1
#define RESULTS_ALL -1

#define PARSE_GREEDY 1
#define PARSE_KEYVAL 2

#define PROCESS_ASSIGN (1<<0)
#define PROCESS_CHAIN (1<<1)

#define COR_SUSPENDED 0
#define COR_RUNNING 1
#define COR_DEAD 2

static void* allot(rela_vm* vm, size_t bytes) {
	assert(vm->memory_usage <= vm->memory_limit);
	ensure(vm, vm->memory_usage+bytes <= vm->memory_limit, "out of memory");

	void* ptr = calloc(bytes,1);
	ensure(vm, ptr, "calloc");

	if (!vm->allocations.cells) {
		vm->allocations.depth = 0;
		vm->allocations.limit = 128;
		vm->allocations.cells = malloc(vm->allocations.limit * sizeof(allocation_t));
	}

	if (vm->allocations.depth == vm->allocations.limit) {
		vm->allocations.limit *= 2;
		vm->allocations.cells = realloc(vm->allocations.cells, vm->allocations.limit * sizeof(allocation_t));
	}

	vm->allocations.cells[vm->allocations.depth++] = (allocation_t){.ptr = ptr, .bytes = bytes};
	vm->memory_usage += bytes;
	return ptr;
}

static void* reallot(rela_vm* vm, void* ptr, size_t bytes) {
	assert(vm->memory_usage <= vm->memory_limit);

	for (int i = (int)vm->allocations.depth-1; i >= 0; --i) {
		allocation_t* allocation = &vm->allocations.cells[i];

		if (allocation->ptr == ptr) {
			assert(vm->memory_usage >= allocation->bytes);
			ensure(vm, vm->memory_usage-allocation->bytes+bytes <= vm->memory_limit, "out of memory");
			vm->memory_usage -= allocation->bytes;
			allocation->ptr = realloc(ptr, bytes);
			assert(allocation->ptr);
			allocation->bytes = bytes;
			vm->memory_usage += allocation->bytes;
			return allocation->ptr;
		}
	}

	ensure(vm, 0, "bad reallot");
	return NULL;
}

static void discard(rela_vm* vm, void* ptr) {
	// interned strings may already have multiple references
	for (int i = (int)vm->strings.depth-1; i >= 0; --i) {
		if (vm->strings.cells[i] == ptr) return;
	}

	for (int i = (int)vm->allocations.depth-1; i >= 0; --i) {
		allocation_t* allocation = &vm->allocations.cells[i];

		if (allocation->ptr == ptr) {
			assert(vm->memory_usage >= allocation->bytes);
			vm->memory_usage -= allocation->bytes;
			free(allocation->ptr);
			allocation->ptr = NULL;
			allocation->bytes = 0;
			// only shrink the region if discarding the last allot
			while (vm->allocations.depth > 0 && !vm->allocations.cells[vm->allocations.depth-1].ptr)
				vm->allocations.depth--;
			return;
		}
	}

	ensure(vm, 0, "bad discard");
}

static vec_t* vec_alloc(rela_vm* vm) {
	vec_t* vec = allot(vm, sizeof(vec_t));
	vec->items = NULL;
	vec->count = 0;
	return vec;
}

static size_t vec_size(rela_vm* vm, vec_t* vec) {
	return vec->count;
}

static item_t* vec_ins(rela_vm* vm, vec_t* vec, int index) {
	ensure(vm, index >= 0 && index <= vec->count, "vec_ins out of bounds");

	if (!vec->items) {
		assert(vec->count == 0);
		vec->items = allot(vm, sizeof(item_t) * 8);
	}

	vec->count++;

	int size = vec->count;
	bool power_of_2 = size > 0 && (size & (size - 1)) == 0;

	if (power_of_2 && size >= 8)
		vec->items = reallot(vm, vec->items, sizeof(item_t) * (size * 2));

	if (index < vec->count-1)
		memmove(&vec->items[index+1], &vec->items[index], (vec->count - index - 1) * sizeof(item_t));

	vec->items[index].type = NIL;

	return &vec->items[index];
}

static void vec_push(rela_vm* vm, vec_t* vec, item_t item) {
	vec_ins(vm, vec, vec->count)[0] = item;
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

static map_t* map_alloc(rela_vm* vm) {
	return allot(vm, sizeof(map_t));
}

static int map_lower_bound(rela_vm* vm, map_t* map, item_t key) {
	return vec_lower_bound(vm, &map->keys, key);
}

static item_t* map_ref(rela_vm* vm, map_t* map, item_t key) {
	int i = map_lower_bound(vm, map, key);
	return (i < vec_size(vm, &map->keys) && equal(vm, vec_get(vm, &map->keys, i), key)) ? vec_cell(vm, &map->vals, i): NULL;
}

static item_t map_get(rela_vm* vm, map_t* map, item_t key) {
	item_t* item = map_ref(vm, map, key);
	return item ? *item: nil(vm);
}

static void map_set(rela_vm* vm, map_t* map, item_t key, item_t val) {
	char tmp[100];
	int i = map_lower_bound(vm, map, key);
	if (i < vec_size(vm, &map->keys) && equal(vm, vec_get(vm, &map->keys, i), key)) {
		ensure(vm, !map->immutable, "attempt to change immutable key: %s", tmptext(vm, key, tmp, sizeof(tmp)));
		vec_cell(vm, &map->vals, i)[0] = val;
	}
	else {
		vec_ins(vm, &map->keys, i)[0] = key;
		vec_ins(vm, &map->vals, i)[0] = val;
	}
}

static void map_clear(rela_vm* vm, map_t* map) {
	vec_clear(vm, &map->keys);
	vec_clear(vm, &map->vals);
}

static char* strintern(rela_vm* vm, char* str) {
	ensure(vm, str, "strintern() null string");

	int size = vm->strings.depth;
	int index = 0;

	if (size) {
		int lower = 0;
		int upper = size-1;

		while (lower <= upper) {
			int i = (int)floor((float)(lower + upper) / 2.0f);
			int c = strcmp(vm->strings.cells[i], str);
			if (c == 0) { index = i; break; }
			if (c < 0) lower = i+1; else upper = i-1;
			index = lower;
		}

		if (index < vm->strings.depth && vm->strings.cells[index] == str) {
			return str;
		}

		if (index < vm->strings.depth && strcmp(vm->strings.cells[index], str) == 0) {
			discard(vm, str);
			return vm->strings.cells[index];
		}
	}

	vm->strings.depth++;
	assert(index >= 0 && index < vm->strings.depth);

	if (index == vm->strings.depth-1) {
		vm->strings.cells[index] = str;
		return str;
	}

	memmove(&vm->strings.cells[index+1], &vm->strings.cells[index], (vm->strings.depth - index - 1) * sizeof(char*));

	vm->strings.cells[index] = str;
	return str;
}

static void reset(rela_vm* vm) {
	while (vec_size(vm, &vm->routines)) vec_pop(vm, &vm->routines);

	vec_clear(vm, &vm->scope_cache);
	vm->scope_cache.items = NULL;

	for (int i = vm->allocations.start; i < vm->allocations.depth; i++) {
		allocation_t* allocation = &vm->allocations.cells[i];
		free(allocation->ptr);
		allocation->ptr = NULL;
		allocation->bytes = 0;
	}

	vm->allocations.depth = vm->allocations.start;
	vm->scope_global = NULL;
}

static char* strcopy(rela_vm* vm, const char* str) {
	char* cpy = allot(vm, strlen(str)+1);
	strcpy(cpy, str);
	return strintern(vm, cpy);
}

static char* strf(rela_vm *vm, char *pattern, ...) {
	char *result = NULL;
	va_list args;
	char buffer[8];

	va_start(args, pattern);
	int len = vsnprintf(buffer, sizeof(buffer), pattern, args);
	va_end(args);

	assert(len > -1);
	result = allot(vm, len+1);
	assert(result);

	va_start(args, pattern);
	vsnprintf(result, len+1, pattern, args);
	va_end(args);

	return strintern(vm,result);
}

static char* substr(rela_vm* vm, char *start, int offset, int length) {
	char *buffer = allot(vm, length+1);
	memcpy(buffer, start+offset, length);
	buffer[length] = 0;
	return strintern(vm,buffer);
}

static char* strliteral(rela_vm* vm, char *str, char **err) {
	char *res = allot(vm, strlen(str)+1);
	char *rp = res, *sp = str;

	sp++;

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
	}
	*rp = 0;

	if (err)
		*err = sp;

	return strintern(vm,res);
}

static int str_skip(char *source, strcb cb) {
	int offset = 0;
	while (source[offset] && cb(source[offset])) offset++;
	return offset;
}

static int str_scan(char *source, strcb cb) {
	int offset = 0;
	while (source[offset] && !cb(source[offset])) offset++;
	return offset;
}

static item_t nil(rela_vm* vm) {
	return (item_t){.type = NIL, .str = NULL};
}

static item_t integer(rela_vm* vm, int64_t i) {
	return (item_t){.type = INTEGER, .inum = i};
}

static item_t string(rela_vm* vm, char* s) {
	return (item_t){.type = STRING, .str = s};
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
	if (a.type == FLOAT && b.type == INTEGER) return (item_t){.type = FLOAT, .inum = a.fnum * b.inum};
	if (a.type == FLOAT && b.type == FLOAT) return (item_t){.type = FLOAT, .inum = a.fnum * b.fnum};
	return nil(vm);
}

static item_t divide(rela_vm* vm, item_t a, item_t b) {
	if (a.type == INTEGER && b.type == INTEGER) return (item_t){.type = INTEGER, .inum = a.inum / b.inum};
	if (a.type == INTEGER && b.type == FLOAT) return (item_t){.type = INTEGER, .inum = a.inum / b.fnum};
	if (a.type == FLOAT && b.type == INTEGER) return (item_t){.type = FLOAT, .inum = a.fnum / b.inum};
	if (a.type == FLOAT && b.type == FLOAT) return (item_t){.type = FLOAT, .inum = a.fnum / b.fnum};
	return nil(vm);
}

static char* tmptext(rela_vm* vm, item_t a, char* tmp, int size) {
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

	char subtmpA[50];
	char subtmpB[50];

	if (a.type == VECTOR) {
		int len = snprintf(tmp, size, "[");
		for (int i = 0, l = vec_size(vm, a.vec); len < size && i < l; i++) {
			len += snprintf(tmp+len, size-len, "%s", tmptext(vm, vec_get(vm, a.vec, i), subtmpA, sizeof(subtmpA)));
			if (i < l-1) len += snprintf(tmp+len, size-len, ", ");
		}
		len += snprintf(tmp+len, size-len, "]");
	}

	if (a.type == MAP) {
		int len = snprintf(tmp, size, "{");
		for (int i = 0, l = vec_size(vm, &a.map->keys); len < size && i < l; i++) {
			len += snprintf(tmp+len, size-len, "%s = %s",
				tmptext(vm, vec_get(vm, &a.map->keys, i), subtmpA, sizeof(subtmpA)),
				tmptext(vm, vec_get(vm, &a.map->vals, i), subtmpB, sizeof(subtmpB))
			);
			if (i < l-1) len += snprintf(tmp+len, size-len, ", ");
		}
		len += snprintf(tmp+len, size-len, "}");
	}

	assert(tmp[0]);
	return tmp;
}

static cor_t* cor_alloc(rela_vm* vm) {
	return allot(vm, sizeof(cor_t));
}

static code_t* compile(rela_vm* vm, int op) {
	if (!vm->code.cells) {
		vm->code.cells = malloc(1024 * sizeof(code_t));
	}
	if (vm->code.depth%1024 == 0) {
		vm->code.cells = realloc(vm->code.cells, (vm->code.depth+1024) * sizeof(code_t));
	}

	// some peephole
	if (vm->code.depth) {
		code_t* last = &vm->code.cells[vm->code.depth-1];

		// scope_core is immutable, so compile stuff inline
		if (op == OP_FIND && last->op == OP_LIT && last->item.type == STRING) {
			item_t val = map_get(vm, &vm->scope_core, last->item);
			if (val.type != NIL) {
				last->item = val;
				op = OP_NOP;
			}
		}
	}

	code_t* c = &vm->code.cells[vm->code.depth++];
	c->op = op;
	c->item.type = 0;
	c->item.inum = 0;
	return c;
}

static cor_t* routine(rela_vm* vm) {
	return vec_top(vm, &vm->routines).cor;
}

static map_t* scope_writing(rela_vm* vm) {
	return vec_size(vm, &routine(vm)->scopes) ? vec_top(vm, &routine(vm)->scopes).map: vm->scope_global;
}

static map_t* scope_reading(rela_vm* vm) {
	for (int i = 0; i < vec_size(vm, &routine(vm)->scopes); i++) {
		map_t *map = vec_get(vm, &routine(vm)->scopes, -i-1).map;
		if (!map->smudged) return map;
	}
	return vm->scope_global;
}

static vec_t* stack(rela_vm* vm) {
	return &routine(vm)->stack;
}

static int depth(rela_vm* vm) {
	return vec_size(vm, stack(vm)) - vec_cell(vm, &routine(vm)->marks, -1)->inum;
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
	return vec_cell(vm, stack(vm), i);
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

static int skip_gap(char *source) {
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

static int peek(char *source, char *name) {
	int len = strlen(name);
	return !strncmp(source, name, len) && !isname(source[len]);
}

static node_t* node_alloc(rela_vm* vm) {
	return allot(vm, sizeof(node_t));
}

static int parse_block(rela_vm* vm, char *source, node_t *node) {
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
		vec_push(vm, &node->vals, pop(vm));
	}

	ensure(vm, found_end, "expected keyword 'end': %s", &source[offset]);
	return offset;
}

static int parse_branch(rela_vm* vm, char *source, node_t *node) {
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
		vec_push(vm, &node->vals, pop(vm));
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
			vec_push(vm, &node->keys, pop(vm));
		}
	}

	ensure(vm, found_else || found_end, "expected keyword 'else' or 'end': %s", &source[offset]);

	if (vec_size(vm, &node->vals))
		vec_cell(vm, &node->vals, vec_size(vm, &node->vals)-1)->node->results = 1;

	if (vec_size(vm, &node->keys))
		vec_cell(vm, &node->keys, vec_size(vm, &node->keys)-1)->node->results = 1;

	return offset;
}

static int parse_arglist(rela_vm* vm, char *source) {
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
static int parse_node(rela_vm* vm, char *source) {
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
		node_t* outer = node_alloc(vm);
		outer->type = NODE_OPCODE;
		outer->opcode = modifier->opcode;
		outer->args = pop(vm).node;
		outer->results = 1;
		push(vm, (item_t){.type = NODE, .node = outer});
		return offset;
	}

	node_t *node = node_alloc(vm);

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

				// then block, optional else
				offset += parse_branch(vm, &source[offset], node);
			}
			else
			if (peek(&source[offset], "while")) {
				offset += 5;
				node->type = NODE_WHILE;

				// conditions
				offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_GREEDY);
				node->args = pop(vm).node;

				// do block
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
				vec_push(vm, &node->keys, string(vm, substr(vm, &source[offset], 0, length)));
				offset += length;

				offset += skip_gap(&source[offset]);
				if (source[offset] == ',') {
					offset++;
					length = str_skip(&source[offset], isname);
					vec_push(vm, &node->keys, string(vm, substr(vm, &source[offset], 0, length)));
					offset += length;
				}

				offset += skip_gap(&source[offset]);
				if (peek(&source[offset], "in")) offset += 2;

				// iterable
				offset += parse(vm, &source[offset], RESULTS_FIRST, PARSE_GREEDY);
				node->args = pop(vm).node;

				// do block
				offset += parse_block(vm, &source[offset], node);
			}
			else
			if (peek(&source[offset], "function")) {
				offset += 8;
				node->type = NODE_FUNCTION;

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

						node_t *param = node_alloc(vm);
						param->type = NODE_NAME;
						param->item = string(vm, substr(vm, &source[offset], 0, length));
						vec_push(vm, &node->keys, (item_t){.type = NODE, .node = param});

						offset += length;
					}
				}

				// do block
				offset += parse_block(vm, &source[offset], node);
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
	if (source[offset] == '[' && source[offset+1] == '[') {
		node->type = NODE_LITERAL;

		char *start = &source[offset+2];
		char *end = start;

		while (*end && !(end[0] == ']' && end[1] == ']'))
			end = strchr(end, ']');

		ensure(vm, end[0] == ']' && end[1] == ']', "expected closing bracket: %s", &source[offset]);

		node->item = string(vm, substr(vm, start, 0, end - start));
		offset += end - &source[offset] + 2;
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
		offset += skip_gap(&source[offset]);
		if (source[offset] != ']') {
			offset += parse(vm, &source[offset], RESULTS_ALL, PARSE_GREEDY);
			node->args = pop(vm).node;
			offset += skip_gap(&source[offset]);
		}
		ensure(vm, source[offset] == ']', "expected closing bracket: %s", &source[offset]);
		offset++;
	}
	else
	if (source[offset] == '{') {
		offset++;
		node->type = NODE_MAP;

		while (source[offset]) {
			if ((length = skip_gap(&source[offset])) > 0)
			{
				offset += length;
				continue;
			}
			if (source[offset] == '}')
			{
				offset++;
				break;
			}
			if (source[offset] == ',')
			{
				offset++;
				continue;
			}
			offset += parse(vm, &source[offset], RESULTS_DISCARD, PARSE_KEYVAL);
			vec_push(vm, &node->vals, pop(vm));
		}
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
				node_t* call = node_alloc(vm);
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

static int parse(rela_vm* vm, char *source, int results, int mode) {
	int length = 0;
	int offset = skip_gap(source);

	node_t *node = node_alloc(vm);
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
				node_t *result = node_alloc(vm);
				result->type   = NODE_OPCODE;
				result->opcode = consume->opcode;
				ensure(vm, argument >= consume->argc, "operator %s insufficient arguments", consume->name);
				for (int j = consume->argc; j > 0; --j) {
					vec_push(vm, &result->vals, (item_t){.type = NODE, .node = arguments[argument-j]});
				}
				argument -= consume->argc;
				arguments[argument++] = result;
			}

			operations[operation++] = compare;
		}

		while (operation && argument) {
			operator_t *consume = operations[--operation];
			node_t *result = node_alloc(vm);
			result->type   = NODE_OPCODE;
			result->opcode = consume->opcode;
			ensure(vm, argument >= consume->argc, "operator %s insufficient arguments", consume->name);
			for (int j = consume->argc; j > 0; --j) {
				vec_push(vm, &result->vals, (item_t){.type = NODE, .node = arguments[argument-j]});
			}
			argument -= consume->argc;
			arguments[argument++] = result;
		}

		ensure(vm, !operation && argument == 1, "unbalanced expression");
		vec_push(vm, &node->vals, (item_t){.type = NODE, .node = arguments[0]});

		offset += skip_gap(&source[offset]);

		if (source[offset] == '=') {
			ensure(vm, vec_size(vm, &node->vals) > 0, "missing assignment name: %s", &source[offset]);

			offset++;
			for (int i = 0; i < vec_size(vm, &node->vals); i++)
				vec_push(vm, &node->keys, vec_get(vm, &node->vals, i));

			vec_clear(vm, &node->vals);
			continue;
		}

		if (source[offset] == ',' && mode == PARSE_GREEDY) {
			offset++;
			continue;
		}

		break;
	}

	ensure(vm, vec_size(vm, &node->vals) > 0, "missing assignment value: %s", &source[offset]);

	push(vm, (item_t){.type = NODE, .node = node});
	return offset;
}

static void process(rela_vm* vm, node_t *node, int flags, int index) {
	int flag_assign = flags & PROCESS_ASSIGN ? 1:0;

	// if we're assigning with chained expressions, only OP_SET|OP_ASSIGN the last one
	bool assigning = flag_assign && !node->chain;

	// a multi-part expression: a[,b...] = node[,node...]
	// this is the entry point for most non-control non-opcode statements
	if (node->type == NODE_MULTI) {
		assert(!node->args);
		assert(vec_size(vm, &node->vals));

		// stack frame
		compile(vm, OP_MARK);

		// stream the values onto the stack
		for (int i = 0; i < vec_size(vm, &node->vals); i++)
			process(vm, vec_get(vm, &node->vals, i).node, 0, 0);

		// OP_SET|OP_ASSIGN index values from the bottom of the current stack frame
		for (int i = 0; i < vec_size(vm, &node->keys); i++) {
			node_t* subnode = vec_get(vm, &node->keys, i).node;
			process(vm, subnode, PROCESS_ASSIGN, i);
		}

		// end stack frame
		compile(vm, OP_LIMIT)->item = integer(vm, node->results);
	}
	else
	if (node->type == NODE_NAME) {
		assert(!vec_size(vm, &node->keys) && !vec_size(vm, &node->vals));

		// function or method call
		if (node->call) {

			// vecmap[fn()]
			if (node->index) {
				compile(vm, OP_MARK);
				if (node->args)
					process(vm, node->args, 0, 0);
				compile(vm, OP_LIT)->item = node->item;
				compile(vm, OP_FIND);
				compile(vm, OP_CALL);
				compile(vm, OP_LIMIT)->item = integer(vm, 1);
				compile(vm, assigning ? OP_SET: OP_GET)->item = integer(vm, index);
			}

			// .fn()
			if (node->field) {
				compile(vm, OP_LIT)->item = node->item;
				compile(vm, assigning ? OP_SET: OP_GET)->item = integer(vm, index);
				if (node->args) {
					compile(vm, OP_SHUNT);
					process(vm, node->args, 0, 0);
					compile(vm, OP_SHIFT);
				}
				compile(vm, OP_CALL);
			}

			// fn()
			if (!node->index && !node->field) {
				if (node->args)
					process(vm, node->args, 0, 0);
				compile(vm, OP_LIT)->item = node->item;
				compile(vm, assigning ? OP_ASSIGN: OP_FIND)->item = integer(vm, index);
				compile(vm, OP_CALL);
			}
		}
		// variable reference
		else {
			compile(vm, OP_LIT)->item = node->item;

			if (node->index || node->field) {
				compile(vm, assigning ? OP_SET: OP_GET)->item = integer(vm, index);
			}

			if (!node->index && !node->field) {
				compile(vm, assigning ? OP_ASSIGN: OP_FIND)->item = integer(vm, index);
			}
		}

		if (node->chain) {
			process(vm, node->chain, flag_assign ? PROCESS_ASSIGN: 0, 0);
		}
	}
	else
	// inline opcode
	if (node->type == NODE_OPCODE) {
		if (node->opcode == OP_CALL)
			compile(vm, OP_SHUNT);

		if (node->args)
			process(vm, node->args, 0, 0);

		if (node->opcode == OP_CALL)
			compile(vm, OP_SHIFT);

		if (node->opcode == OP_AND || node->opcode == OP_OR) {
			process(vm, vec_get(vm, &node->vals, 0).node, 0, 0);
			code_t *jump = compile(vm, node->opcode);
			process(vm, vec_get(vm, &node->vals, 1).node, 0, 0);
			jump->item = integer(vm, vm->code.depth);
		}
		else {
			for (int i = 0; i < vec_size(vm, &node->vals); i++)
				process(vm, vec_get(vm, &node->vals, i).node, 0, 0);

			compile(vm, node->opcode);
		}

		if (node->index) {
			compile(vm, assigning ? OP_SET: OP_GET)->item = integer(vm, index);
		}

		if (node->chain) {
			process(vm, node->chain, flag_assign ? PROCESS_ASSIGN: 0, 0);
		}
	}
	else
	// string or number literal, optionally part of array chain a[b]["c"]
	if (node->type == NODE_LITERAL) {
		assert(!node->args && !vec_size(vm, &node->keys) && !vec_size(vm, &node->vals));

		char *dollar = node->item.type == STRING ? strchr(node->item.str, '$'): NULL;

		if (dollar && dollar < node->item.str + strlen(node->item.str) - 1) {
			assert(node->item.type == STRING);

			char *str = node->item.str;
			char *left = str;
			char *right = str;

			bool started = false;

			while ((right = strchr(left, '$')) && right && *right) {
				char *start = right+1;
				char *finish = start;
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
					compile(vm, OP_LIT)->item = string(vm, substr(vm, left, 0, right-left+(length ? 0:1)));
					if (started) compile(vm, OP_CONCAT);
					started = true;
				}

				left = finish;

				if (length) {
					char *sub = substr(vm, start, 0, length);
					ensure(vm, length == parse(vm, sub, RESULTS_FIRST, PARSE_GREEDY), "string interpolation parsing failed");
					process(vm, pop(vm).node, 0, 0);
					if (started) compile(vm, OP_CONCAT);
					started = true;
				}
			}

			if (strlen(left)) {
				compile(vm, OP_LIT)->item = string(vm, substr(vm, left, 0, strlen(left)));
				if (started) compile(vm, OP_CONCAT);
				started = true;
			}
		}
		else {
			compile(vm, OP_LIT)->item = node->item;
		}

		if (node->index) {
			compile(vm, assigning ? OP_SET: OP_GET)->item = integer(vm, index);
		}

		if (node->chain) {
			process(vm, node->chain, flag_assign ? PROCESS_ASSIGN: 0, 0);
		}
	}
	else
	// a built-in function-like keyword with arguments
	if (node->type == NODE_BUILTIN) {
		assert(!vec_size(vm, &node->keys) && !vec_size(vm, &node->vals));

		compile(vm, OP_MARK);

		if (node->args)
			process(vm, node->args, 0, 0);

		compile(vm, node->opcode);
		compile(vm, OP_LIMIT)->item = integer(vm, node->results);
	}
	else
	// if expression (returns a value for ternary style)
	if (node->type == NODE_IF) {
		assert(&node->vals);

		// conditions
		if (node->args)
			process(vm, node->args, 0, 0);

		// if false, jump to else/end
		code_t *jump = compile(vm, OP_JFALSE);
		compile(vm, OP_DROP);

		// success block
		for (int i = 0; i < vec_size(vm, &node->vals); i++)
			process(vm, vec_get(vm, &node->vals, i).node, 0, 0);

		// optional failure block
		if (vec_size(vm, &node->keys)) {
			// jump success path past failure block
			code_t *jump2 = compile(vm, OP_JMP);
			jump->item = integer(vm, vm->code.depth);
			compile(vm, OP_DROP);

			// failure block
			for (int i = 0; i < vec_size(vm, &node->keys); i++)
				process(vm, vec_get(vm, &node->keys, i).node, 0, 0);

			jump2->item = integer(vm, vm->code.depth);
		}
		else {
			jump->item = integer(vm, vm->code.depth);
		}
	}
	else
	// while ... do ... end
	if (node->type == NODE_WHILE) {
		assert(&node->vals);

		compile(vm, OP_MARK);
		code_t *loop = compile(vm, OP_LOOP);
		int begin = vm->code.depth;

		// condition(s)
		if (node->args)
			process(vm, node->args, 0, 0);

		// if false, jump to end
		code_t *iter = compile(vm, OP_JFALSE);
		compile(vm, OP_DROP);

		// do ... end
		for (int i = 0; i < vec_size(vm, &node->vals); i++)
			process(vm, vec_get(vm, &node->vals, i).node, 0, 0);

		// clean up
		compile(vm, OP_JMP)->item = integer(vm, begin);
		iter->item = integer(vm, vm->code.depth);
		loop->item = integer(vm, vm->code.depth);
		compile(vm, OP_UNLOOP);
		compile(vm, OP_LIMIT);
	}
	else
	// for ... in ... do ... end
	if (node->type == NODE_FOR) {
		compile(vm, OP_MARK);

		// the iterable
		if (node->args)
			process(vm, node->args, 0, 0);

		// loop counter
		compile(vm, OP_LIT)->item = integer(vm, 0);

		compile(vm, OP_MARK);
		code_t *loop = compile(vm, OP_LOOP);

		int begin = vm->code.depth;

		code_t *iter = compile(vm, OP_FOR);
		// OP_FOR expects a vector with key[,val] variable names
		iter->item = (item_t){.type = VECTOR, .vec = &node->keys};

		// do block
		for (int i = 0; i < vec_size(vm, &node->vals); i++)
			process(vm, vec_get(vm, &node->vals, i).node, 0, 0);

		// clean up
		compile(vm, OP_JMP)->item = integer(vm, begin);
		vec_push(vm, iter->item.vec, integer(vm, vm->code.depth));
		loop->item = integer(vm, vm->code.depth);
		compile(vm, OP_UNLOOP);
		compile(vm, OP_LIMIT);
		compile(vm, OP_LIMIT);
	}
	else
	// function with optional name assignment
	if (node->type == NODE_FUNCTION) {
		assert(!node->args);

		compile(vm, OP_MARK);
		code_t *entry = compile(vm, OP_LIT);

		if (node->item.type) {
			code_t* name = compile(vm, OP_LIT);
			name->item = node->item;
			compile(vm, OP_ASSIGN);
		}

		code_t *jump = compile(vm, OP_JMP);
		entry->item = (item_t){.type = SUBROUTINE, .sub = vm->code.depth};

		for (int i = 0; i < vec_size(vm, &node->keys); i++)
			process(vm, vec_get(vm, &node->keys, i).node, PROCESS_ASSIGN, i);

		for (int i = 0; i < vec_size(vm, &node->vals); i++)
			process(vm, vec_get(vm, &node->vals, i).node, 0, 0);

		// if an explicit return expression is used, these instructions
		// will be dead code
		compile(vm, OP_REPLY);
		compile(vm, OP_RETURN);
		jump->item = integer(vm, vm->code.depth);
		// will sub dead code

		compile(vm, OP_LIMIT)->item = integer(vm, 1);
	}
	else
	// return 0 or more values
	if (node->type == NODE_RETURN) {
		compile(vm, OP_REPLY);

		if (node->args)
			process(vm, node->args, 0, 0);

		compile(vm, OP_RETURN);
	}
	else
	// literal vector [1,2,3]
	if (node->type == NODE_VEC) {
		compile(vm, OP_MARK);

		if (node->args)
			process(vm, node->args, 0, 0);

		compile(vm, OP_LITSTACK);
		compile(vm, OP_LIMIT)->item = integer(vm, 1);
	}
	else
	// literal map { a = 1, b = 2, c = nil }
	if (node->type == NODE_MAP) {
		compile(vm, OP_MARK);
		compile(vm, OP_SCOPE);
		compile(vm, OP_SMUDGE);

		if (node->args)
			process(vm, node->args, 0, 0);

		for (int i = 0; i < vec_size(vm, &node->vals); i++)
			process(vm, vec_get(vm, &node->vals, i).node, 0, 0);

		compile(vm, OP_LITSCOPE);
		compile(vm, OP_LIMIT)->item = integer(vm, 1);
	}
	else {
		ensure(vm, 0, "unexpected expression type: %d", node->type);
	}
}

static void source (rela_vm* vm, char *s) {
	int offset = skip_gap(s);

	while (s[offset])
		offset += parse(vm, &s[offset], RESULTS_DISCARD, PARSE_GREEDY);

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

	char tmp[100];
	for (int i = 0; i < items; i++) {
		char *str = tmptext(vm, *item++, tmp, sizeof(tmp));
		fprintf(stdout, "%s%s", i ? "\t": "", str);
	}
	fprintf(stdout, "\n");
	fflush(stdout);
}

static void op_scope(rela_vm* vm) {
	map_t* map = vec_size(vm, &vm->scope_cache) ? vec_pop(vm, &vm->scope_cache).map: map_alloc(vm);
	vec_push(vm, &routine(vm)->scopes, (item_t){.type = MAP, .map = map});
}

static void op_unscope(rela_vm* vm) {
	vec_push(vm, &vm->scope_cache, vec_pop(vm, &routine(vm)->scopes));
	map_clear(vm, vec_top(vm, &vm->scope_cache).map);
}

static void op_coroutine(rela_vm* vm) {
	cor_t *cor = cor_alloc(vm);
	cor->ip = pop_type(vm, SUBROUTINE).sub;

	cor->state = COR_RUNNING;
	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor});

	op_scope(vm);

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

static void op_local(rela_vm* vm) {
	push(vm, (item_t){.type = MAP, .map = scope_reading(vm)});
}

static void op_call(rela_vm* vm) {
	item_t item = pop(vm);

	if (item.type == CALLBACK) {
		item.cb((rela_vm*)vm);
		return;
	}

	char tmp[100];
	ensure(vm, item.type == SUBROUTINE, "invalid function: %s (ip: %u)", tmptext(vm, item, tmp, sizeof(tmp)), routine(vm)->ip);

	op_scope(vm);
	cor_t* cor = routine(vm);

	vec_push(vm, &cor->calls, integer(vm, vec_size(vm, &cor->loops)));
	vec_push(vm, &cor->calls, integer(vm, vec_size(vm, &cor->marks)));
	vec_push(vm, &cor->calls, integer(vm, cor->ip));

	cor->ip = item.sub;
}

static void op_return(rela_vm* vm) {
	op_unscope(vm);
	cor_t* cor = routine(vm);

	if (vec_size(vm, &cor->calls) == 0) {
		cor->state = COR_DEAD;
		op_yield(vm);
		return;
	}

	cor->ip = vec_pop(vm, &cor->calls).inum;

	ensure(vm, vec_pop(vm, &cor->calls).inum == vec_size(vm, &cor->marks), "mark stack mismatch (return)");
	ensure(vm, vec_pop(vm, &cor->calls).inum == vec_size(vm, &cor->loops), "loop stack mismatch (return)");
}

static void op_drop(rela_vm* vm) {
	pop(vm);
}

static void op_reply(rela_vm* vm) {
	cor_t* cor = routine(vm);
	while (vec_size(vm, &cor->marks) > vec_cell(vm, &cor->calls, -2)->inum)
		vec_pop(vm, &cor->marks);
	while (vec_size(vm, &cor->loops) > vec_cell(vm, &cor->calls, -3)->inum)
		vec_pop(vm, &cor->loops);
	while (depth(vm)) pop(vm);
}

static void op_lit(rela_vm* vm) {
	push(vm, literal(vm));
}

static void op_smudge(rela_vm* vm) {
	scope_writing(vm)->smudged = true;
}

static void op_litstack(rela_vm* vm) {
	int items = depth(vm);

	vec_t* vec = vec_alloc(vm);

	for (int i = 0; i < items; i++)
		vec_push(vm, vec, vec_get(vm, stack(vm), vec_size(vm, stack(vm)) - items + i));

	for (int i = 0; i < items; i++)
		vec_pop(vm, stack(vm));

	push(vm, (item_t){.type = VECTOR, .vec = vec});
}

static void op_litscope(rela_vm* vm) {
	map_t *map = vec_pop(vm, &routine(vm)->scopes).map;
	map->smudged = false;
	push(vm, (item_t){.type = MAP, .map = map});
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

static void op_limit(rela_vm* vm) {
	int count = literal_int(vm);
	int old_depth = vec_pop(vm, &routine(vm)->marks).inum;
	int req_depth = old_depth + count;
	if (count >= 0) {
		while (req_depth < vec_size(vm, stack(vm))) pop(vm);
		while (req_depth > vec_size(vm, stack(vm))) push(vm, nil(vm));
	}
}

static void op_break(rela_vm* vm) {
	routine(vm)->ip = vec_cell(vm, &routine(vm)->loops, -1)->inum;
	while (vec_size(vm, &routine(vm)->marks) > vec_cell(vm, &routine(vm)->loops, -2)->inum)
		vec_pop(vm, &routine(vm)->marks);
	while (depth(vm)) pop(vm);
}

static void op_continue(rela_vm* vm) {
	routine(vm)->ip = vec_cell(vm, &routine(vm)->loops, -1)->inum-1;
	while (vec_size(vm, &routine(vm)->marks) > vec_cell(vm, &routine(vm)->loops, -2)->inum)
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

static void op_for(rela_vm* vm) {
	assert(literal(vm).type == VECTOR);

	int var = 0;
	vec_t* vars = literal(vm).vec;
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
				map_set(vm, scope_writing(vm), vec_get(vm, vars, var++), integer(vm, step));

			map_set(vm, scope_writing(vm), vec_get(vm, vars, var++), integer(vm,step));
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
				map_set(vm, scope_writing(vm), vec_get(vm, vars, var++), integer(vm, step));

			map_set(vm, scope_writing(vm), vec_get(vm, vars, var++), vec_get(vm, iter.vec, step));
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
				map_set(vm, scope_writing(vm), vec_get(vm, vars, var++), vec_get(vm, &iter.map->keys, step));

			map_set(vm, scope_writing(vm), vec_get(vm, vars, var++), vec_get(vm, &iter.map->vals, step));
			push(vm, integer(vm, ++step));
		}
	}
	else {
		routine(vm)->ip = quit;
	}
}

static void op_assign(rela_vm* vm) {
	item_t key = pop(vm);
	int index = literal_int(vm);
	item_t val = index < depth(vm) ? *item(vm, -depth(vm)+index): nil(vm);
	map_set(vm, scope_writing(vm), key, val);
}

static void op_find(rela_vm* vm) {
	item_t key = pop(vm);
	vec_t* scopes = &routine(vm)->scopes;

	// this is less than ideal, but should be fixed with the stack locals plan
	for (int i = 0, l = vec_size(vm, scopes); i < l; i++) {
		map_t *scope = vec_get(vm, scopes, -i-1).map;
		if (scope->smudged) continue;

		item_t val = map_get(vm, scope, key);
		if (val.type == NIL) continue;

		push(vm, val);
		return;
	}

	item_t val = nil(vm);

	if (val.type == NIL) {
		val = map_get(vm, vm->scope_global, key);
	}

	if (val.type == NIL) {
		val = map_get(vm, &vm->scope_core, key);
	}

	char tmp[100];
	ensure(vm, val.type != NIL, "unknown name: %s", tmptext(vm, key, tmp, sizeof(tmp)));

	push(vm, val);
}

static void op_set(rela_vm* vm) {
	item_t key = pop(vm);
	item_t dst = pop(vm);

	int index = literal_int(vm);
	item_t val = index < depth(vm) ? *item(vm, index): nil(vm);

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
		char tmpA[100];
		char tmpB[100];
		ensure(vm, 0, "cannot set %s in item %s", tmptext(vm, key, tmpA, sizeof(tmpA)), tmptext(vm, key, tmpB, sizeof(tmpB)));
	}
}

static void op_get(rela_vm* vm) {
	item_t key = pop(vm);
	item_t src = pop(vm);

	if (src.type == VECTOR && key.type == INTEGER) {
		push(vm, vec_get(vm, src.vec, key.inum));
	}
	else
	if (src.type == MAP) {
		push(vm, map_get(vm, src.map, key));
	}
	else {
		char tmpA[100];
		char tmpB[100];
		ensure(vm, 0, "cannot get %s from item %s", tmptext(vm, key, tmpA, sizeof(tmpA)), tmptext(vm, key, tmpB, sizeof(tmpB)));
	}
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
		char tmp[100];
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

	char tmpA[100];
	char tmpB[100];

	char *bs = tmptext(vm, b, tmpA, sizeof(tmpA));
	char *as = tmptext(vm, a, tmpB, sizeof(tmpB));

	push(vm, string(vm, strf(vm, "%s%s", as, bs)));
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
	int ovector[100];
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

	char *buffer = allot(vm, strlen(subject.str)+1);

	for (int i = 0; i < matches; i++) {
		int offset = ovector[2*i];
		int length = ovector[2*i+1] - offset;
		memmove(buffer, subject.str+offset, length);
		buffer[length] = 0;
		push(vm, string(vm, strf(vm, "%s", buffer)));
	}

	discard(vm, buffer);

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
			void *ptr = allot(vm, bytes + 1);

			size_t read = 0;
			for (int i = 0; i < 3; i++) {
				read += fread(ptr + read, 1, bytes - read, file);
				if (read == bytes) break;
			}
			((char*)ptr)[bytes] = 0;

			ensure(vm, read == bytes, "fread failed");

			pop(vm);
			push(vm, string(vm, strintern(vm, ptr)));
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
	ensure(vm, depth(vm) && truth(vm, *item(vm, 0)), "assert");
}

func_t funcs[OPERATIONS] = {
	[OP_NOP] = { .name = "nop", .func = op_nop },
	[OP_PRINT] = { .name = "print", .func = op_print },
	[OP_COROUTINE] = { .name = "coroutine", .func = op_coroutine },
	[OP_RESUME] = { .name = "resume", .func = op_resume },
	[OP_YIELD] = { .name = "yield", .func = op_yield },
	[OP_CALL] = { .name = "call", .func = op_call },
	[OP_RETURN] = { .name = "return", .func = op_return },
	[OP_GLOBAL] = { .name = "global", .func = op_global },
	[OP_LOCAL] = { .name = "local", .func = op_local },
	[OP_LITSTACK] = { .name = "litstack", .func = op_litstack },
	[OP_SCOPE] = { .name = "scope", .func = op_scope },
	[OP_SMUDGE] = { .name = "smudge", .func = op_smudge },
	[OP_UNSCOPE] = { .name = "unscope", .func = op_unscope },
	[OP_LITSCOPE] = { .name = "litscope", .func = op_litscope },
	[OP_MARK] = { .name = "mark", .func = op_mark },
	[OP_LIMIT] = { .name = "limit", .func = op_limit },
	[OP_LOOP] = { .name = "loop", .func = op_loop },
	[OP_UNLOOP] = { .name = "unloop", .func = op_unloop },
	[OP_REPLY] = { .name = "reply", .func = op_reply },
	[OP_BREAK] = { .name = "break", .func = op_break },
	[OP_CONTINUE] = { .name = "continue", .func = op_continue },
	[OP_AND] = { .name = "and", .func = op_and },
	[OP_OR] = { .name = "or", .func = op_or },
	[OP_JMP] = { .name = "jmp", .func = op_jmp },
	[OP_JFALSE] = { .name = "jfalse", .func = op_jfalse },
	[OP_JTRUE] = { .name = "jtrue", .func = op_jtrue },
	[OP_FOR] = { .name = "for", .func = op_for },
	[OP_NIL] = { .name = "nil", .func = op_nil },
	[OP_SHUNT] = { .name = "shunt", .func = op_shunt },
	[OP_SHIFT] = { .name = "shift", .func = op_shift },
	[OP_TRUE] = { .name = "true", .func = op_true },
	[OP_FALSE] = { .name = "false", .func = op_false },
	[OP_LIT] = { .name = "lit", .func = op_lit },
	[OP_ASSIGN] = { .name = "assign", .func = op_assign },
	[OP_FIND] = { .name = "find", .func = op_find },
	[OP_SET] = { .name = "set", .func = op_set },
	[OP_GET] = { .name = "get", .func = op_get },
	[OP_COUNT] = { .name = "count", .func = op_count },
	[OP_DROP] = { .name = "drop", .func = op_drop },
	[OP_ADD] = { .name = "add", .func = op_add },
	[OP_NEG] = { .name = "neg", .func = op_neg },
	[OP_SUB] = { .name = "sub", .func = op_sub },
	[OP_MUL] = { .name = "mul", .func = op_mul },
	[OP_DIV] = { .name = "div", .func = op_div },
	[OP_MOD] = { .name = "mod", .func = op_mod },
	[OP_NOT] = { .name = "not", .func = op_not },
	[OP_EQ] = { .name = "eq", .func = op_eq },
	[OP_NE] = { .name = "ne", .func = op_ne },
	[OP_LT] = { .name = "lt", .func = op_lt },
	[OP_LTE] = { .name = "lte", .func = op_lte },
	[OP_GT] = { .name = "gt", .func = op_gt },
	[OP_GTE] = { .name = "gte", .func = op_gte },
	[OP_CONCAT] = { .name = "concat", .func = op_concat },
	[OP_MATCH] = { .name = "match", .func = op_match },
	[OP_SLURP] = { .name = "slurp", .func = op_slurp },
	[OP_SORT] = { .name = "sort", .func = op_sort },
	[OP_ASSERT] = { .name = "assert", .func = op_assert },
};

static void decompile(rela_vm* vm, code_t* c) {
	char tmp[100];
	char *str = tmptext(vm, c->item, tmp, sizeof(tmp));
	fprintf(stderr, "%04ld  %-10s  %s\n",
		c - vm->code.cells, funcs[c->op].name, str);
	fflush(stderr);
}

static int run(rela_vm* vm) {
	int wtf = setjmp(vm->jmp);
	if (wtf) {
		char tmp[100];
		fprintf(stderr, "%s (", vm->err);
		fprintf(stderr, "ip %d ", routine(vm)->ip);
		fprintf(stderr, "stack %s", tmptext(vm, (item_t){.type = VECTOR, .vec = stack(vm)}, tmp, sizeof(tmp)));
		fprintf(stderr, ")\n");
		reset(vm);
		return wtf;
	}

	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor_alloc(vm)});
	routine(vm)->ip = vm->code.start;

	vm->scope_global = map_alloc(vm);

	for (;;) {
		int ip = routine(vm)->ip++;
		if (ip < 0 || ip >= vm->code.depth) break;
		funcs[vm->code.cells[ip].op].func(vm);

//		char tmp[100];
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

	vm->allocations.start = 0;
	reset(vm);
	free(vm->code.cells);
	free(vm->allocations.cells);
	free(vm);
}

static rela_vm* create(const char* src, size_t memory, void* custom, size_t registrations, const rela_register* registry) {
	rela_vm* vm = calloc(sizeof(rela_vm),1);
	vm->custom = custom;
	vm->memory_limit = memory;

	if (!vm) {
		fprintf(stderr, "calloc\n");
		return NULL;
	}

	if (setjmp(vm->jmp)) {
		fprintf(stderr, "%s\n", vm->err);
		destroy(vm);
		return NULL;
	}

	map_set(vm, &vm->scope_core, string(vm, strintern(vm, "print")), (item_t){.type = SUBROUTINE, .sub = vm->code.depth});
	compile(vm, OP_PRINT);
	compile(vm, OP_RETURN);

	vm->code.start = vm->code.depth;

	for (int i = 0; i < registrations; i++) {
		const rela_register* reg = &registry[i];
		map_set(vm, &vm->scope_core,
			string(vm, strcopy(vm, (char*)reg->name)),
			(item_t){.type = CALLBACK, .cb = reg->func}
		);
	}

	vec_push(vm, &vm->routines, (item_t){.type = COROUTINE, .cor = cor_alloc(vm)});
	op_mark(vm);
	routine(vm)->ip = vm->code.depth;
	push(vm, string(vm, strintern(vm, (char*)src)));

	op_slurp(vm);
	ensure(vm, top(vm).type == STRING, "cannot read %s", src);

	source(vm, pop_type(vm, STRING).str);
	assert(!vec_size(vm, stack(vm)));

	op_unmark(vm);
	vec_pop(vm, &vm->routines);

	vm->allocations.start = vm->allocations.depth;
	vm->strings.start = vm->strings.depth;
	return vm;
}

rela_vm* rela_create(const char* src, size_t memory, void* custom, size_t registrations, const rela_register* registry) {
	return (rela_vm*) create(src, memory, custom, registrations, registry);
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

size_t rela_depth(rela_vm* vm) {
	return depth(vm);
}

void rela_push(rela_vm* vm, rela_item opaque) {
	push(vm, *((item_t*)&opaque));
}

void rela_push_number(rela_vm* vm, double val) {
	push(vm, (item_t){.type = FLOAT, .fnum = val});
}

void rela_push_integer(rela_vm* vm, int64_t val) {
	push(vm, integer(vm, val));
}

void rela_push_string(rela_vm* vm, const char* str) {
	push(vm, string(vm, strcopy(vm, str)));
}

void rela_push_data(rela_vm* vm, void* data) {
	push(vm, (item_t){.type = USERDATA, .data = data});
}

rela_item rela_pop(rela_vm* vm) {
	rela_item opaque;
	*((item_t*)&opaque) = pop(vm);
	return opaque;
}

rela_item rela_get(rela_vm* vm, int pos) {
	rela_item opaque;
	*((item_t*)&opaque) = *item(vm, pos);
	return opaque;
}

bool rela_is_number(rela_vm* vm, rela_item opaque) {
	item_t item = *((item_t*)&opaque);
	return item.type == FLOAT;
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

const char* rela_text(rela_vm* vm, rela_item opaque, char* tmp, size_t size) {
	item_t item = *((item_t*)&opaque);
	return tmptext(vm, item, tmp, size);
}

double rela_number(rela_vm* vm, rela_item opaque) {
	char tmp[100];
	item_t item = *((item_t*)&opaque);
	ensure(vm, item.type == FLOAT, "item is not a float: %s", tmptext(vm, item, tmp, sizeof(tmp)));
	return item.fnum;
}

int64_t rela_integer(rela_vm* vm, rela_item opaque) {
	char tmp[100];
	item_t item = *((item_t*)&opaque);
	ensure(vm, item.type == INTEGER, "item is not an integer: %s", tmptext(vm, item, tmp, sizeof(tmp)));
	return item.inum;
}

const char* rela_string(rela_vm* vm, rela_item opaque) {
	char tmp[100];
	item_t item = *((item_t*)&opaque);
	ensure(vm, item.type == STRING, "item is not a string: %s", tmptext(vm, item, tmp, sizeof(tmp)));
	return item.str;
}

void* rela_data(rela_vm* vm, rela_item opaque) {
	char tmp[100];
	item_t item = *((item_t*)&opaque);
	ensure(vm, item.type == USERDATA, "item is not userdata: %s", tmptext(vm, item, tmp, sizeof(tmp)));
	return item.data;
}
