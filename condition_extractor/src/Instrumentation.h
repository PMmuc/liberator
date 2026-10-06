#pragma once

namespace liberator {

struct instr_t {
  uint64_t pops = 0;       // paths taken from the worklist
  uint64_t dedup_hits = 0; // pops rejected by visited
  size_t max_worklist = 0; // max number in the worklist
  size_t max_visited = 0;  // max number of nodes in visited
  size_t depth_cuts =
      0; // how many paths were cut (not tracked because limit exceeded)
};

struct di_chain_instr_t {
  size_t ok = 0;           // next_di_field returned a type
  size_t fresh_break = 0;  // had DIType on entry, but than lost it.
  size_t poisoned = 0;     // already null on entry - break was earlier
  size_t formal_no_di = 0; // summarize_formal inits a path with no DIType
  size_t formal_with_di = 0;
  size_t compose_null_di = 0; // merge_access_type copied a null DI from suffix
  size_t gep_no_di = 0; // the gep handler has no di info on the path anymore.
  // Which pase feeds handleGep a path that already lost its DIType.
  size_t gep_bu_null = 0, gep_bu_ok = 0; // bottom-up
  size_t gep_void_recovered = 0;         // recovered void* type in handleGep
  size_t gep_untyped_skip = 0;           //
};

/**
 * Instrumenting merge_summary. Checking if parameter is passed as addr_of.
 * Checking if the suffix is empty, -1 (wildcard), [0] (zero), "other"
 * otherwise.
 */
struct addr_instr_t {
  size_t cs_addr_of = 0;   // actual parameter is an result of GEP, therefore an
                           // address of operator
  size_t cs_plain = 0;     // actual parameter is an not result of a GEP.
  size_t suf_empty = 0;    // suffix field vector is empty
  size_t suf_wildcard = 0; // suffix has only [-1]
  size_t suf_zero = 0;     // suffix has only [0]
  size_t suf_other = 0;
  size_t composed = 0;
  size_t rejected = 0;
  void dump(llvm::raw_ostream &os) const;
};

/**
 * Instrumenting merge_access_type. Deciding whether a callee path is merged
 * with the caller path. Only printed when -metrics is passed.
 */
struct compose_instr_t {
  size_t attempts = 0;       // compositions with a non-empty suffix path
  size_t di_match = 0;       // same di_types -> continue with path
  size_t di_reject = 0;      // different di_types -> stop analysing path
  size_t di_void = 0;        // one side is void*
  size_t di_undecidable = 0; // DWARF type could not be determined from prefix
                             // or suffix -> stop analysing path
  size_t di_absent = 0;      // prefix or suffix DIType is null
  size_t di_unnamed = 0;     // anonymous record without a typedef name
  size_t void_dropped = 0;   // void composition dropped by current policy

  static compose_instr_t &instance();

  void dump(llvm::raw_ostream &os) const;
};

struct type_instr_t {
  size_t cleared_is_malloc_sz =
      0; // number of is malloc size flag removed for pointer types
};

extern type_instr_t type_instr;
extern compose_instr_t cinstr;
extern addr_instr_t addr_instr;
extern instr_t instr;
extern di_chain_instr_t di_instr;

} // namespace liberator
