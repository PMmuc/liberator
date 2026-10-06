#include "AccessTracker.h"
#include "AccessType.h"
#include "AccessTypeHandler.h"
#include "AccessTypeIO.h"
#include "DebugInfoParser.hpp"
#include "Instrumentation.h"
#include "IntraAnalysis.hpp"
#include <Graphs/CallGraph.h>
#include <Graphs/ICFGNode.h>
#include <Graphs/SVFGNode.h>
#include <Graphs/VFGNode.h>
#include <SVF-LLVM/LLVMModule.h>
#include <SVF-LLVM/LLVMUtil.h>
#include <SVFIR/SVFIR.h>
#include <SVFIR/SVFVariables.h>
#include <Util/Casting.h>
#include <Util/GeneralType.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <optional>
#include <unordered_map>

#include "Config.h"
#include "ValueMetadata.hpp"

using namespace SVF;

// How many nodes a maximal considered in the backward_slice.
static constexpr unsigned int MAX_SLICE_NODES = 10000;
// How many fixpoint iterations in process_scc before returning
static constexpr unsigned int MAX_FIXPOINT_ITERATIONS = 3;
// How many depth fields are tracked before returning.
static constexpr unsigned int MAX_FIELD_DEPTH = 6;
// How many indirect callsites are merged maximally for function pointers.
static constexpr size_t MAX_INDIRECT_TARGETS = 4;
// How many maximal contexts are saved before returning
// Limits the number for recursive functions.
static constexpr unsigned int MAX_LEN_CTX_DEPTH = 8;
static constexpr size_t MAX_BAD_AT_REPORTS = 200;
static constexpr size_t MAX_REPORTS_PER_FORMAL = 5;

// type matching in have_compatible_types
// e_reject - types do not match
// e_match - types match
// e_is_void - types are void -> look up policy
enum class matching_e { e_reject, e_match, e_is_void };
// accept - void types are accepted and traced.
// old_liberator - void types are all rejected.
// cut - void types are only rejected for paths with more than 1 field
enum class void_policy_e { accept, old_liberator, cut };

namespace {
// get void policy
void_policy_e get_void_policy() {
  string_view policy = config_t::instance()->void_policy;

  if (policy == "cut")
    return void_policy_e::cut;
  if (policy == "old")
    return void_policy_e::old_liberator;
  return void_policy_e::accept;
}

// malloc 8-byte header, rounded up to 16
size_t chunk(size_t n) {
  return n == 0 ? 0 : std::max<size_t>(32, (n + 8 + 15) & ~size_t(15));
}

//
template <typename C> size_t tree_bytes(const C &c) {
  return c.size() * chunk(sizeof(std::_Rb_tree_node<typename C::value_type>));
}

template <typename T> size_t vec_bytes(const std::vector<T> &v) {
  return v.capacity() * sizeof(T);
}

// we assume the hash map is a next pointer + the value type for each element in
// the map (for resolving collisions) plus the bucket array. (containing the
// first element)
template <typename M> size_t map_bytes(const M &m) {
  return m.size() * chunk(sizeof(void *) + sizeof(typename M::value_type)) +
         chunk(m.bucket_count() * sizeof(void *));
}

size_t at_heap(const liberator::AccessType &at) {
  return tree_bytes(at.getICFGNodes()) + tree_bytes(at.get_visited_types()) +
         vec_bytes(at.get_fields()) + vec_bytes(at.get_parent_fields());
}

bool returns_pointer(const FunObjVar *f) {
  auto llvm_fun = dyn_cast_or_null<Function>(
      LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(f));

  return llvm_fun && llvm_fun->getReturnType()->isPointerTy();
}

static bool is_formal_pointer_type(const VFGNode *n) {
  auto *fp = SVFUtil::dyn_cast<FormalParmVFGNode>(n);
  if (!fp)
    return false;
  const auto v =
      LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(fp->getParam());
  return v && v->getType()->isPointerTy();
}

/**
 * @returns a pair of (id, depth), where the id is a unique ttype string
 * identifing the DI type and depth is the number of pointers or indirection
 * (for c++ references). Also if the name is a typedef but the underlying Object
 * is a struct return without a name return the typedef name.
 * For definitions such as typedef struct {...} S1;
 */
static pair<string, unsigned> di_shape_id(llvm::DIType *di) {
  unsigned depth = 0;
  llvm::StringRef typedef_name;
  llvm::DIType *type = di;

  while (auto deriv = llvm::dyn_cast_or_null<llvm::DIDerivedType>(type)) {
    auto tag = deriv->getTag();
    if (liberator::is_indirection_tag(tag)) {
      ++depth;
      typedef_name = "";
    } else if (tag == llvm::dwarf::DW_TAG_typedef) {
      typedef_name = deriv->getName();
    } else if (tag != llvm::dwarf::DW_TAG_const_type &&
               tag != llvm::dwarf::DW_TAG_volatile_type &&
               tag != dwarf::DW_TAG_restrict_type &&
               llvm::dwarf::DW_TAG_atomic_type != tag) {
      break;
    }
    type = deriv->getBaseType();
  }

  if (!type) {
    return {"VO", depth++};
  }
  if (auto comp = llvm::dyn_cast_or_null<llvm::DICompositeType>(type)) {
    auto tag = comp->getTag();
    if (tag == llvm::dwarf::DW_TAG_structure_type ||
        tag == llvm::dwarf::DW_TAG_class_type ||
        tag == llvm::dwarf::DW_TAG_union_type) {
      return {(comp->getName().empty() ? typedef_name : comp->getName()).str(),
              depth};
    }
  }

  if (auto *basic = llvm::dyn_cast_or_null<llvm::DIBasicType>(type)) {
    // For floats.
    if (basic->getEncoding() == llvm::dwarf::DW_ATE_float)
      return {"F" + std::to_string(basic->getSizeInBits()), depth};
    // For integers.
    return {"I" + std::to_string(basic->getSizeInBits()), depth};
  }

  if (llvm::isa_and_nonnull<llvm::DISubroutineType>(type))
    return {"FN", depth};

  return {};
}

/**
 * type matcher: Tries to match di types of callee and caller.
 */
matching_e have_compatible_types(llvm::DIType *prefix_di,
                                 llvm::DIType *suffix_di,
                                 liberator::compose_instr_t &ci,
                                 unsigned prefix_extra_depth) {
  if (!prefix_di || !suffix_di)
    ci.di_absent++;

  auto prefix = di_shape_id(prefix_di);
  auto suffix = di_shape_id(suffix_di);

  prefix.second += prefix_extra_depth;
  if (prefix.first == "VO" || suffix.first == "VO") {
    ci.di_void++;
    return matching_e::e_is_void;
  }

  if (prefix.first.empty() || suffix.first.empty()) {
    ci.di_unnamed++;
    ci.di_undecidable++;
    return matching_e::e_reject;
  }

  if (prefix == suffix) {
    ci.di_match++;
    return matching_e::e_match;
  }

  ci.di_reject++;
  return matching_e::e_reject;
}
/**
 * Returns the DIType of the parameter if a DILocalVariable record exists for
 * the alloca instruction. This function relies on debug information, i.e. that
 * the target library is compiled with debug information (-g).
 * @param the actual param PAG Node
 * @return DIType of the actual parameter
 */
static llvm::DIType *actual_param_di_type(const SVF::ValVar *param) {
  if (!param)
    return nullptr;

  const Value *v = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(param);
  if (llvm::isa<Argument>(v)) {
    return liberator::restore_param_di_type(v);
  }

  const llvm::Value *base = v;
  if (const auto *ld = llvm::dyn_cast<llvm::LoadInst>(v))
    base = llvm::getUnderlyingObject(ld->getPointerOperand());

  const auto *alloc = dyn_cast<AllocaInst>(base);

  if (!alloc)
    return nullptr;

  for (auto *rec : llvm::findDVRDeclares(const_cast<AllocaInst *>(alloc))) {
    if (const llvm::DILocalVariable *lv = rec->getVariable())
      return lv->getType();
  }

  return nullptr;
}

std::string fmt_path(const vector<int> &fields) {
  if (fields.empty())
    return ".";

  std::string s;
  for (auto f : fields) {
    s += ".";
    s += (f == -1) ? std::string("*") : std::to_string(f);
  }

  return s;
}

/**
 * Returns if param is defined as a result of a GEP instruction.
 * @param param
 */
static bool is_addr_of(const SVF::ValVar *param) {
  if (!param)
    return false;

  const llvm::Value *v = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(param);

  if (!v)
    return false;
  if (llvm::isa<llvm::GetElementPtrInst>(v))
    return true;
  if (const auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(v))
    return ce->getOpcode() == llvm::Instruction::GetElementPtr;
  return false;
}

/**
 * @retrun the function corresponding to a formal parameter
 */
const FunObjVar *get_function(const SVFG &svfg, SVF::NodeID param) {
  SVF::NodeID fid = param;
  auto *pag = SVFIR::getPAG();
  if (auto *pNode = pag->getGNode(param)) {
    if (auto *valVar = SVFUtil::dyn_cast<SVF::ValVar>(pNode)) {
      if (svfg.hasDefSVFGNode(valVar)) {
        fid = svfg.getDefSVFGNode(valVar)->getId();
      }
    }
  }
  const VFGNode *n = svfg.getGNode(fid);
  if (auto fn = dyn_cast<FormalParmVFGNode>(n)) {
    return fn->getFun();
  }
  return nullptr;
}

size_t rss_mib() {
  std::ifstream f("/proc/self/statm");
  std::size_t total = 0, resident = 0;
  if (!f)
    return 0;
  f >> total >> resident;
  return resident * static_cast<size_t>(sysconf(_SC_PAGESIZE)) / (1024 * 1024);
}
std::set<const VFGNode *> getDefinitionSetCtx(const VFGNode *n,
                                              liberator::Path *path_in) {

  std::set<const VFGNode *> definitions;

  std::set<const VFGNode *> visited;
  std::vector<const VFGNode *> worklist;

  liberator::Path path = *path_in;

  // outs() << "n: " << n->toString() << "\n";

  worklist.push_back(n);
  while (!worklist.empty()) {
    auto n = worklist.back();
    worklist.pop_back();
    if (visited.find(n) != visited.end())
      continue;
    int n_parents = 0;
    for (auto in : n->getInEdges()) {
      if (auto src = SVFUtil::dyn_cast<ActualParmVFGNode>(in->getSrcNode())) {
        auto cs = src->getCallSite();
        if (path.isCorrect(cs)) {
          path.popFrame();
          // outs() << "This is correct!!!\n";
        } else
          continue;
      }
      // outs() << in->toString() << "\n";

      auto pn = in->getSrcNode();
      worklist.push_back(pn);
      n_parents++;
    }
    // Maybe select some classes, e.g., alloca, param
    if (n_parents == 0)
      definitions.insert(n);
    visited.insert(n);
  }

  return definitions;
}
std::set<const VFGNode *> getDefinitionSet(const VFGNode *n) {
  std::set<const VFGNode *> definitions;
  std::set<const VFGNode *> visited;
  std::vector<const VFGNode *> worklist;

  worklist.push_back(n);
  while (!worklist.empty()) {
    auto n = worklist.back();
    worklist.pop_back();
    if (visited.find(n) != visited.end())
      continue;
    int n_parents = 0;
    for (auto in : n->getInEdges()) {
      auto pn = in->getSrcNode();
      worklist.push_back(pn);
      n_parents++;
    }
    // Maybe select some classes, e.g., alloca, param
    if (n_parents == 0)
      definitions.insert(n);
    visited.insert(n);
  }

  return definitions;
}
/**
 * This function tries to retrieve the source node from the value flow for
 * the variable v.
 *
 * The idea is given that in -O0 every load is stored on the stack,
 * everytime the index variable of the array is loaded, another SSA
 * register is used. For example in the icmp instruction register %1 will be
 * used in the comparision p1 < len1. And later in the array indexing arr[p1],
 * register %2 will be used. They mean the same variable so we have to map them
 * to it. We do that by first removing any zext/sext/trunc casts, and arithmetic
 * with constant of the index variable. On the stripped variable we
 * getUnderlyingObject to return the alloca instruction as each use shares the
 * same stack variable.
 * Note that for struct objects this can lead to false positives because alloca
 * returns the struct object.
 *
 * @return the source node of v
 */
static const llvm::Value *get_cannonical_index(const llvm::Value *v) {
  // We need to find the LOAD instruction that reads our value before
  // we can call getUnderlyingObject otherwise the value stays unchanged.
  while (true) {
    // remove zext/sext/trunc
    if (auto *c = dyn_cast<llvm::CastInst>(v)) {
      v = c->getOperand(0);
      continue;
    }
    if (auto b = dyn_cast<BinaryOperator>(v)) {
      if (isa<Constant>(b->getOperand(1))) {
        v = b->getOperand(0);
        continue;
      }
      if (isa<Constant>(b->getOperand(0))) {
        v = b->getOperand(1);
        continue;
      }
    }
    break;
  }

  if (auto *ld = SVFUtil::dyn_cast<llvm::LoadInst>(v))
    return llvm::getUnderlyingObject(ld->getPointerOperand());

  return v;
}

/**
 * @param returns true if n walks by target.
 */
bool backward_flow(const VFGNode *n, const VFGNode *target, const SVFG &svfg) {
  std::set<const VFGNode *> visited;
  vector<const VFGNode *> worklist{n};
  while (!worklist.empty()) {
    const VFGNode *curr = worklist.back();
    worklist.pop_back();

    if (curr == target)
      return true;

    if (!visited.insert(curr).second)
      continue;

    for (auto e : curr->getInEdges()) {
      const VFGNode *v = e->getSrcNode();

      // do not track the pointer operand. we are only interested in the
      // indirect value operand that gets loaded
      if (SVFUtil::isa<LoadVFGNode>(curr) && e->isDirectVFGEdge())
        continue;

      // Store has edges for value and address; keep only the value
      if (auto store = SVFUtil::dyn_cast<StoreVFGNode>(curr)) {
        if (e->isDirectVFGEdge()) {
          auto val = SVFUtil::dyn_cast<ValVar>(store->getSrcNode());
          if (!val || !svfg.hasDefSVFGNode(val) ||
              svfg.getDefSVFGNode(val) != v)
            continue;
        }
      }
      worklist.push_back(v);
    }
  }
  return false;
}

/**
 * Walks both sets and if one node is equal returns true
 * @param s1 - first node
 * @param s2 - second node
 */
static bool intersects(const std::set<const VFGNode *> &s1,
                       const std::set<const VFGNode *> &s2) {
  auto it1 = s1.begin(), it2 = s2.begin();
  while (it1 != s1.end() && it2 != s2.end()) {
    // because both are sorted by the same key, we can just compare them.
    if (*it1 < *it2)
      ++it1;
    else if (*it1 > *it2)
      ++it2;
    else
      return true;
  }
  return false;
}
} // namespace

