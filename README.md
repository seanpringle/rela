Rela is a scripting language designed to:

* Be small and self-contained with syntax mostly based on Lua
* Be easy to embed with similar usage patterns to Lua (stack-based, callbacks etc)
* Provide separate vector[] and map{} types, with vectors indexed from 0
* Provide explicit coroutines and nested functions, but not implicit closures
* Use setjmp/longjmp only for emulating exceptions (not for switching coroutines)
* Use regional memory management without ref-counting, cycles or GC pauses
* Provide only an unmanaged USERDATA pointer type (like Lua light user data)
* Not provide persistent global state across script executions

No persistent state means a VM starts from a clean slate every time it is
called, executing its compiled bytecode from the start each time and releasing
all runtime memory at the end. If saving state is required it needs to be
handled by registering custom callbacks. This moves the problem of managing
state onto the application and keeps the VM simple and predictable.
Examples for handling state:

* Callback to query current app state each execution
* Callbacks to read and write maps as records in SQLite
* Callbacks to serialize/unserialize custom data
* Callbacks to use the built-in USERDATA type
