// Exercises the third fallback in ConditionExtractor.cpp,
// inferTypeFromForwardUses. The CMake rule compiles this file with -O1 and
// WITHOUT -g, which is what makes the fallback chain land here:
//
//   1. SVF inferObjType  → opaque ptr (function has no callers in the bc).
//   2. Argument attrs    → nullptr   (no byval / sret / elementtype).
//   3. DWARF resolver    → skipped   (no DISubprogram, file built w/o -g).
//   4. Forward use scan  → fires, and reads %struct.ForwardUseStruct off the
//                          first GEP whose pointer operand is the param.
//
// At -O0 clang spills the parameter to an alloca, so the only direct user
// of %p would be `store ptr %p, ptr %p.addr` (with %p as the value operand,
// not the pointer operand) — which the scan ignores. -O1 elides that spill
// and the GEP is on %p directly.

struct ForwardUseStruct {
  int a;
  int b;
};

void test_forward_use(struct ForwardUseStruct *p) { p->b = 7; }
