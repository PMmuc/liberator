patch 0001 fixes the problem, that memcpy iterates over a llvm StructType but forgets that
SVF uses flattened types. Therefore this leads to an error if there is something like 
struct A {int x, int y;} struct B{struct A a; int z;} because for iterating B index 1 in llvm will point to z,
while in SVF point to y.
patch 0002
patch 0003 is for uriparser to work correctly. This asserts if this is not representable as a double.
patch 0004 lets annotated extapi.c functions (e.g. ALLOC_HEAP_RET) replace app definitions, needed for allocator wrappers like _cmsMalloc.
