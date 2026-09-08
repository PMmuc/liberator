// Exercises the indirect-call path that replaced the AParm case of
// compute_local_effect. In the bottom-up analysis an ActualParmVFGNode is
// caught as a call boundary and routed through merge_summary/callee_targets.
// For indirect calls callee_targets consults myCallEdgeMap_inst, which is
// populated by GlobalStruct's signature matching for calls whose points-to
// set is empty (see GlobalStruct.cpp / unresolved_calls). That is the exact
// scenario handleActualParam used to cover.
//
// Setup:
//   - write_sink's address is taken (stored in `registry`), so the signature
//     matcher knows a void(int*) target exists.
//   - dispatch calls through `fp`, obtained from the external, unresolvable
//     get_sink(). Points-to cannot resolve fp, so the call is type-matched to
//     write_sink and recorded in myCallEdgeMap_inst.
//
// With consider_indirect_calls the callee's write must be merged into buf's
// summary, so the parameter summary must contain a write access.

typedef void (*sink_t)(int *);

void write_sink(int *p) { *p = 42; }

// Address-taken so write_sink is a signature-match candidate.
sink_t registry = &write_sink;

// External and unresolvable: the returned pointer has an empty points-to set.
extern sink_t get_sink(void);

void dispatch(int *buf) {
  sink_t fp = get_sink();
  fp(buf); // unresolved indirect call -> type-matched to write_sink
}

int main(void) {
  int v = 0;
  dispatch(&v);
  return v;
}
