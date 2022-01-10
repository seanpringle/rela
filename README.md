Rela is a scripting language designed to:

* Be small and self-contained with syntax mostly based on Lua
* Be easy to embed with similar usage patterns to Lua (stack-based, callbacks etc)
* Provide separate vector[] and map{} types, with vectors indexed from 0
* Provide explicit coroutines and nested functions, but not implicit closures
* Use setjmp/longjmp only for emulating exceptions (not for switching coroutines)
* Use regional memory management without ref-counting, cycles or GC pauses
* Provide only an unmanaged USERDATA pointer type (like Lua light user data)
* Not provide persistent global state across script executions
* Use PCRE regex syntax

## No persistent state

No persistent state means a Rela VM runs on a clean run-time state each time
it is called, executing its compiled bytecode from the start and releasing
all working memory at the end. If saving state is required it needs to be
handled by registering custom callbacks. This moves the problem of managing
state onto the host application and keeps Rela simple and predictable.

## Memory management

https://en.wikipedia.org/wiki/Region-based_memory_management

Executing a script allocates memory across several internal regions which is
cheap, fast and means internal objects can be removed from the stack and used
by callbacks without risk of untimely garbage collection.

There is a mark-and-sweep stop-the-world garbage collector that the VM
guarantees never to implicitly trigger during execution. Instead, memory
reclaimation occurs when:

* `rela_create()` completes and compile-time memory is reset
* `rela_run()` completes and the run-time memory is reset
* A callback explicitly calls `rela_collect()`
* A script explicitly calls `lib.collect()`

The host application can decide which approach is best for each use case.

## Keywords

```
if else end while for break continue return function nil true false and or
global lib print
```

The `print` keyword can be overriden.

## Operators

```
and or == != >= > <= < ~ + - * / %
```

## Standard library

The `lib` namespace holds other functions:

```
assert collect coroutine resume yield sort type sin cos tan asin acos atan sinh
cosh tanh ceil floor sqrt abs atan2 log log10 pow min max
```

Any `lib` function can be assigned to a local variable for brevity and
performance.

```
min = lib.min
print(min(2,1,3))
```

## PCRE

The match operator `~` is available if Rela is built with PCRE, returning the
full match and any groups:

```lua
> print("abcd" ~ "(ab)(c)")
abc     ab      c
```

