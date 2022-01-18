Rela is a scripting language designed to:

* Be small and self-contained with syntax mostly based on Lua
* Be easy to embed with similar usage patterns to Lua (stack-based, callbacks etc)
* Provide separate vector[] and map{} types, with vectors indexed from 0
* Treat all variables as implicitly local, unless explicitly declared global
* Provide explicit coroutines and nested functions, but not closures or metatables
* Use setjmp/longjmp for emulating exceptions, but not for switching coroutines
* Use regional memory management without ref-counting, cycles or collection pauses
* Use PCRE regex syntax

## Basic workflow

* Create a Rela instance with `rela_create()`
  * Pass in a script and a table of C callbacks
  * Source code is compiled to byte code

* Call `rela_run()` repeatedly
  * Byte code executes each time on a fresh run-time state
  * Persistence and global state done via callbacks as needed
  * Run-time memory regions are released

* Call `rela_destroy()`

```c
void hello(rela_vm* rela) {
  rela_push(rela, rela_make_string(rela, "hello world"));
}

rela_register registry[] = {
  {"hello", hello},
};

bool run(const char* source) {
  int ok = false;

  rela_vm* rela = rela_create(source, 1, registry, NULL);

  if (rela) {
    ok = rela_run(rela) == 0;
    rela_destroy(rela);
  }

  return ok;
}
```

## Memory management

https://en.wikipedia.org/wiki/Region-based_memory_management

Executing a script allocates memory from regions which is cheap, fast and means
internal objects can be removed from the stack and used by callbacks without
risk of untimely garbage collection.

There is a simple mark-and-sweep stop-the-world garbage collector that the VM
guarantees never to implicitly trigger during execution. Instead, memory
reclamation occurs when:

* `rela_create()` completes and compile-time regions are reset
* `rela_run()` completes and run-time regions are reset
* A callback explicitly calls `rela_collect()`
* A script explicitly calls `lib.collect()`

The host application can decide which approach is best. The best GC for an
embedded scripting language is the one you figure out how to avoid using at all!

## Keywords

```
if else end while for in break continue return function nil true false or
global lib print
```

The `print` keyword can be overriden.

## Standard library

The `lib` namespace holds other functions:

```
assert collect coroutine resume yield sort type sin cos tan asin acos atan sinh
cosh tanh ceil floor sqrt abs atan2 log log10 pow min max
```

Any `lib` function can be assigned to a local variable for brevity and
performance.

```lua
min = lib.min
print(min(2,1,3))
```

```
1
```

### vector

```lua
stuff = [1,3,5]
print(stuff)
stuff[#stuff] = 7
print(stuff)
print(stuff[1])
```

```
[1, 3, 5]
[1, 3, 5, 7]
```

```lua
function test() return 1, 2, 3 end
print(test(), [test()])
```

```
1       2       3       [1, 2, 3]
```

```lua
alpha = [1, 2, 3]
beta = [alpha..., 4, 5, 6]
print(beta)
```

```
[1, 2, 3, 4, 5, 6]
```

### map

```lua
stuff = { apples = 1, oranges = 2 }
print(stuff, stuff.apples, stuff["oranges"])
```

```
{apples = 1, oranges = 2}       1       2
```

### if

```lua
if 10 > 5 print("yes") else print("no") end
```

```
yes
```

```lua
print(if 10 > 5 "yes" else "no" end)
```

```
yes
```

### for

```lua
for i in 3 print(i) end
```

```
0
1
2
```

```lua
for cell in [1,3,5] print(cell) end
```

```
1
3
5
```

```lua
stuff = [1,3,5]
for i in #stuff print("$i : $(cells[i])") end
```

```
0 : 1
1 : 3
2 : 5
```

```lua
for key,val in { apples = 1, oranges = 2 } print("$key : $val") end
```

```
apples : 1
oranges : 2
```

## PCRE

The match operator `~` is available if Rela is built with PCRE, returning the
full match and any groups:

```lua
print("abcd" ~ "(ab)(c)")
```

```
abc     ab      c
```

