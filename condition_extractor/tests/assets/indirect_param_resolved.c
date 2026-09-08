// Companion to indirect_param.c for the OTHER indirect-resolution source.
//
// Here global_fp is initialized to exactly &write_sink, so the points-to
// analysis resolves fp(buf) precisely to write_sink and records it as an
// indirect call-graph edge. Such calls are NOT in myCallEdgeMap_inst (which
// GlobalStruct fills only for calls with an empty points-to set), so before
// the callee_targets fix their callee effect was silently dropped.
// callee_targets must now pick the target up via getIndCSCallees and merge
// write_sink's write into buf's summary.

typedef void (*sink_t)(int *);

void write_sink(int *p) { *p = 42; }

sink_t global_fp = &write_sink;

void dispatch(int *buf) {
  sink_t fp = global_fp;
  fp(buf); // precisely resolved by points-to to write_sink
}

int main(void) {
  int v = 0;
  dispatch(&v);
  return v;
}
