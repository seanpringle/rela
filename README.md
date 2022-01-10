Rela is a scripting language designed to:

* Be small and self-contained with syntax mostly based on Lua
* Be easy to embed with similar usage patterns to Lua (stack-based, callbacks etc)
* Provide separate vector[] and map{} types, with vectors indexed from 0
* Provide explicit coroutines and nested functions, but not implicit closures
* Use setjmp/longjmp only for emulating exceptions (not for switching coroutines)
* Use regional memory management without ref-counting, cycles or GC pauses
* Provide only an unmanaged USERDATA pointer type (like Lua light user data)
* Not provide persistent global state across script executions

## No persistent state

No persistent state means a Rela VM runs on a clean run-time state every time
it is called, executing the compiled bytecode from the beginning and releasing
all working memory at the end. If saving state is required that needs to be
handled by registering custom callbacks. This moves the problem of managing
state onto the host application and keeps Rela simple and predictable.

## Memory management

https://en.wikipedia.org/wiki/Region-based_memory_management

Executing a script allocates memory across several internal regions, which is
cheap, fast and means internal objects can be removed from the stack and used
by callbacks without risk of untimely garbage collection.

There is a small mark-and-sweep garbage collector but the VM guarantees never
to implicitly trigger a stop-the-world collection during execution.

Instead, memory reclaimation only occurs when:

* rela_create() completes and compile-time memory is reset
* rela_run() completes and the run-time memory is reset
* A callback explicitly calls rela_collect()
* A script explicitly calls collect()