namespace liberator {

static unsigned int addr_of_samples = 0;
compose_instr_t cinstr;
addr_instr_t addr_instr;
instr_t instr;
di_chain_instr_t di_instr;
type_instr_t type_instr;

std::set<const FunObjVar *> ind_collected_function_calls;

compose_instr_t &compose_instr_t::instance() {
  static compose_instr_t instance;
  return instance;
}

struct visit_key_t {
  const VFGNode *node;
  const std::string *di;
  AccessType::kind_e kind;
  vector<int> fields;
  bool operator<(const visit_key_t &o) const {
    return std::tie(node, fields, kind, di) <
           std::tie(o.node, o.fields, o.kind, o.di);
  }
};

void compose_instr_t::dump(llvm::raw_ostream &os) const {
  os << "[COMPOSE] attempts=" << attempts << " di_match=" << di_match
     << " di_reject=" << di_reject << " di_void=" << di_void
     << " undecidable=" << di_undecidable << " absent=" << di_absent
     << " unnamed=" << di_unnamed << ")\n";
}

SVF::NodeID access_tracker_t::formal_id_of(const FunObjVar *f, int n) {
  if (!f || n < 0 || (size_t)n >= f->arg_size())
    return 0;
  auto *arg_val = f->getArg(n);
  if (!arg_val)
    return 0;
  auto *vv = SVFUtil::dyn_cast<ValVar>(arg_val);
  if (!vv || !svfg->hasDefSVFGNode(vv))
    return 0;
  return svfg->getDefSVFGNode(vv)->getId();
}

llvm::DIType *access_tracker_t::ret_di_type(const FunObjVar *f) {
  auto llvm_fun = dyn_cast_or_null<llvm::Function>(
      LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(f));

  if (!llvm_fun)
    return nullptr;

  return restore_ret_di_type(llvm_fun);
}

void access_tracker_t::merge_summary(VFGNode *succ, const CallICFGNode *cs,
                                     Path &p, func_summary_t &summ,
                                     vector<Path> &worklist) {
  // If the callsite is an indirect call we don't merge caller and callee.
  if (cs->isIndirectCall() && !config_t::instance()->consider_indirect_calls) {
    return;
  }

  // 1. First we need to connect the actual to the formal parameter
  //    There are two ways to do this: using OutEdges or getting
  //    ValVar for the actualParm and get the position of the passed argument in
  //    the callsite. With the position we can determine the formal parameter in
  //    the callee.
  //    -> We use the position approach because, also we need the position for
  //    the handlerDispatcher
  const ValVar *actual_param = nullptr;
  llvm::DIType *actual_param_di = nullptr;
  bool addr_of = false;

  if (auto actual_param_node = SVFUtil::dyn_cast<ActualParmVFGNode>(succ)) {
    actual_param = actual_param_node->getParam(); // same as getValue()
    // returns if actual parameter is an addr of operator. (Just used for
    // Instrumentation)
    addr_of = ::is_addr_of(actual_param);
    // returns the actual parameter di type for have_compatible_types
    actual_param_di = actual_param_di_type(actual_param);

    if (addr_of)
      addr_instr.cs_addr_of++;
    else
      addr_instr.cs_plain++;
  }

  if (!actual_param)
    return;

  int n;
  for (n = 0; n < cs->getNumArgOperands(); ++n) {
    if (actual_param == cs->getActualParms()[n]) {
      break;
    }
  }

  const auto targets = callee_targets(cs);

  const bool too_many_targets =
      cs->isIndirectCall() && targets.size() > MAX_INDIRECT_TARGETS;

  for (auto callee : callee_targets(cs)) {
    auto callee_formal = formal_id_of(callee, n);
    SUMM_LOG("Possible Called function: {} for id: {}\n", callee->getName(),
             callee_formal);

    if (cs->isIndirectCall()) {
      ind_collected_function_calls.insert(callee);
    }

    // IMPORTANT: always add the results of handlerDispatcher before the check
    // for callee_formal, because for external functions like malloc, stdlib
    // there does not exists a summary.
    std::string callee_name = callee->getName();
    if (hasHandlerDispatcher(&summ.effects, callee_name, cs, cs, n, C_PARAM)) {
      handlerDispatcher(summ.effects, callee_name, cs, cs, n,
                        p.get_access_type(), C_PARAM, &p);
    }

    if (callee_formal == 0)
      continue;

    auto it = summaries.find(callee_formal);
    if (it == summaries.end())
      continue;

    const func_summary_t &suffix_sum = it->second;

    llvm::DIType *callee_base_di = formal_di_type(callee_formal);

    for (const AccessType &suffix_at :
         suffix_sum.effects.get_access_type_set()) {
      // we keep access types too base parameter but skip subfields
      if (too_many_targets && suffix_at.get_num_fields() > 0)
        continue;
      if (addr_of) {
        const auto &sf = suffix_at.get_fields();
        if (sf.empty())
          addr_instr.suf_empty++;
        else if (sf.front() == -1)
          addr_instr.suf_wildcard++;
        else if (sf.front() == 0)
          addr_instr.suf_zero++;
        else
          addr_instr.suf_other++;
      }
      auto composed_opt =
          merge_access_type(p.get_access_type(), suffix_at, callee_base_di,
                            addr_of, actual_param_di);

      if (addr_of) {
        if (composed_opt)
          addr_instr.composed++;
        else
          addr_instr.rejected++;

        if (addr_of_samples < 25) {
          addr_of_samples++;
          auto prefix_type =
              di_aggregate_name(p.get_access_type().get_di_type());
          auto suffix_type = di_aggregate_name(callee_base_di);

          llvm::outs() << "[ADDROF] " << (composed_opt ? "OK" : "REJ")
                       << " callee=" << callee->getName() << " prefix="
                       << fmt_path(p.get_access_type().get_fields()) << "("
                       << (prefix_type.first.empty() ? "<unnamed di type>"
                                                     : prefix_type.first)
                       << "/" << prefix_type.second << ")"
                       << " suffix=("
                       << (suffix_type.first.empty() ? "<unnamed di type>"
                                                     : suffix_type.first)
                       << "/" << suffix_type.second << ")"
                       << " suffix=" << fmt_path(suffix_at.get_fields())
                       << " kind=" << to_string(suffix_at.get_kind()) << "\n";
          llvm::outs().flush();
        }
      }

      if (composed_opt) {
        summ.effects.get_access_type_set().insert_nodes(
            composed_opt.value(), suffix_at.getICFGNodes());
      }
    }
    const bool passed_whole_object =
        p.get_access_type().get_num_fields() == 0 && !addr_of;

    if (passed_whole_object && !too_many_targets) {
      if (suffix_sum.effects.isArray())
        summ.effects.setIsArray(true);
      if (suffix_sum.effects.isFilePath())
        summ.effects.setIsFilePath(true);
      if (suffix_sum.effects.isMallocSize())
        summ.effects.setMallocSize(true);
      for (llvm::Value *idx : suffix_sum.effects.getIndexes())
        summ.effects.addIndex(idx);

      // if it is a self recursive function summ and suffix_sum alias.
      // therefore if summ.effects.add_len_source pushes and reallocates
      // the vector the fp points to unallocated memory leading to a crash.
      // We don't need to update the len source for recursive functions anyway
      // so just skip it.
      if (&summ != &suffix_sum)
        for (auto &fp : suffix_sum.effects.get_len_source()) {
          // the path from get_len_source gets copied and appended with the
          // current callsite.
          Path ctx = fp.second;
          if (ctx.getStackSize() >= MAX_LEN_CTX_DEPTH)
            continue;
          // we save callsites only if we have get_len_sources.
          ctx.push_callsite(cs);
          summ.effects.add_len_source(fp.first, &ctx);
        }
    }

    for (const exit_state_t &exit : suffix_sum.exits) {
      // skip adding access types if too many targets.
      // but keep parameter subfields.
      if (too_many_targets && exit.at.get_num_fields() > 0)
        continue;
      const SVFGNode *poss_ret = get_resume_node(exit.formal_ret, cs);

      if (!poss_ret)
        continue;

      auto composed_opt =
          merge_access_type(p.get_access_type(), exit.at, callee_base_di,
                            addr_of, actual_param_di);

      if (!composed_opt)
        continue;
      Path r(poss_ret, nullptr, exit.at.get_llvm_type());
      r.set_access_type(*composed_opt);
      worklist.push_back(r);
    }
  }
}
const VFGNode *access_tracker_t::get_resume_node(SVF::NodeID ret,
                                                 const CallICFGNode *cs) {
  const VFGNode *formal_ret = svfg->getGNode(ret);

  for (auto e : formal_ret->getOutEdges()) {
    auto dst = e->getDstNode();
    if (auto actual_ret = SVFUtil::dyn_cast<ActualRetVFGNode>(dst))
      if (actual_ret->getCallSite() == cs)
        return dst;
    if (auto outs_ret = SVFUtil::dyn_cast<ActualOUTSVFGNode>(dst))
      if (outs_ret->getCallSite() == cs)
        return dst;
  }

  return nullptr;
}

static const char *key_str(const std::string *key) {
  return key ? key->c_str() : "<no di>";
}

std::optional<AccessType> access_tracker_t::merge_access_type(
    const AccessType &prefix, const AccessType &suffix,
    llvm::DIType *callee_base_di, bool addr_of, llvm::DIType *actual_di) {
  cinstr.attempts++;
  auto passed_di = actual_di;
  if (!passed_di)
    passed_di = prefix.get_di_type();

  auto matching =
      have_compatible_types(passed_di, callee_base_di, cinstr, addr_of ? 1 : 0);

  if (matching == matching_e::e_reject)
    return std::nullopt;

  if (matching == matching_e::e_is_void) {
    switch (get_void_policy()) {
    case void_policy_e::old_liberator:
      cinstr.void_dropped++;
      return std::nullopt;
    case void_policy_e::accept:
      break;
    case void_policy_e::cut:
      if (suffix.get_num_fields() > 0) {
        cinstr.void_dropped++;
        return std::nullopt;
      }
      break;
    }
  }

  // stop for self referencing data structures after MAX_GEP_RECURSION_DEPTH
  // rounds.
  for (auto &kv : suffix.get_visited_types()) {
    int count = prefix.visit_count(kv.first.first, kv.first.second) + kv.second;

    if (count > MAX_GEP_RECURSION_DEPTH)
      return std::nullopt;
  }

  AccessType out = prefix;

  // we stop when the path gets too deep, because then we likely have some
  // recursive definition.
  if (prefix.get_num_fields() + suffix.get_num_fields() > MAX_FIELD_DEPTH)
    return std::nullopt;

  if (suffix.get_num_fields() == 1) {
    out.append_path(suffix.get_fields(), prefix.get_kind(),
                    prefix.get_llvm_type(), prefix.get_di_type());
  }
  if (suffix.get_num_fields() > 1) {
    out.append_path(suffix.get_fields(), suffix.get_parent_kind(),
                    suffix.get_parent_llvm_type(), suffix.get_p_di_type());
  }

  out.set_kind(suffix.get_kind());

  // we only change the type if sfields is not empty.
  // as otherwise we track a wrong field type.
  if (suffix.get_num_fields() > 0) {
    if (!suffix.get_di_type())
      di_instr.compose_null_di++;
    out.set_type(suffix.get_di_type());
  }

  for (const auto &kv : suffix.get_visited_types()) {
    out.add_visited_count(kv.first.first, kv.first.second, kv.second);
  }

  return out;
}

const VFGNode *access_tracker_t::def_node_of(const SVFVar *var) {
  auto *vv = SVFUtil::dyn_cast<ValVar>(var);
  return (vv && svfg->hasDefSVFGNode(vv) ? svfg->getDefSVFGNode(vv) : nullptr);
}

bool access_tracker_t::summarize_from(const VFGNode *entry,
                                      llvm::DIType *di_type,
                                      func_summary_t &summ) {
  // We can use this to check for changes, because effects will grow in
  // size for access types. And exits will grow in size for finding more
  // ActualRetVFGNode. This is because the only functions changing the effects
  // are the merge_summary and the transform function.
  const auto org_effects = summ.effects.get_access_type_set().size();
  const size_t org_exists = summ.exits.size();
  const bool org_is_array = summ.effects.isArray();
  const bool org_is_malloc_size = summ.effects.isMallocSize();
  const bool org_is_file_path = summ.effects.isFilePath();

  ValueMetadata &work = summ.effects;

  // TODO: this should not be a set but an unordered_map and the key should
  // just be a plain struct containing the kind, fields, type, current node.
  // And using a good hash.
  std::vector<Path> worklist;
  std::set<visit_key_t> visited;

  (di_type ? di_instr.formal_with_di : di_instr.formal_no_di)++;

  worklist.push_back(Path(entry, nullptr, nullptr, di_type));

  // llvm::outs() << "Tracking key: " << key_str(di_key(di_type)) << "\n";

  size_t num = 1024;
  while (!worklist.empty()) {
    Path p = worklist.back();
    worklist.pop_back();

    instr.pops++;
    instr.max_worklist = std::max(worklist.size(), instr.max_worklist);
    instr.max_visited = std::max(visited.size(), instr.max_visited);

    // every 2^20
    if (worklist.size() >= num) {
      llvm::outs() << "[PATHS] formal=" << entry->getId() << " Function="
                   << (entry->getFun() ? entry->getFun()->getName()
                                       : std::string("global"))
                   << " pops=" << instr.pops
                   << " ditype=" << key_str(p.get_access_type().get_di_key())
                   << " fields=" << p.get_access_type().get_num_fields()
                   << " dedup_hits=" << instr.dedup_hits
                   << " depth cuts=" << instr.depth_cuts
                   << " worklist=" << worklist.size()
                   << " (max wl)=" << instr.max_worklist
                   << " visited=" << visited.size()
                   << " (max vis)=" << instr.max_visited << " effects AT="
                   << summ.effects.get_access_type_set().size()
                   << " exits=" << summ.exits.size() << " rss=" << rss_mib()
                   << "MiB\n";
      llvm::outs().flush();
      while (num <= worklist.size())
        num <<= 1;
    }

    const AccessType &path_at = p.get_access_type();

    if (!visited
             .insert(visit_key_t{p.getNode(), path_at.get_di_key(),
                                 path_at.get_kind(), path_at.get_fields()})
             .second) {
      instr.dedup_hits++;
      continue;
    }

    const VFGNode *curr = p.getNode();

    // Intra procedural analysis.
    auto lr = transfer_function(curr, p.get_access_type(), work, p);

    // Reasons we skip here:
    // 1. GEP found some pointer arithmetic, that we can't assign to
    // arrays or struct fields.
    // 2. GEP converts to a constant expression.
    if (lr.skip)
      continue;

    p.set_access_type(lr.ac_node);
    p.setPrevValue(lr.prev_value);

    const AccessType &cur_at = p.get_access_type();
    if (cur_at.get_num_fields() > static_cast<int>(MAX_FIELD_DEPTH) ||
        cur_at.exceeds_recursion(MAX_GEP_RECURSION_DEPTH)) {
      instr.depth_cuts++;
      continue;
    }

    // Leaf nodes in SVF are for example ActualRetVFGNodes that are not
    // consumed by the caller. A store that is never read. (dead store) A
    // terminal node, which is not used anymore further down.
    if (!curr->hasOutgoingEdge())
      continue;

    for (auto edge : curr->getOutEdges()) {
      // We will only track indirect edges, that origin from a store or a
      // MemoryIntraPhi.
      if (curr->getNodeKind() != VFGNode::VFGNodeK::Store &&
          curr->getNodeKind() != VFGNode::VFGNodeK::MIntraPhi &&
          SVFUtil::isa<SVF::IndirectSVFGEdge>(edge)) {
        continue;
      }

      auto *succ = edge->getDstNode();

      const CallICFGNode *cs = nullptr;

      // Case1: A call: merge the summaries.
      if (auto actual_param = SVFUtil::dyn_cast<ActualParmVFGNode>(succ)) {
        cs = actual_param->getCallSite();
      } else if (auto actual_in_svfg_node =
                     SVFUtil::dyn_cast<ActualINSVFGNode>(succ)) {
        cs = actual_in_svfg_node->getCallSite();
      }

      if (cs) {
        merge_summary(succ, cs, p, summ, worklist);
        continue;
      }

      // Case 2: return from the summary exits.
      if (SVFUtil::isa<ActualRetVFGNode>(succ) ||
          SVFUtil::isa<ActualOUTSVFGNode>(succ)) {
        summ.exits.insert(exit_state_t{curr->getId(), p.get_access_type()});
        continue;
      }

      // Case 3: Normal Intra edge
      // Copy path and push it into the worklist with successor node as
      // continuation point.
      Path ps = p;
      ps.setNode(succ);
      worklist.push_back(ps);
    }
  }

  // if the entry node was a pointer set the malloc size to false
  // regardless what the analysis said because pointer types can not be
  // malloc sizes.
  if (is_formal_pointer_type(entry)) {
    if (summ.effects.isMallocSize())
      type_instr.cleared_is_malloc_sz++;
    summ.effects.setMallocSize(false);
  }

  return summ.effects.get_access_type_set().size() != org_effects ||
         summ.exits.size() != org_exists ||
         summ.effects.isArray() != org_is_array ||
         summ.effects.isMallocSize() != org_is_malloc_size ||
         summ.effects.isFilePath() != org_is_file_path;
}

/**
 * For multi/cycles SCCs compute fixpoint for all other just compute
 * summmary for all formal params.
 * @param id of the function to be queried.
 */
/*void access_tracker_t::walk_scc(const FunObjVar *f) {
  if (!f)
    return;

  auto cg_node = cg->getCallGraphNode(f);
  if (!cg_node)
    return;
  SVF::NodeID rep = cg_scc->repNode(cg_node->getId());
  if (!scc_visited.insert(rep).second)
    return;

  // the callgraph is now a DAG. So we will first walk the DAG to the leaf
  // nodes using DFS and then evaluate the summaries of each function
  // bottom-up.
  for (const FunObjVar *G : functions_in_scc(rep)) {
    for (const FunObjVar *C : direct_callees(G)) {
      if (!C)
        continue;
      auto c_node = cg->getCallGraphNode(C);
      if (!c_node)
        continue;
      if (cg_scc->repNode(c_node->getId()) != rep) {
        walk_scc(C);
      }
    }
  }

  // fixpoint each SCC. A SCC node can contain one Function Object for
  // non-recursive and multiple for recursive functions. For recursive
  // function calls we need to evaluate the fixpoint.
  bool changed = true;
  int fixpoint_iter = 0;
  while (changed && fixpoint_iter < MAX_FIXPOINT_ITERATIONS) {
    fixpoint_iter++;
    changed = false;
    for (const FunObjVar *G : functions_in_scc(rep)) {
      for (SVF::NodeID fid : formal_ids(G)) {
        changed |= summarize_formal(fid, summaries[fid]);
      }
    }
  }
  if (changed) {
    llvm::outs() << "[WARN] SCC " << rep << " did not reach a fixpoint within "
                 << MAX_FIXPOINT_ITERATIONS
                 << " iterations; widening. Summaries for this SCC are "
                    "under-approximate.\n";
    llvm::outs().flush();
  }
}*/

bool access_tracker_t::summarize_return(const FunObjVar *f) {
  ret_summary_t &rs = ret_summaries[f];
  const size_t org_effects = rs.effects.get_access_type_set().size();
  const size_t org_params = rs.return_params.size();
  const bool org_arr = rs.effects.isArray();
  const bool org_fp = rs.effects.isFilePath();
  const bool org_ms = rs.effects.isMallocSize();

  // no dwarf debug information
  if (!rs.base_di)
    rs.base_di = ret_di_type(f);

  auto module_set = LLVMModuleSet::getLLVMModuleSet();

  func_summary_t tmp_summ;
  tmp_summ.effects = std::move(rs.effects);

  for (SVF::NodeID sid : backward_slice(f)) {
    const VFGNode *src = svfg->getGNode(sid);

    // handles the case where the return value was passed as an parameter to
    // f.
    if (auto *fp = SVFUtil::dyn_cast<FormalParmVFGNode>(src)) {
      unsigned int pos = 0;
      // determine the position
      for (auto *param : pag->getFunArgsList(f)) {
        if (param == fp->getParam())
          break;
        ++pos;
      }
      rs.return_params.insert(pos);

      // just use the already evaluated summary for the effects.
      auto it = summaries.find(sid);
      if (it != summaries.end()) {
        for (const AccessType &at : it->second.effects.get_access_type_set()) {
          tmp_summ.effects.get_access_type_set().insert_nodes(
              at, at.getICFGNodes());
        }
        tmp_summ.effects.setIsArray(tmp_summ.effects.isArray() ||
                                    it->second.effects.isArray());
        tmp_summ.effects.setIsFilePath(tmp_summ.effects.isFilePath() ||
                                       it->second.effects.isFilePath());
        tmp_summ.effects.setMallocSize(tmp_summ.effects.isMallocSize() ||
                                       it->second.effects.isMallocSize());
        continue;
      }
    }

    // Handles the case where the return value is the result of a function
    // call.
    // We go through each callee_target and call handlerDispatcher in case it
    // was an external function call. We then look if ret_summaries already
    // contains a summary for the callee function and add their summary to the
    // result.
    // We then do the forward walk using summarize_from to track further
    // accesses to the return value.
    if (auto *ar = SVFUtil::dyn_cast<ActualRetVFGNode>(src)) {
      const CallICFGNode *cs = ar->getCallSite();
      for (const FunObjVar *callee : callee_targets(cs)) {
        auto name = callee->getName();
        if (hasHandlerDispatcher(&tmp_summ.effects, name, cs, cs, -1,
                                 C_RETURN)) {
          AccessType at(nullptr, rs.base_di);
          handlerDispatcher(tmp_summ.effects, name, cs, cs, -1, at, C_RETURN,
                            nullptr);
        }
        auto it = ret_summaries.find(callee);
        if (it == ret_summaries.end())
          continue;
        const ret_summary_t &ret_sum = it->second;
        for (auto &at : ret_sum.effects.get_access_type_set()) {
          tmp_summ.effects.get_access_type_set().insert_nodes(
              at, at.getICFGNodes());
        }
        tmp_summ.effects.setIsArray(tmp_summ.effects.isArray() ||
                                    ret_sum.effects.isArray());
        tmp_summ.effects.setIsFilePath(tmp_summ.effects.isFilePath() ||
                                       ret_sum.effects.isFilePath());
        tmp_summ.effects.setMallocSize(tmp_summ.effects.isMallocSize() ||
                                       ret_sum.effects.isMallocSize());
        for (unsigned i : ret_sum.return_params) {
          if (i < cs->getActualParms().size())
            if (const VFGNode *arg_def = def_node_of(cs->getActualParms()[i]))
              summarize_from(arg_def, ret_sum.base_di, tmp_summ);
        }
      }
      // now walk forward to get further accesses to the return value.
      summarize_from(src, rs.base_di, tmp_summ);
      continue;
    }

    // handles the case where the return value is a stack or heap allocation.
    // If it is a heap allocation it must have come from a call to stdlib,
    // Therefore call handlerDispatcher. If it is a global variable, we can
    // not track its initialization because it is statically initialized ->
    // just assume it is and return a write to every thing.
    if (auto addr = SVFUtil::dyn_cast<AddrVFGNode>(src)) {
      const llvm::Value *v = module_set->getLLVMValue(addr->getValue());
      if (auto *call = dyn_cast_or_null<llvm::CallInst>(v)) {
        if (auto callee = call->getCalledFunction()) {
          string name = callee->getName().str();
          const CallICFGNode *cs = module_set->getCallICFGNode(call);
          if (hasHandlerDispatcher(&tmp_summ.effects, name, cs, cs, -1,
                                   C_RETURN)) {
            AccessType at(nullptr, rs.base_di);
            handlerDispatcher(tmp_summ.effects, name, cs, cs, -1, at, C_RETURN,
                              nullptr);
          }
        }
      } else if (SVFUtil::isa<GlobalVariable>(v)) {
        // For global variables we will not be able to track it's
        // initialization anyway. So just assume it is initialized.
        AccessType at(nullptr, rs.base_di);
        addWrteToAllFields(tmp_summ.effects, at,
                           pag->getICFG()->getFunExitICFGNode(f));
        continue;
      }
      summarize_from(src, rs.base_di, tmp_summ);
    }
  } // end backward_slice
  rs.effects = std::move(tmp_summ.effects);
  if (returns_pointer(f) && rs.effects.isMallocSize()) {
    rs.effects.setMallocSize(false);
    type_instr.cleared_is_malloc_sz++;
  }
  if (!rs.type) {
    if (!rs.effects.get_access_type_set().empty())
      rs.type = rs.effects.get_access_type_set().begin()->get_llvm_type();
  }

  return rs.effects.get_access_type_set().size() != org_effects ||
         rs.return_params.size() != org_params ||
         rs.effects.isArray() != org_arr || rs.effects.isFilePath() != org_fp ||
         rs.effects.isMallocSize() != org_ms;
}
void access_tracker_t::report_progress(SVF::NodeID n) {
  const unsigned scc_size = cg_scc->subNodes(n).count();
  num_analyzed_functions += scc_size;
  num_analyzed_sccs++;

  llvm::outs() << "[INFO " << num_analyzed_functions << "/"
               << total_cg_functions << " funcs, " << num_analyzed_sccs << "/"
               << total_sccs << " SCCs] callgraph: ";

  auto *rep_node = cg->getCallGraphNode(n);
  if (rep_node && rep_node->getFunction())
    llvm::outs() << rep_node->getFunction()->getName();
  else
    llvm::outs() << "<scc id=" << n << ">";

  if (scc_size > 1)
    llvm::outs() << " (SCC of " << scc_size << ")";

  llvm::outs() << "\n";
  llvm::outs().flush();
}

void access_tracker_t::dump_mem_stats() {
  size_t n_at = 0, n_icfg = 0, n_vis = 0, n_fields = 0, n_pfields = 0,
         n_exits = 0, max_set = 0, n_fp = 0;

  size_t bytes_ats = 0, bytes_total_ats = 0, bytes_exits = 0, bytes_idx = 0,
         bytes_fp = 0;

  auto add_metadata = [&](const ValueMetadata &mdata) {
    const auto &ats = mdata.get_access_type_set();
    max_set = std::max(max_set, ats.size());
    n_at += ats.size();
    bytes_ats += ats.size() * chunk(sizeof(std::_Rb_tree_node<AccessType>));
    for (const AccessType &at : ats) {
      n_icfg += at.getICFGNodes().size();
      n_vis += at.get_visited_types().size();
      bytes_total_ats += at_heap(at);
    }

    const auto fps = mdata.get_len_source();
    n_fp += fps.size();
    bytes_fp += chunk(fps.size() * sizeof(fps[0]));
  };

  for (const auto &kv : summaries) {
    add_metadata(kv.second.effects);
    n_exits += kv.second.exits.size();
    bytes_exits += tree_bytes(kv.second.exits);
    for (const exit_state_t &e : kv.second.exits)
      bytes_exits += at_heap(e.at);
  }

  for (const auto &kv : ret_summaries)
    add_metadata(kv.second.effects);

  const size_t bytes_summaries = summaries.size() + ret_summaries.size();

  auto mib = [](size_t b) { return llvm::format("%.1f", b / 1048576.0); };
  const size_t total = bytes_summaries + bytes_ats + bytes_total_ats +
                       bytes_exits + bytes_idx + bytes_fp;

  llvm::outs() << "[MEM SUMMARIES] num summaries=" << summaries.size()
               << " num ret summaries=" << ret_summaries.size()
               << " num ats=" << n_at << "(max/summary=" << max_set
               << ") num icfg=" << n_icfg << " num vis=" << n_vis
               << " num exits=" << n_exits << " fun_params=" << n_fp
               << " MiB: maps=" << mib(bytes_summaries)
               << " ats=" << mib(bytes_ats)
               << " total ats=" << mib(bytes_total_ats)
               << " bytes exists=" << mib(bytes_exits)
               << " bytes indexes=" << mib(bytes_idx)
               << " bytes fun_params=" << mib(bytes_fp)
               << " TOTAL=" << mib(total) << " rss=" << rss_mib() << "MiB\n";

  llvm::outs().flush();
}

std::vector<const FunObjVar *>
access_tracker_t::functions_in_scc(SVF::NodeID rep) {
  std::vector<const FunObjVar *> results;

  for (SVF::NodeID id : cg_scc->subNodes(rep)) {
    auto n = cg->getCallGraphNode(id);
    results.push_back(n->getFunction());
  }
  return results;
}

enum class walk_e { e_ok, e_unknown, e_out_of_range };

static walk_e di_step(DIType *cur, int field, DIType *&out) {
  if (field < 0) {
    out = cur;
    return walk_e::e_ok;
  }

  auto comp = dyn_cast_or_null<DICompositeType>(decay_di_type(cur));

  if (!comp) {
    return walk_e::e_unknown;
  }

  auto tag = comp->getTag();
  if (tag != llvm::dwarf::DW_TAG_class_type &&
      tag != llvm::dwarf::DW_TAG_structure_type &&
      tag != llvm::dwarf::DW_TAG_union_type)
    return walk_e::e_unknown;

  int n_members = 0;
  llvm::DIType *hit = nullptr;
  for (auto el : comp->getElements()) {
    auto mem = dyn_cast<DIDerivedType>(el);
    if (!mem || mem->getTag() != llvm::dwarf::DW_TAG_member) {
      continue;
    }
    if (mem->getName().empty())
      return walk_e::e_unknown;
    if (n_members == field)
      hit = mem->getBaseType();
    ++n_members;
  }

  if (n_members == 0)
    return walk_e::e_unknown;

  if (!hit)
    return walk_e::e_out_of_range;

  out = hit;
  return out ? walk_e::e_ok : walk_e::e_unknown;
}

void access_tracker_t::validate_summary(SVF::NodeID formal_id,
                                        const func_summary_t &summ) {
  if (!config_t::instance()->print_metrics) {
    return;
  }
  if (bad_at_reports > MAX_BAD_AT_REPORTS)
    return;
  if (!validated.insert(formal_id).second)
    return;

  llvm::DIType *base_di = formal_di_type(formal_id);

  if (!base_di) {
    return;
  }
  const auto *fp = dyn_cast<FormalParmVFGNode>(svfg->getGNode(formal_id));
  std::string fname =
      (fp && fp->getFun()) ? fp->getFun()->getName() : "<unknown>";

  unsigned reported = 0;
  for (const AccessType &at : summ.effects.get_access_type_set()) {
    if (at.get_num_fields() == 0)
      continue;

    llvm::DIType *cur = base_di;
    walk_e st = walk_e::e_ok;
    for (int f : at.get_fields()) {
      llvm::DIType *next = nullptr;
      st = di_step(cur, f, next);
      if (st != walk_e::e_ok)
        break;
      cur = next;
    }

    if (st == walk_e::e_unknown) {
      continue;
    }

    auto want = di_aggregate_name(cur);
    auto got = di_aggregate_name(at.get_di_type());

    const char *why = nullptr;
    if (st == walk_e::e_out_of_range)
      why = "field index does not exists on this record";
    else if ((!want.first.empty() && !got.first.empty()) &&
             (want.first != got.first || want.second != got.second))
      why = "endpoint type disagrees with DWARF walk";

    if (!why)
      continue;

    llvm::outs() << "[BADAT] " << fname << " formal=" << formal_id
                 << " path=" << fmt_path(at.get_fields())
                 << " access=" << to_string(at.get_kind()) << " want="
                 << (want.first.empty() ? llvm::StringRef("<unnamed>")
                                        : want.first)
                 << "/" << want.second << " got="
                 << (got.first.empty() ? llvm::StringRef("<unnamed>")
                                       : got.first)
                 << "/" << got.second << " (" << why << ")\n";

    if (++bad_at_reports >= MAX_BAD_AT_REPORTS) {
      llvm::outs() << "[BADAT] too many bad ats\n";
      break;
    }

    if (++reported >= MAX_REPORTS_PER_FORMAL)
      break;
  }
  llvm::outs().flush();
}

void access_tracker_t::process_scc(NodeID id) {
  report_progress(id);

  if (num_analyzed_functions % 500 == 0) {
    dump_mem_stats();
    llvm::outs() << "[DICHAIN periodic] ok=" << di_instr.ok
                 << " fresh_break=" << di_instr.fresh_break
                 << " poisoned=" << di_instr.poisoned
                 << " | formal_with_di=" << di_instr.formal_with_di
                 << " | formal_no_di=" << di_instr.formal_no_di
                 << " compose_null_di=" << di_instr.compose_null_di
                 << " | gep_bu(ok/null)=" << di_instr.gep_bu_ok << "/"
                 << di_instr.gep_bu_null << "\n";
    llvm::outs().flush();
  }

  llvm::SmallVector<SVF::NodeID, 8> formals;
  llvm::SmallVector<const SVF::FunObjVar *, 8> rets;

  for (SVF::NodeID tid : cg_scc->subNodes(id)) {
    const FunObjVar *f = cg->getGNode(tid)->getFunction();
    for (SVF::NodeID fid : formal_ids(f)) {
      formals.push_back(fid);
    }

    if (f && !f->isDeclaration() && returns_pointer(f))
      rets.push_back(f);
  }

  if (formals.empty() && rets.empty())
    return;

  // Single SCC (non-recursive function)
  if (!cg_scc->isInCycle(id)) {
    for (auto fid : formals) {
      summarize_formal(fid, summaries[fid]);
    }
    for (const FunObjVar *f : rets) {
      summarize_return(f);
    }

    for (auto fid : formals) {
      validate_summary(fid, summaries[fid]);
    }

    return;
  }

  bool changed = true;

  // print scc
  auto nodes = cg_scc->subNodes(id);
  int num = 1;
  for (auto n : nodes) {
    auto tmp = cg->getCallGraphNode(n);
    llvm::outs() << tmp->getName() << "\n";

    if (num == 5) {
      llvm::outs() << "\n";
      num = 0;
    }

    num++;
  }

  int fixpoint_iter = 0;
  // Fixpoint iteration
  while (changed) {
    if (fixpoint_iter >= MAX_FIXPOINT_ITERATIONS) {
      llvm::outs() << "[WARN] SCC " << id << " (" << formals.size()
                   << " formals) did not reach a fixpoint within "
                   << MAX_FIXPOINT_ITERATIONS << " iterations";
      llvm::outs().flush();
      break;
    }
    fixpoint_iter++;
    changed = false;

    for (const FunObjVar *f : functions_in_scc(id)) {
      for (SVF::NodeID fid : formal_ids(f)) {
        changed |= summarize_formal(fid, summaries[fid]);
      }
    }

    for (const FunObjVar *f : rets) {
      changed |= summarize_return(f);
    }

    size_t total_ats = 0, total_exits = 0, max_fields = 0;

    for (auto fid : formals) {
      const auto &ats = summaries[fid].effects.get_access_type_set();
      total_ats += ats.size();
      total_exits += summaries[fid].exits.size();
      for (const AccessType &at : ats) {
        max_fields = std::max<size_t>(max_fields, at.get_num_fields());
      }
    }

    llvm::outs() << "[FIXPOINT] scc=" << id << " iter=" << fixpoint_iter
                 << " formals=" << formals.size() << " total_ats=" << total_ats
                 << " exits=" << total_exits
                 << " max_field_depth=" << max_fields << "\n";
    llvm::outs() << "[DICHAIN] ok=" << di_instr.ok
                 << " fresh_break=" << di_instr.fresh_break
                 << " poisoned=" << di_instr.poisoned
                 << " | formal_with_di=" << di_instr.formal_with_di
                 << " | formal_no_di=" << di_instr.formal_no_di
                 << " compose_null_di=" << di_instr.compose_null_di
                 << " | gep_bu(ok/null)=" << di_instr.gep_bu_ok << "/"
                 << di_instr.gep_bu_null << "\n";

    llvm::outs().flush();
  }

  for (auto fid : formals) {
    validate_summary(fid, summaries[fid]);
    auto fp = dyn_cast<FormalParmVFGNode>(svfg->getGNode(fid));
    llvm::outs() << "[process_scc] "
                 << "Evaluated Function: " << fp->getFun()->getName()
                 << "(Formal Param ID: " << fid << ")\n"
                 << "Effects: " << print_summary(summaries[fid].effects)
                 << "Exit Count: " << summaries[fid].exits.size() << "\n\n";
  }
}

void access_tracker_t::walk_inverted_scc(const FunObjVar *f) {
  if (!f)
    return;

  auto cg_node = cg->getCallGraphNode(f);
  if (!cg_node)
    return;

  SVF::NodeID rep = cg_scc->repNode(cg_node->getId());

  while (!scc_visited.count(rep) && !inverted_scc.empty()) {
    auto el = inverted_scc.top();
    inverted_scc.pop();
    process_scc(el);
    scc_visited.insert(el);
  }
}

func_summary_t &access_tracker_t::get_summary(SVF::NodeID param_id) {
  SVF::NodeID fid = param_id;
  auto *pNode = pag->getGNode(param_id);
  if (pNode && SVFUtil::isa<SVF::ValVar>(pNode)) {
    auto *valVar = SVFUtil::cast<SVF::ValVar>(pNode);
    if (svfg->hasDefSVFGNode(valVar)) {
      fid = svfg->getDefSVFGNode(valVar)->getId();
    }
  }
  return summaries[fid];
}

bool access_tracker_t::summarize_formal(SVF::NodeID formal_id,
                                        func_summary_t &summ) {
  return summarize_from(svfg->getGNode(formal_id), formal_di_type(formal_id),
                        summ);
}

llvm::DIType *access_tracker_t::formal_di_type(SVF::NodeID formal_id) {
  const VFGNode *entry = svfg->getGNode(formal_id);
  if (auto param = dyn_cast<FormalParmVFGNode>(entry)) {
    auto *llvm_module_set = LLVMModuleSet::getLLVMModuleSet();
    if (auto val = llvm_module_set->getLLVMValue(param->getParam()))
      return restore_param_di_type(val);
  }
  GEP_LOG("Could not find di type for formal_id {}\n", formal_id);
  return nullptr;
}

vector<SVF::NodeID> access_tracker_t::formal_ids(const FunObjVar *G) {
  std::vector<SVF::NodeID> ids;

  // there can be cases where we have inline assembly that we have to skip
  if (!G || G->isDeclaration() || !pag->hasFunArgsList(G))
    return ids;

  for (const SVFVar *arg : pag->getFunArgsList(G)) {
    if (!arg->getType() /*|| !arg->getType()->isPointerTy() */)
      continue;

    auto *vv = SVFUtil::cast<ValVar>(arg);

    if (!svfg->hasDefSVFGNode(vv))
      continue;

    ids.push_back(svfg->getDefSVFGNode(vv)->getId());
  }

  return ids;
}

std::vector<const FunObjVar *>
access_tracker_t::callee_targets(const CallICFGNode *cs) {
  std::vector<const FunObjVar *> res;

  if (!cs->isIndirectCall()) {
    if (const FunObjVar *t = cs->getCalledFunction())
      res.push_back(t);
    return res;
  }

  if (!config_t::instance()->consider_indirect_calls)
    return res;

  std::set<const FunObjVar *> targets;
  if (cg->hasIndCSCallees(cs)) {
    const auto &resolved = cg->getIndCSCallees(cs);
    targets.insert(resolved.begin(), resolved.end());
  }

  auto it = ValueMetadata::myCallEdgeMap_inst.find(cs);
  if (it != ValueMetadata::myCallEdgeMap_inst.end())
    targets.insert(it->second.begin(), it->second.end());

  res.assign(targets.begin(), targets.end());
  return res;
}

static bool compatible_signature(const CallICFGNode *cs,
                                 const FunObjVar *callee) {
  if (!cs || !callee)
    return true;
  if (callee->isVarArg())
    return true;

  const auto &params = cs->getActualParms();
  if (params.size() != callee->arg_size()) {
    return false;
  }

  for (auto i = 0; i < params.size(); ++i) {
    const SVFType *actual = params[i] ? params[i]->getType() : nullptr;
    const auto *formal_var = callee->getArg(i);
    const SVFType *formal = formal_var ? formal_var->getType() : nullptr;
    if (!actual || !formal || actual == formal)
      continue;
    if (actual->isPointerTy() && formal->isPointerTy())
      continue;
    return false;
  }
  return true;
}

void access_tracker_t::dump_scc_stats(int num_top) {
  struct scc_info_t {
    SVF::NodeID rep = 0;
    size_t functions = 0;
    size_t formals = 0;
    size_t intra_direct = 0;   // direct calls
    size_t intra_indirect = 0; // indirect calls
  };

  scc_info_t info;
  std::unordered_map<NodeID, scc_info_t> info_map;

  for (const auto &kv : *cg) {
    CallGraphNode *node = kv.second;
    const SVF::NodeID rep = cg_scc->repNode(node->getId());
    scc_info_t &info = info_map[rep];
    info.rep = rep;
    info.functions++;
    info.formals += formal_ids(node->getFunction()).size();

    for (auto edge : node->getOutEdges()) {
      if (cg_scc->repNode(edge->getDstID()) != rep)
        continue;
      if (!edge->getDirectCalls().empty())
        info.intra_direct++;
      if (!edge->getIndirectCalls().empty())
        info.intra_indirect++;
    }
  }

  std::vector<scc_info_t> sorted;
  sorted.reserve(info_map.size());

  for (auto &kv : info_map) {
    sorted.push_back(kv.second);
  }
  // sort descending
  std::sort(sorted.begin(), sorted.end(),
            [](const scc_info_t &s1, const scc_info_t &s2) {
              return s1.formals > s2.formals;
            });

  llvm::outs() << "[SCC] top " << num_top << " largest SCCs (biggest first):\n";
  int num = 0;
  for (auto &i : sorted) {
    if (num++ >= num_top)
      break;
    llvm::outs() << "[SCC] scc=" << i.rep << " num functions=" << i.functions
                 << " formals=" << i.formals
                 << " num direct edges=" << i.intra_direct
                 << " num indirect edges=" << i.intra_indirect << "\n";
  }

  if (!sorted.empty() && sorted.front().intra_indirect > 0) {
    auto rep = sorted.front().rep;
    size_t shown = 0;
    llvm::outs() << "[SCC] indirect edges inside scc rep=" << rep << ":\n";
    for (auto scc : cg_scc->subNodes(rep)) {
      CallGraphNode *node = cg->getCallGraphNode(scc);
      for (auto &e : node->getOutEdges()) {
        if (shown == num_top)
          break;
        if (cg_scc->repNode(e->getDstID()) != rep ||
            e->getIndirectCalls().empty())
          continue;
        llvm::outs()
            << "[SCC] " << node->getFunction()->getName() << " -> "
            << cg->getCallGraphNode(e->getDstID())->getFunction()->getName()
            << " (" << e->getIndirectCalls().size() << " callsites)\n";
        ++shown;
      }
    }
  }

  vector<NodeID> members;
  size_t cyclic = 0;
  size_t direct_only = largest_component(
      [](CallGraphEdge *e) { return !e->getDirectCalls().empty(); }, members,
      cyclic);

  llvm::outs() << "[SCC] direct edges only: largest=" << direct_only
               << " functions (" << formals_of(members) << " formals), "
               << cyclic << " cyclic components\n";

  members.clear();
  cyclic = 0;
  size_t filtered = largest_component(
      [](CallGraphEdge *e) {
        if (!e->getDirectCalls().empty())
          return true;
        const FunObjVar *callee = e->getDstNode()->getFunction();
        for (const CallICFGNode *cs : e->getIndirectCalls())
          if (compatible_signature(cs, callee))
            return true;
        return false;
      },
      members, cyclic);

  llvm::outs() << "[SCC] signature-filtered:largest=" << filtered
               << " functions (" << formals_of(members) << " formals),"
               << cyclic << " cyclic components\n";

  llvm::outs().flush();
  dump_indirect_block(num_top);
}

void access_tracker_t::dump_indirect_block(int num_top) {
  // (callsites -> targets)
  std::unordered_map<const CallICFGNode *, std::set<const FunObjVar *>> targets;

  for (const auto &kv : *cg) {
    for (auto e : kv.second->getOutEdges()) {
      const FunObjVar *callee = e->getDstNode()->getFunction();
      for (const CallICFGNode *cs : e->getIndirectCalls()) {
        targets[cs].insert(callee);
      }
    }
  }
  llvm::outs() << "[IND] num of indirect callsites=" << targets.size() << "\n";

  std::vector<const CallICFGNode *> ranking;
  ranking.reserve(targets.size());

  // rank the callsites for number of indirect pushes
  for (const auto &kv : targets) {
    ranking.push_back(kv.first);
  }
  std::sort(ranking.begin(), ranking.end(),
            [&targets](const CallICFGNode *cs1, const CallICFGNode *cs2) {
              return targets[cs1].size() > targets[cs2].size(); // descending
            });

  std::set<const CallICFGNode *> removed_cs;
  std::vector<SVF::NodeID> members;

  // remove all indirect edges from the graph that results from the indirect
  // callsite cs. The aim is to see if the large recursive SCCs disappear.
  for (size_t i = 0; i < ranking.size() && i < num_top; ++i) {
    const CallICFGNode *cs = ranking[i];
    size_t cyclic = 0;
    members.clear();

    const size_t largest = largest_component(
        [&cs](CallGraphEdge *e) {
          if (!e->getDirectCalls().empty())
            return true;
          for (const CallICFGNode *c : e->getIndirectCalls())
            // only keep edges, where the callsites of the currently tracked
            // callsite does not match the callsite in the graph.
            if (c != cs)
              return true;

          return false;
        },
        members, cyclic);
    llvm::outs() << "[IND] " << cs->getCaller()->getName()
                 << "(cs=" << cs->getId() << ") targets=" << targets[cs].size()
                 << "-> largest=" << largest << " functions ("
                 << formals_of(members) << " formals)\n";
    removed_cs.insert(cs);
  }

  // What remains if all the callsites are suppressed.
  size_t cyclic = 0;
  members.clear();
  const auto largest = largest_component(
      [&removed_cs](CallGraphEdge *e) {
        if (!e->getDirectCalls().empty())
          return true;
        // this "set" should always only have one element.
        for (const CallICFGNode *c : e->getIndirectCalls())
          if (!removed_cs.count(c))
            return true;
        return false;
      },
      members, cyclic);

  llvm::outs() << "[IND] removing top " << removed_cs.size()
               << " indirect callsites. Remaining largest SCC=" << largest
               << " (with" << formals_of(members) << " formals), with "
               << cyclic << " cyclic components\n";
  llvm::outs().flush();
}

size_t access_tracker_t::formals_of(const std::vector<SVF::NodeID> &nodes) {
  size_t sum = 0;

  for (auto id : nodes) {
    sum += formal_ids(cg->getCallGraphNode(id)->getFunction()).size();
  }

  return sum;
}

NodeID access_tracker_t::ret_id_of(const FunObjVar *f) {
  if (!f || f->isDeclaration() || !pag->funHasRet(f))
    return 0;
  const SVFVar *exits = pag->getFunRet(f);
  auto *vv = dyn_cast<ValVar>(exits);
  if (!vv || !svfg->hasDefSVFGNode(vv))
    return 0;
  return svfg->getDefSVFGNode(vv)->getId();
}

const std::vector<SVF::NodeID> &
access_tracker_t::backward_slice(const FunObjVar *f) {
  auto it = ret_sources.find(f);

  if (it != ret_sources.end())
    return it->second;

  auto &out = ret_sources[f];
  NodeID ret = ret_id_of(f);
  if (ret == 0)
    return out;

  // instead of walking the ICFG graph we walk the SVFG graph back
  // and get
  std::unordered_set<SVF::NodeID> visited;
  vector<const VFGNode *> wl{svfg->getGNode(ret)};
  while (!wl.empty()) {
    const VFGNode *n = wl.back();
    wl.pop_back();
    if (!visited.insert(n->getId()).second)
      continue;

    if (visited.size() > MAX_SLICE_NODES) {
      ret_summaries[f].widened = true;
      break;
    }

    if (n->getFun() != f)
      continue;
    if (SVFUtil::isa<GepVFGNode>(n)) {
      auto gep_param = n->getValue();
    }

    // This is what the original condition extractor extractReturnMetadata
    // function tracks.
    if (SVFUtil::isa<AddrVFGNode>(n) || SVFUtil::isa<ActualRetVFGNode>(n) ||
        SVFUtil::isa<FormalParmVFGNode>(n)) {
      out.push_back(n->getId());
      continue;
    }

    for (auto e : n->getInEdges()) {
      wl.push_back(e->getSrcNode());
    }
  }
  return out;
}

access_tracker_t &access_tracker_t::get_tracker(const SVFG &vfg) {
  static std::unordered_map<const SVFG *, std::unique_ptr<access_tracker_t>>
      trackers;
  auto &t = trackers[&vfg];
  if (!t)
    t = make_unique<access_tracker_t>(SVFIR::getPAG(), vfg.getPTA(),
                                      const_cast<SVFG *>(&vfg));
  return *t;
};

access_tracker_t::access_tracker_t(SVF::SVFIR *svfir,
                                   SVF::PointerAnalysis *pta_, SVF::SVFG *svfg_)
    : pag(svfir), pta(pta_), svfg(svfg_), cg(pta_->getCallGraph()) {
  // evaluate SCC using Tarjan
  cg_scc = new SCCDetection<CallGraph *>(cg);
  // execute scc
  cg_scc->find();

  // Reverse the SCC order for bottom-up traversal.
  // Important: Do not use topoNodeStack after this again
  // because it will be empty.
  worklist_t st = cg_scc->topoNodeStack();

  while (!st.empty()) {
    auto el = st.top();
    st.pop();
    inverted_scc.push(el);
  }

  total_cg_functions = cg->getTotalNodeNum();
  total_sccs = inverted_scc.size();

  llvm::outs() << "[INFO] CallGraph: " << total_cg_functions << " functions in "
               << total_sccs << " SCCs\n";
  llvm::outs().flush();

  if (config_t::instance()->print_metrics)
    dump_scc_stats();
}
ValueMetadata access_tracker_t::extract_return_metadata(const SVFG &vfg,
                                                        const Value *ret_val,
                                                        const FunObjVar *f) {
  auto &tracker = access_tracker_t::get_tracker(vfg);
  tracker.walk_inverted_scc(f);
  ValueMetadata mdata = tracker.get_ret_summary(f).effects;
  mdata.setValue(ret_val);
  return mdata;
}
ValueMetadata access_tracker_t::extract_parameter_metadata(const SVFG &vfg,
                                                           const Value *val,
                                                           unsigned param_id) {
  auto &tracker = access_tracker_t::get_tracker(vfg);
  const FunObjVar *f = get_function(vfg, param_id);
  tracker.walk_inverted_scc(f);

  func_summary_t &s = tracker.get_summary(param_id);
  ValueMetadata mdata = s.effects;
  mdata.setValue(val);
  return mdata;
}
len_dependency_tracker_t::len_dependency_tracker_t(const SVFG &svfg) noexcept
    : svfg_(svfg) {}

std::string len_dependency_tracker_t::extract(const SVF::SVFVar *current_param,
                                              ValueMetadata &mdata) {
  auto fun = current_param->getFunction();
  if (!fun || fun->isDeclaration())
    return {};
  // optimization return asap if no parameter is a non-pointer.
  auto fun_arg_list = PAG::getPAG()->getFunArgsList(fun);
  bool has_non_ptr = false;
  for (auto arg : fun_arg_list) {
    has_non_ptr |= !arg->getType()->isPointerTy();
  }
  if (!has_non_ptr)
    return {};

  auto module_set = LLVMModuleSet::getLLVMModuleSet();
  auto pag = PAG::getPAG();

  auto llvm_fun = dyn_cast<llvm::Function>(module_set->getLLVMValue(fun));
  auto param_type = current_param->getType();

  // if param is no pointer we skip checking asap.
  if (!param_type->isPointerTy())
    return "";

  // gets the SVFG node for a PAG node, if it exists.
  auto def_node = [&](const SVF::SVFVar *var) -> const VFGNode * {
    auto *valvar = var ? SVFUtil::dyn_cast<SVF::ValVar>(var) : nullptr;
    return valvar && svfg_.hasDefSVFGNode(valvar) ? svfg_.getDefSVFGNode(valvar)
                                                  : nullptr;
  };

  const auto &fun_params = pag->getFunArgsList(fun);

  // gets the SVFG node for an llvm::Value, if it exists.
  auto value_def_node = [&](const llvm::Value *v) -> const VFGNode * {
    if (!v || !module_set->hasValueNode(v)) {
      return nullptr;
    }
    return def_node(pag->getGNode(module_set->getValueNode(v)));
  };

  // (index of param, SVFG node of parameter)
  vector<pair<int, const VFGNode *>> len_params;
  size_t param_idx = 0;
  // Formal Parameters in fun_params.
  for (auto param : fun_params) {
    // we need to check size only for parameters that are non pointers.
    if (param != current_param && !param->getType()->isPointerTy()) {
      // push (param_idx, SVFG node) to len_params
      len_params.emplace_back(param_idx, def_node(param));
    }
    param_idx++;
  }

  // if not length parameters candidates are found, we just return.
  if (len_params.empty())
    return {};

  // evalute and cache the definitionSets (root nodes) of each formal
  // parameter beforehand.
  map<const VFGNode *, set<const VFGNode *>> def_cache;
  auto defs_of = [&](const VFGNode *node) -> const std::set<const VFGNode *> & {
    auto it = def_cache.find(node);
    if (it == def_cache.end()) {
      it = def_cache
               .emplace(node, node ? getDefinitionSet(node)
                                   : std::set<const VFGNode *>{})
               .first;
    }
    return it->second;
  };

  std::string dependent_param = "";

  std::optional<std::set<const VFGNode *>> index_defs;
  std::set<const Loop *> visited_loops;

  std::vector<const SVFVar *> param_list(fun_params.begin(), fun_params.end());
  int cmp_param = track_compares(current_param, mdata, param_list);
  if (cmp_param >= 0)
    return "param_" + std::to_string(cmp_param);

  for (auto i : mdata.getIndexes()) {
    llvm::Instruction *ii = llvm::dyn_cast<Instruction>(i);
    if (!ii)
      continue;

    llvm::Function *f = ii->getFunction();
    auto &li = loop_infos_[f];
    if (!li.first) {
      li.first = std::make_unique<DominatorTree>(*f);
      li.second = std::make_unique<LoopInfo>(*li.first);
    }

    Loop *l = li.second->getLoopFor(ii->getParent());

    if (l == nullptr || !visited_loops.insert(l).second)
      continue;

    if (!index_defs) {
      index_defs.emplace();
      for (auto idx : mdata.getIndexes()) {
        if (const VFGNode *n = value_def_node(idx)) {
          const auto &defs = defs_of(n);
          index_defs->insert(defs.begin(), defs.end());
        }
      }
    }

    SmallVector<BasicBlock *> exits;
    l->getExitingBlocks(exits);

    // exit blocks of loops are contain branches that branch to places outside
    // the loop.
    for (auto e : exits) {
      // find terminator instruction in the cache and evaluate the defintions if
      // not available.
      const auto &exit_defs = defs_of(value_def_node(&e->back()));

      // instead of evaluating the intersection of the root nodes O(n+m) (n size
      // of set1, m size of set2), we notice that both sets are sorted by the
      // same key (VFGNode*) so equal values should be equal pointers. We just
      // walk both sets until we find the first matching pointers instead of
      // evaluating the whole set.
      if (!intersects(*index_defs, exit_defs))
        continue;

      for (const auto &[idx, vP] : len_params) {
        if (intersects(defs_of(vP), exit_defs)) {
          dependent_param = "param_" + std::to_string(idx);
          break;
        }
      } // end of for len params
    } // end of for exits
  }

  // If the first pass could not find a dependent_param
  if (dependent_param == "") {
    // (array index var, Path)
    for (const auto &el : mdata.get_len_source()) {
      const VFGNode *vS = value_def_node(el.first);
      if (!vS)
        continue;

      auto path = el.second;

      std::set<const VFGNode *> slot_defs = getDefinitionSetCtx(vS, &path);

      for (const auto &[idx, vP] : len_params) {
        if (intersects(defs_of(vP), slot_defs)) {
          dependent_param = "param_" + std::to_string(idx);
          break;
        }
      }
    }
  }

  return dependent_param;
}

int len_dependency_tracker_t::track_compares(
    const SVFVar *current_parm, liberator::ValueMetadata &mdata,
    const std::vector<const SVFVar *> &params) {
  SVFIR *pag = SVFIR::getPAG();
  auto llvm_module_set = LLVMModuleSet::getLLVMModuleSet();

  // candiate parameter -> number of comparisons naming it
  std::map<int, unsigned> votes;

  for (auto idx : mdata.getIndexes()) {
    // for pointer arithmetic, we store the GEP in handleGep
    // therefore there is index variable to compare
    if (SVFUtil::isa<llvm::GetElementPtrInst>(idx))
      continue;

    auto *inst = llvm::dyn_cast<llvm::Instruction>(idx);
    if (!inst)
      continue;
    const llvm::Function *f = inst->getFunction();
    auto &kv = loop_infos_[f];
    if (!kv.first) {
      kv.first =
          std::make_unique<DominatorTree>(*const_cast<llvm::Function *>(f));
      kv.second = std::make_unique<LoopInfo>(*kv.first);
    }
    Loop *loop = kv.second->getLoopFor(inst->getParent());

    if (!loop)
      continue;

    const Value *unique_idx = get_cannonical_index(idx);

    for (llvm::BasicBlock *bb : loop->blocks()) {
      // search for the CmpInstructions
      for (llvm::Instruction &inst : *bb) {
        auto *cmp = dyn_cast<llvm::ICmpInst>(&inst);
        if (!cmp)
          continue;
        const Value *other = nullptr;
        if (get_cannonical_index(cmp->getOperand(0)) == unique_idx)
          other = cmp->getOperand(1);
        else if (get_cannonical_index(cmp->getOperand(1)) == unique_idx)
          other = cmp->getOperand(0);
        if (!other || isa<llvm::Constant>(other))
          continue;

        // other var must be the parameter value
        auto *other_var = dyn_cast<ValVar>(
            pag->getGNode(llvm_module_set->getValueNode(other)));
        if (!other_var || !svfg_.hasDefSVFGNode(other_var))
          continue;
        const VFGNode *other_vfg = svfg_.getDefSVFGNode(other_var);

        int param_idx = 0;
        for (const SVFVar *p : params) {
          const int this_idx = param_idx++;
          if (p == current_parm)
            continue;
          auto llvm_param = llvm_module_set->getLLVMValue(p);
          if (!llvm_param || isa<PointerType>(llvm_param->getType()))
            continue;
          auto *param_var = dyn_cast<ValVar>(p);
          if (!param_var || !svfg_.hasDefSVFGNode(param_var))
            continue;
          if (backward_flow(other_vfg, svfg_.getDefSVFGNode(param_var), svfg_))
            votes[this_idx]++;
        }
      }
    }
  }
  int best = -1;
  size_t best_votes = 0;
  for (auto [idx, n] : votes) {
    if (n > best_votes) {
      best = idx;
      best_votes = n;
    }
  }
  return best;
}
} // namespace liberator
