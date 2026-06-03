#include "ConditionExtractor.hpp"
#include "AccessType.h"
#include "Config.h"
#include "Dominators.h"
#include "FileLogger.h"
#include "FunctionConditions.hpp"
#include "GenericDominatorTy.h"
#include "GlobalStruct.h"
#include "IBBG.h"
#include "LibfuzzUtil.h"
#include "PhiFunction.h"
#include "PostDominators.h"
#include "Profiler.hpp"
#include "SVF-LLVM/ObjTypeInference.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFVariables.h"
#include "TypeMatcher.h"
#include "Util.hpp"
#include "Util/Options.h"
#include "Util/SVFUtil.h"
#include "ValueMetadata.hpp"
#include "WPA/Andersen.h"
#include "WPA/AndersenPWC.h"
#include "WPA/TypeAnalysis.h"
#include <MSSA/SVFGBuilder.h>
#include <SVF-LLVM/BasicTypes.h>
#include <SVF-LLVM/LLVMModule.h>
#include <SVF-LLVM/LLVMUtil.h>
#include <SVF-LLVM/SVFIRBuilder.h>
#include <algorithm>
#include <filesystem>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/TypedPointerType.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <ranges>
#include <utility>

using namespace SVF;
using namespace SVFUtil;
using namespace LLVMUtil;

namespace {

llvm::Type *infer_type_from_arg_attrs(const llvm::Argument *arg) {
  if (!arg->getType()->isPointerTy())
    return nullptr;
  // When the struct type is passed by value but is to big
  if (auto *T = arg->getParamByValType())
    return T;
  return nullptr;
}

llvm::DIType *peel_di_qualifiers(llvm::DIType *t) {
  using namespace llvm::dwarf;
  if (!t) {
    TYPE_LOG("peel_di_qualifiers: null input\n");
    return nullptr;
  }
  std::string s;
  raw_string_ostream os(s);
  t->printTree(os);
  TYPE_LOG("{}", s);
  while (auto *d = llvm::dyn_cast_or_null<llvm::DIDerivedType>(t)) {
    auto tag = d->getTag();
    if (tag != DW_TAG_typedef && tag != DW_TAG_const_type &&
        tag != DW_TAG_volatile_type && tag != DW_TAG_restrict_type &&
        tag != DW_TAG_atomic_type)
      break;
    t = d->getBaseType();
  }
  return t;
}

llvm::Type *resolve_di_type_to_llvm(llvm::DIType *di, llvm::Module &mod) {
  using namespace llvm::dwarf;
  auto &ctx = mod.getContext();

  if (!di)
    return llvm::Type::getVoidTy(ctx);

  std::string s;
  raw_string_ostream os(s);
  di->printTree(os);
  TYPE_LOG("{}", s);
  di = peel_di_qualifiers(di);
  if (!di)
    return llvm::Type::getVoidTy(ctx);

  // function pointers
  if (auto *sr = llvm::dyn_cast<llvm::DISubroutineType>(di)) {
    TYPE_LOG("Found a function pointer\n");
    auto types = sr->getTypeArray();
    if (types.size() == 0)
      return nullptr;
    // resolve return type
    llvm::Type *ret = resolve_di_type_to_llvm(types[0], mod);
    if (!ret)
      ret = llvm::Type::getVoidTy(ctx);
    // resolve param types
    std::vector<llvm::Type *> params;
    for (unsigned i = 1; i < types.size(); ++i) {
      llvm::Type *p = resolve_di_type_to_llvm(types[i], mod);
      if (!p)
        return nullptr;
      params.push_back(p);
    }
    // create the function type signature
    return llvm::FunctionType::get(ret, params, /*isVarArg=*/false);
  }

  // TODO: maybe use something better then linear search for finding the correct
  // struct
  // for struct types and union and classes
  if (auto *comp = llvm::dyn_cast<llvm::DICompositeType>(di)) {
    auto tag = comp->getTag();
    if (tag == DW_TAG_structure_type || tag == DW_TAG_class_type ||
        tag == DW_TAG_union_type) {
      auto name = comp->getName().str();
      if (name.empty())
        return nullptr;
      // iterate each struct type defined in the module.
      for (auto *ST : mod.getIdentifiedStructTypes()) {
        if (ST->getName() == name || ST->getName() == "struct." + name ||
            ST->getName() == "union." + name ||
            ST->getName() == "class." + name)
          return ST;
      }
      return nullptr;
    }
  }

  if (auto *d = llvm::dyn_cast<llvm::DIDerivedType>(di)) {
    auto tag = d->getTag();
    if (tag == DW_TAG_pointer_type || tag == DW_TAG_reference_type ||
        tag == DW_TAG_rvalue_reference_type) {
      llvm::Type *pointee = resolve_di_type_to_llvm(d->getBaseType(), mod);
      // void* (DW_TAG_pointer_type with null base) and unresolvable pointees
      // both become i8* — matches the old C-style convention and keeps the
      // pointer wrap intact rather than degenerating to plain i8.
      if (!pointee || pointee->isVoidTy())
        pointee = llvm::Type::getInt8Ty(ctx);
      return llvm::TypedPointerType::get(pointee, 0);
    }
  }

  // get the correct basic type (int, float, short, double, long, char)
  if (auto *b = llvm::dyn_cast<llvm::DIBasicType>(di)) {
    auto bits = b->getSizeInBits();
    if (bits == 0)
      return llvm::Type::getVoidTy(ctx);
    switch (b->getEncoding()) {
    case DW_ATE_boolean:
      return llvm::Type::getInt1Ty(ctx);
    case DW_ATE_signed:
    case DW_ATE_unsigned:
    case DW_ATE_signed_char:
    case DW_ATE_unsigned_char:
    case DW_ATE_UTF:
      return llvm::IntegerType::get(ctx, bits);
    case DW_ATE_float:
      if (bits == 16)
        return llvm::Type::getHalfTy(ctx);
      if (bits == 32)
        return llvm::Type::getFloatTy(ctx);
      if (bits == 64)
        return llvm::Type::getDoubleTy(ctx);
      if (bits == 80)
        return llvm::Type::getX86_FP80Ty(ctx);
      if (bits == 128)
        return llvm::Type::getFP128Ty(ctx);
      return nullptr;
    default:
      return nullptr;
    }
  }

  return nullptr;
}

llvm::Type *infer_type_from_forward_uses(const llvm::Value *param) {
  llvm::Type *best_struct = nullptr;
  llvm::Type *fallback = nullptr;
  for (const llvm::User *U : param->users()) {
    if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(U)) {
      if (gep->getPointerOperand() != param)
        continue;
      llvm::Type *src = gep->getSourceElementType();
      std::string str;
      raw_string_ostream os(str);
      src->print(os);
      TYPE_LOG("Found source element type: {}\n", str);
      if (src && src->isStructTy()) {
        if (!best_struct)
          best_struct = src;
      } else if (!fallback) {
        fallback = src;
      }
    } else if (auto *ld = llvm::dyn_cast<llvm::LoadInst>(U)) {
      if (ld->getPointerOperand() == param && !fallback)
        fallback = ld->getType();
    } else if (auto *st = llvm::dyn_cast<llvm::StoreInst>(U)) {
      if (st->getPointerOperand() == param && !fallback)
        fallback = st->getValueOperand()->getType();
    }
  }
  return best_struct ? best_struct : fallback;
}

std::string getCacheDomFile(std::string fun_name) {
  return config_t::instance()->cache_folder + "/" +
         computeHash(config_t::instance()->input_filename) + "_" + fun_name +
         "_dom.txt";
}

std::string getCachePostDomFile(std::string fun_name) {
  return config_t::instance()->cache_folder + "/" +
         computeHash(config_t::instance()->input_filename) + "_" + fun_name +
         "_postdom.txt";
}
// OLD TEST -- KEPT FOR REFERENCE
void testDom2(liberator::FunctionConditions *fun_conds, IBBGraph *ibbg) {

  std::set<const ICFGNode *> cond_nodes;

  int num_param = fun_conds->getParameterNum();

  for (int p = 0; p < num_param; p++) {
    liberator::ValueMetadata meta = fun_conds->getParameterMetadata(p);
    for (auto i : meta.getAccessTypeSet()->getAllICFGNodes())
      cond_nodes.insert(i);
  }

  liberator::ValueMetadata meta = fun_conds->getReturnMetadata();
  for (auto i : meta.getAccessTypeSet()->getAllICFGNodes())
    cond_nodes.insert(i);

  SVFUtil::outs() << "[DEBUG] All nodes from ATS: " << cond_nodes.size()
                  << "\n";

  std::set<const ICFGNode *> not_found;
  for (auto i : cond_nodes) {
    if (ibbg->getIBBNode(i->getId()) == nullptr)
      not_found.insert(i);
  }

  SVFUtil::outs() << "[DEBUG] Not found " << not_found.size() << "\n";
  // std::set<const SVFFunction*> funs;
  // for (auto i: not_found) {
  //     // SVFUtil::outs() << i->toString() << "\n";
  //     funs.insert(i->getFun());
  // }

  // for (auto f: funs)
  //     SVFUtil::outs() << f->getName() << "\n";
  // exit(1);
}

// OLD TEST -- KEPT FOR REFERENCE
// void testDom(Dominator* dom, IBBGraph* ibbg) {

//     // diff between dom and ibbg nodes

//     // print the diff

//     // find common subset (if any) and check if the 2 doms are coherent

//     SVFUtil::outs() << "[DOING DOM TESTING]\n";

//     // std::set<ICFGNode*> all_nodes = dom->getRelevantNodes();
//     IBBGraph::NodeIDSet all_nodes_id = ibbg->getNodeIdAllocated();
//     std::set<ICFGNode*> all_nodes;
//     for (auto i: all_nodes_id)
//         all_nodes.insert(ibbg->getICFG()->getICFGNode(i));

//     unsigned ok_nodes = 0;

//     srand (time(NULL));

//     unsigned MAX_TEST = 10000;
//     for (int i = 0; i < MAX_TEST; i++) {
//         auto n = rand() % all_nodes.size();
//         auto it = std::begin(all_nodes);
//         std::advance(it,n);
//         auto node1 = *it;

//         n = rand() % all_nodes.size();
//         it = std::begin(all_nodes);
//         std::advance(it,n);
//         auto node2 = *it;

//         if (SVFUtil::isa<FunEntryICFGNode>(node1) ||
//             SVFUtil::isa<FunEntryICFGNode>(node2))
//             continue;

//         SVFUtil::outs() << "[INFO] TEST " << i << "/" << MAX_TEST << ")\r";

//         bool n1_d_n2_a = dom->dominates(node1, node2);
//         bool n2_d_n1_a = dom->dominates(node2, node1);
//         bool n1_d_n2_b = ibbg->dominates(node1, node2);
//         bool n2_d_n1_b = ibbg->dominates(node2, node1);

//         if (n1_d_n2_a == n1_d_n2_b && n2_d_n1_a == n2_d_n1_b)
//             ok_nodes++;
//         else {
//             SVFUtil::outs() << "\n";
//             SVFUtil::outs() << "Node1: " << node1->toString() << "\n";
//             SVFUtil::outs() << "Node2: " << node2->toString() << "\n";
//             SVFUtil::outs() << "n1_d_n2_a" << n1_d_n2_a << "\n";
//             SVFUtil::outs() << "n2_d_n1_a" << n2_d_n1_a << "\n";
//             SVFUtil::outs() << "n1_d_n2_b" << n1_d_n2_b << "\n";
//             SVFUtil::outs() << "n2_d_n1_b" << n2_d_n1_b << "\n";
//             SVFUtil::outs() << "early stop!\n";
//             exit(1);

//         }
//     }

//     SVFUtil::outs() << \n";
//     SVFUtil::outs() << "OK nodes: " << ok_nodes << "\n";
//     SVFUtil::outs() << "exit for debug\n";
//     exit(1);
// }
bool is_fuzzing_compatible(llvm::StructType *st) {

  for (auto el : st->elements())
    if (el->isPointerTy())
      return false;

  return true;
}

// at1 over_dom at2 iif
// for each i2 in at2 . exists i1 in at1 s.t. i1 dom i2
bool dominatesAccessType(GenericDominatorTy *dom, liberator::AccessType at1,
                         liberator::AccessType at2) {

  uint n_instr_dominated = 0;
  bool is_dominated;
  for (auto i2 : at2.getICFGNodes()) {
    is_dominated = false;
    for (auto i1 : at1.getICFGNodes()) {
      is_dominated |= dom->dominates(const_cast<ICFGNode *>(i1),
                                     const_cast<ICFGNode *>(i2));
      if (is_dominated)
        break;
    }
    n_instr_dominated += is_dominated ? 1 : 0;
  }

  return n_instr_dominated == at2.getICFGNodes().size();
}

void pruneAccessTypes(Dominator *dom, PostDominator *pDom,
                      liberator::ValueMetadata *meta) {

  // the pair is meant to be <CREATE, DELETE>, not the other way around
  std::set<std::pair<liberator::AccessType, liberator::AccessType>>
      pairs_create_delete;
  for (auto at1 : *meta->getAccessTypeSet()) {
    if (at1.get_kind() != liberator::AccessType::kind_e::create &&
        at1.get_kind() != liberator::AccessType::kind_e::del)
      continue;

    for (auto at2 : *meta->getAccessTypeSet()) {
      if (at2.get_kind() != liberator::AccessType::kind_e::create &&
          at2.get_kind() != liberator::AccessType::kind_e::del)
        continue;

      if (at1 == at2)
        continue;

      if (at1.get_fields() == at2.get_fields())
        // be sure create comes first
        if (at1.get_kind() == liberator::AccessType::kind_e::create)
          pairs_create_delete.insert(std::make_pair(at1, at2));
    }
  }

  for (auto px : pairs_create_delete)
    // (delete, X) PostDom (create, X) => None *remove both*
    if (dominatesAccessType(pDom, px.second, px.first)) {
      meta->getAccessTypeSet()->remove(px.first);
      meta->getAccessTypeSet()->remove(px.second);
      // (create, X) Dom (delete, X) => (create, X)
    } else if (dominatesAccessType(dom, px.first, px.second))
      meta->getAccessTypeSet()->remove(px.second);

  // the pair is meant to be <WRITE, READ>, not the other way around
  std::set<std::pair<liberator::AccessType, liberator::AccessType>>
      pairs_write_read;
  for (auto at1 : *meta->getAccessTypeSet()) {
    if (at1.get_kind() != liberator::AccessType::kind_e::write &&
        at1.get_kind() != liberator::AccessType::kind_e::read)
      continue;

    for (auto at2 : *meta->getAccessTypeSet()) {
      if (at2.get_kind() != liberator::AccessType::kind_e::write &&
          at2.get_kind() != liberator::AccessType::kind_e::read)
        continue;

      if (at1 == at2)
        continue;

      if (at1.get_fields() == at2.get_fields())
        // be sure write comes first
        if (at1.get_kind() == liberator::AccessType::kind_e::write)
          pairs_write_read.insert(std::make_pair(at1, at2));
    }
  }

  for (auto px : pairs_write_read)
    // (write, X) Dom (read, X) => (write, X)
    if (dominatesAccessType(dom, px.first, px.second))
      meta->getAccessTypeSet()->remove(px.second);
}

// bool thereIsCache(std::string fun_name) {
//     std::string cache_dom = getCacheDomFile(fun_name);
//     std::string cache_postdom = getCachePostDomFile(fun_name);
//     return exists_file(cache_dom) && exists_file(cache_postdom);
// }
} // namespace

namespace liberator {

condition_extractor_t::condition_extractor_t(
    const std::set<std::string> &&functions, Module *module,
    unique_ptr<SVFGBuilder> svfg, SVFIR *pag,
    GlobalStruct *point_to_analyses) noexcept
    : functions_(functions), module_(module), pag_(pag),
      svfg_builder_(std::move(svfg)), point_to_analyses_(point_to_analyses) {}

// Loading the LLVM IR
// Generating the SVF Graphs
//
unique_ptr<condition_extractor_t>
make_condition_extractor(std::vector<std::string> &module_name_vec,
                         const std::set<string> &functions) {
  bool all_functions = true;
  std::string function;
  auto config = config_t::instance();
  if (!config->function.empty()) {
    all_functions = false;
    function = config->function;
  }

  if (Options::WriteAnder() == "ir_annotator") {
    LLVMModuleSet::getLLVMModuleSet()->preProcessBCs(module_name_vec);
  }

  SVFUtil::outs() << "[INFO] Loading library...\n";

  LLVMModuleSet *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
  for (auto &str : module_name_vec) {
    if (!std::filesystem::exists(str)) {
      SVFUtil::errs() << "File " << str << " could not be found.\n";
      return nullptr;
    }
  }

  {
    PROFILE_SCOPE("1. LLVM Module Build");
    llvmModuleSet->buildSVFModule(module_name_vec);
  }
  auto *svfModule = &llvmModuleSet->getLLVMModules().front().get();

  SVFUtil::outs() << "[INFO] Done\n";

  // I extract all the function names from the LLVM module
  auto functions_llvm =
      svfModule->functions() |
      views::transform([](auto &F) { return F.getName().str(); }) |
      ranges::to<set<string>>();

  std::set<string> all_funcs;
  ranges::set_intersection(functions_llvm, functions,
                           inserter(all_funcs, all_funcs.begin()));

  // std::vector<std::string> functions;
  // Set of functions that are in the apis_clang.json

  if (config->output_type == OutType::stdo)
    SVFUtil::outs() << "[WARNING] outputting in stdout, ignoring OutputFile\n";

  // Dump LLVM apis function per function
  for (const Function &F : *svfModule) {
    function_record my_fun;
    const auto *DL = &svfModule->getDataLayout();

    Type *retType = F.getReturnType();
    StringRef function_name = F.getName();
    bool is_vararg = F.isVarArg();

    SVFUtil::errs() << "Doing: " << function_name.str() << "\n";

    my_fun.function_name = function_name.str();
    my_fun.is_vararg = is_vararg ? "true" : "false";
    my_fun.return_info.set_from_type(retType);
    my_fun.return_info.size = estimate_size(retType, DL);
    my_fun.return_info.name = "return";

    for (const auto &arg : F.args()) {
      argument_record an_argument;
      an_argument.set_from_argument(arg);
      an_argument.size = estimate_size(&arg, DL);
      my_fun.arguments_info.push_back(an_argument);
    }

    auto api_logger = *logger_manager_t::instance()->logger("llvm.json");
    api_logger->info("{}\n", my_fun.to_json());
    // libfuzz::dumpApiInfo(my_fun);
  }
  /// Build Program Assignment Graph (SVFIR)
  SVFIRBuilder ir_builder;
  PAG *pag = nullptr;
  {
    PROFILE_SCOPE("2. PAG Builder");
    pag = ir_builder.build();
  }
  ICFG *icfg = pag->getICFG();
  /// Create Andersen's pointer analysis
  // Andersen* point_to_analysys =
  // AndersenWaveDiff::createAndersenWaveDiff(pag); FlowSensitive*
  // point_to_analysys = FlowSensitive::createFSWPA(pag); AndersenSCD*
  // point_to_analysys = AndersenSCD::createAndersenSCD(pag); TypeAnalysis*
  // point_to_analysys = new TypeAnalysis(pag);
  auto point_to_analyses = GlobalStruct::createSGWPA(pag);
  {
    PROFILE_SCOPE("3. Points-to Analysis");
    point_to_analyses->analyze();
  }
  SVFUtil::outs() << "[INFO] Points-to analysis done!\n";

  GlobalStruct::CallEdgeMap newEdges = point_to_analyses->get_new_edges();
  // NOTE: copy callsite->target relation in a neutral structure
  for (auto x : newEdges) {
    // callsite
    auto cs = x.first;
    // callee
    for (auto t : x.second) {
      // typedef std::map<const CallICFGNode *, std::set<const FunObjVar *>>
      APARM_LOG("Found an indirect call in {} to {}",
                t->getICFGNode()->getName(), t->getName());
      ValueMetadata::myCallEdgeMap_inst[cs].insert(t);
    }
  }

  // update both callgraphs with added new edges from GlobalStruct::analyze
  CallGraph *callgraph = point_to_analyses->getCallGraph();
  ir_builder.updateCallGraph(callgraph);
  icfg->updateCallGraph(callgraph);
  // icfg->dump("icfg_extractor");
  /// Sparse value-flow graph (SVFG)
  auto svfBuilder = make_unique<SVFGBuilder>();
  SVFG *svfg = nullptr;
  {
    PROFILE_SCOPE("4. Full SVFG Build");
    svfg = svfBuilder->buildFullSVFG(point_to_analyses);
    svfg->updateCallGraph(point_to_analyses);
  }

  // svfg->dump("from_extractor");

  if (config->dump_svfg) {
    svfg->dump("my_graph");
  }

  // I want to find a minimized set of APIs to analyze
  if (config->minimize_api != "") {
    std::set<std::string> minimize_functions;

    SVF::Module::const_iterator it = svfModule->begin();
    SVF::Module::const_iterator eit = svfModule->end();
    for (; it != eit; ++it) {
      const Function &fun = *it;
      std::string fun_name = fun.getName().str();
      if (all_funcs.find(fun_name) != all_funcs.end()) {
        const CallGraphNode *cg_node = callgraph->getCallGraphNode(fun_name);

        bool no_direct_in_edge = true;

        auto it2 = cg_node->directInEdgeBegin();
        auto eit2 = cg_node->directInEdgeEnd();

        if (cg_node->directInEdgeBegin() != cg_node->directInEdgeEnd())
          no_direct_in_edge = false;
        /*for (; it2 != eit2; it2++) {
          no_direct_in_edge = false;
          break;
        }*/

        if (no_direct_in_edge)
          minimize_functions.insert(fun_name);
      }
    }

    // SVFUtil::outs() << "[INFO] The minimize set of function\n";
    std::ofstream minimizeApiFile(config->minimize_api);
    for (auto f : minimize_functions) {
      minimizeApiFile << f << "\n";
    }
    minimizeApiFile.close();
    // SVFUtil::outs() << "[INFO] All function\n";
    // for (auto f: functions)
    //     SVFUtil::outs() << f << "\n";

    // SVFUtil::outs() << "[INFO] Total: " << minimize_functions.size() << "\n";
    // SVFUtil::outs() << "[INFO] Original: " << functions.size() << "\n";
  }

  return std::unique_ptr<condition_extractor_t>(
      new condition_extractor_t(std::move(all_funcs), svfModule,
                                std::move(svfBuilder), pag, point_to_analyses));
}

void type_induction_by_pts(PAG *pag, SVFVar *param) {
  SVF::Andersen *ander = SVF::AndersenWaveDiff::createAndersenWaveDiff(pag);
  const SVF::PointsTo pts = ander->getPts(param->getId());
  TYPE_LOG("Parameter {} with id {} points-to {} objects.\n", param->getName(),
           param->getId(), pts.count());

  for (auto target : pts) {
    auto base = pag->getBaseObject(target);
    if (!base)
      continue;
    TYPE_LOG("Parameter with id: {} - points to {} with type {}\n",
             param->getId(), base->getName(), base->getType()->toString());
  }
}

void type_induction_by_use_def(llvm::Value *param) {
  if (const auto *arg = SVFUtil::dyn_cast<llvm::Argument>(param)) {
    for (const llvm::User *user : arg->users()) {
      if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
        llvm::Type *src_type = gep->getSourceElementType();
        if (src_type->isStructTy()) {
          TYPE_LOG("Parameter {} is actually struct: {}\n",
                   param->getName().str(), src_type->getStructName().str());
        }
      }
    }
  }
}

std::string get_full_type(llvm::DIType *Ty) {
  if (!Ty)
    return "void";

  if (!Ty->getName().empty()) {
    return Ty->getName().str();
  }

  if (auto *derivedTy = llvm::dyn_cast<llvm::DIDerivedType>(Ty)) {
    if (derivedTy->getTag() == llvm::dwarf::DW_TAG_pointer_type) {
      return get_full_type(derivedTy->getBaseType()) + "*";
    }
    if (derivedTy->getTag() == llvm::dwarf::DW_TAG_const_type) {
      return "const " + get_full_type(derivedTy->getBaseType());
    }

    return get_full_type(derivedTy->getBaseType());
  }

  return "unknown";
}

void get_function_metadata(const llvm::Function &F) {
  if (llvm::DISubprogram *SP = F.getSubprogram()) {
    llvm::DISubroutineType *STy = SP->getType();
    llvm::DITypeRefArray typeArray = STy->getTypeArray();
    for (unsigned i = 0; i < typeArray.size(); ++i) {
      if (auto *Ty = typeArray[i]) {
        TYPE_LOG("Type Name: {}\n", get_full_type(Ty));
      }
    }
  }
}

function_condition_set_t condition_extractor_t::extract_function_conditions() {
  PROFILE_SCOPE("Total Extraction");
  auto llvm_module_set = LLVMModuleSet::getLLVMModuleSet();
  auto *svfModule = &llvm_module_set->getLLVMModules().front().get();
  function_condition_set_t result;

  auto pag = SVFIR::getPAG();
  // list to all formal parameters
  auto fun_param_map = pag->getFunArgsMap();
  // list to all returns
  auto ret_param_map = pag->getFunRets();
  // control flow graph
  ICFG *icfg = pag->getICFG();
  unsigned int tot_function = functions_.size();
  unsigned int num_function = 0;
  auto svfg = svfg_builder_->getSVFG();

  for (auto f : functions_) {
    int current_index = num_function++;

    if (config_t::instance()->range_start != -1 &&
        current_index < config_t::instance()->range_start) {
      continue;
    }
    if (config_t::instance()->range_end != -1 &&
        current_index >= config_t::instance()->range_end) {
      continue;
    }

    PROFILE_SCOPE("Process Function: " + f);
    FunctionConditions fun_conds;

    const string prog =
        std::to_string(current_index) + "/" + std::to_string(tot_function);
    SVFUtil::outs() << "[INFO " << prog << "] processing return for " << f
                    << "\n";

    fun_conds.setFunctionName(f);
    get_function_metadata(*llvm_module_set->getFunction(f));

    {
      PROFILE_SCOPE("Process Parameters: " + f);
      // Look up the specific function object instead of iterating all

      std::vector<std::string> set_by_vect;
      auto svf_fun = pag->getFunObjVar(f);
      ValueMetadata param_metadata;
      for (auto param : fun_param_map[svf_fun]) {
        /**
         * First try SVF's interprocedural type inference. Only works if the
         * library is called in the program and it can trace back the origin to
         * an alloc or malloc call.
         */
        auto formal_param_llvm = llvm_module_set->getLLVMValue(param);
        auto type_inference = llvm_module_set->getTypeInference();
        auto seek_type = type_inference->inferObjType(formal_param_llvm);

        /**
         * Fallback chain when SVF returned an opaque pointer:
         *   1. LLVM Argument attributes (byval/sret/elementtype/...)
         *   2. DWARF debug info
         *   3. Intra-procedural forward scan over GEP / load / store uses
         */
        if (seek_type->isPointerTy()) {
          auto *arg = llvm::dyn_cast<llvm::Argument>(formal_param_llvm);

          if (arg) {
            TYPE_LOG("Argument name: {}\n", arg->getName());
            if (auto *attr_type = infer_type_from_arg_attrs(arg)) {
              TYPE_LOG("Recovered pointee type from Argument attribute.\n");
              seek_type = attr_type;
            }
          }

          if (seek_type->isPointerTy() && arg) {
            TYPE_LOG("Couldn't deduce pointer type using SVF or attributes.\n "
                     "Falling back to DWARF.\n");
            if (DISubprogram *SP = arg->getParent()->getSubprogram()) {
              if (DISubroutineType *STy = SP->getType()) {
                DITypeRefArray type_array = STy->getTypeArray();

                unsigned array_index = arg->getArgNo() + 1;
                if (array_index < type_array.size() &&
                    type_array[array_index]) {
                  auto *param_di_type = type_array[array_index];

                  // Resolve the entire DI chain — every pointer/reference
                  // node becomes a TypedPointerType wrapping its (resolved)
                  // pointee, so the full source-level type is preserved.
                  // Examples:
                  //   int **first  → i32**
                  //   cb_t c       → (i32 (i8*))*           (function ptr)
                  //   cb1_t *c     → (i32 (i32*, i8*, float))**
                  //   PointPtr p   → %struct.Point*
                  auto *llvm_module = llvm_module_set->getMainLLVMModule();
                  if (auto *resolved = resolve_di_type_to_llvm(param_di_type,
                                                               *llvm_module)) {
                    if (!resolved->isVoidTy()) {
                      TYPE_LOG("Resolved DWARF source type.\n");
                      seek_type = resolved;
                    }
                  }
                }
              }
            }
          }

          if (seek_type->isPointerTy()) {
            if (auto *use_type =
                    infer_type_from_forward_uses(formal_param_llvm)) {
              TYPE_LOG("Recovered pointee type from forward use scan.\n");
              seek_type = use_type;
            }
          }
        } else {
          TYPE_LOG("SVF type inference found type!\n");
        }
        std::string type_str;
        raw_string_ostream os(type_str);
        seek_type->print(os);
        TYPE_LOG("For parameter: {} in function: {}, we inferred type {}\n",
                 param->toString(), svf_fun->toString(), type_str);

        {
          PROFILE_SCOPE("Function 1: extractParameterMetadata");
          PROFILE_SCOPE("Function 1: extractParameterMetadata: " + f);
          param_metadata = extractParameterMetadata(*svfg, formal_param_llvm,
                                                    seek_type, param->getId());
        }

        if (param_metadata.isArray()) {
          std::string depends_on;
          {
            PROFILE_SCOPE("Function 2: extractLenDependencyParameter");
            PROFILE_SCOPE("Function 2: extractLenDependencyParameter: " + f);
            depends_on = extractLenDependencyParameter(param, param_metadata,
                                                       *svfg, svf_fun);
          }
          if (depends_on.empty())
            param_metadata.setLenDependency(depends_on);
        }

        {
          PROFILE_SCOPE("Function 3: extractDependencyAmongParam");
          PROFILE_SCOPE("Function 3: extractDependencyAmongParam: " + f);
          set_by_vect = extractDependencyAmongParameters(param, param_metadata,
                                                         *svfg, svf_fun);
        }
      }

      for (auto &d : set_by_vect) {
        param_metadata.addSetByDependency(d);
      }

      fun_conds.addParameterMetadata(param_metadata);
    }

    {
      PROFILE_SCOPE("Process Return " + f);
      for (auto r : ret_param_map) {
        const FunObjVar *fun = r.first;
        if (fun->getName() != f) {
          continue;
        }

        const SVFVar *p = r.second;
        const Value *llvm_value = llvm_module_set->getLLVMValue(p);
        auto return_metadata = extractReturnMetadata(*svfg, llvm_value);

        fun_conds.setReturnMetadata(return_metadata);
      }
    }

    if (config_t::instance()->use_dominator) {
      PROFILE_SCOPE("Process Dominators");
      SVF::Module::const_iterator it = svfModule->begin();
      SVF::Module::const_iterator eit = svfModule->end();
      for (; it != eit; ++it) {
        const Function &fun = *it;
        if (fun.getName() != f)
          continue;

        if (fun.isDeclaration())
          continue;

        std::string fun_name = fun.getName().str();

        DOMINATOR_LOG("computing dominators for: {}\n", fun_name);

        auto svf_fun = pag->getFunObjVar(fun_name);
        FunEntryICFGNode *fun_entry = icfg->getFunEntryICFGNode(svf_fun);
        FunExitICFGNode *fun_exit = icfg->getFunExitICFGNode(svf_fun);

        std::string dom_cache_file = getCacheDomFile(fun_name);
        std::string postdom_cache_file = getCachePostDomFile(fun_name);

        auto dom = make_unique<Dominator>(
            point_to_analyses_, fun_entry,
            config_t::instance()->consider_indirect_calls);
        // SVFUtil::outs() << "[INFO] Running pruneUnreachableFunctions()\n";
        // dom->pruneUnreachableFunctions();
        // SVFUtil::outs() << "[INFO] Running buildPhiFun()\n";
        // dom->buildPhiFun();
        // SVFUtil::outs() << "[INFO] Running inferSubGraph()\n";
        // dom->inferSubGraph();
        if (config_t::instance()->cache_folder != "" &&
            std::filesystem::exists(dom_cache_file)) {
          SVFUtil::outs() << "[INFO] There is DOM cache, loading it\n";
          dom->loadDom(dom_cache_file);
        } else {
          PROFILE_SCOPE("Create Dominator");
          SVFUtil::outs()
              << "[INFO] No DOM cache, computing from scratch and save\n";
          auto begin = chrono::high_resolution_clock::now();
          dom->createDom();
          auto end = chrono::high_resolution_clock::now();
          auto dur = end - begin;
          auto min =
              std::chrono::duration_cast<std::chrono::minutes>(dur).count();
          SVFUtil::outs() << "[TIME] Dom: " << min << "min\n";
          // dom->saveIBBGraph("ibbgraph_2");

          if (config_t::instance()->cache_folder != "")
            dom->dumpDom(dom_cache_file);
        }

        auto pDom = make_unique<PostDominator>(
            point_to_analyses_, fun_entry, fun_exit,
            config_t::instance()->consider_indirect_calls);

        if (config_t::instance()->cache_folder != "" &&
            std::filesystem::exists(postdom_cache_file)) {
          SVFUtil::outs() << "[INFO] There is POSTDOM cache, loading it\n";
          pDom->loadDom(postdom_cache_file);
        } else {
          PROFILE_SCOPE("Create PostDominator");
          SVFUtil::outs()
              << "[INFO] No POSTDOM cache, computing from scratch and save\n";
          auto begin = chrono::high_resolution_clock::now();
          pDom->createDom();
          auto end = chrono::high_resolution_clock::now();
          auto dur = end - begin;
          auto min =
              std::chrono::duration_cast<std::chrono::minutes>(dur).count();
          SVFUtil::outs() << "[TIME] Postdom: " << min << "min\n";
          // pDom->saveIBBGraph("ibbgraph_3");

          if (config_t::instance()->use_dominator)
            pDom->dumpDom(postdom_cache_file);
        }

        if (config_t::instance()->print_dominator) {
          SVFUtil::outs() << "[INFO] dumping dominators...\n";
          std::string str1, str2;
          if (dom) {
            dom->dumpTransRed("./" + dom_cache_file);
          }

          if (pDom) {
            pDom->dumpTransRed("./" + postdom_cache_file);
          }
        }

        int num_param = fun_conds.getParameterNum();

        for (int p = 0; p < num_param; p++) {
          ValueMetadata meta = fun_conds.getParameterMetadata(p);
          pruneAccessTypes(dom.get(), pDom.get(), &meta);
          fun_conds.replaceParameterMetadata(p, meta);
        }

        ValueMetadata meta = fun_conds.getReturnMetadata();
        pruneAccessTypes(dom.get(), pDom.get(), &meta);
        fun_conds.setReturnMetadata(meta);
      }
    }
    result.insert({fun_conds.getFunctionName(), fun_conds});
  }
  return result;
}

void save_llvm_data_layout() {
  // extract data layout
  SVFUtil::outs() << "\n[INFO] extract structs data layout!\n";

  Module *m = LLVMModuleSet::getLLVMModuleSet()->getMainLLVMModule();
  const DataLayout &data_layout = m->getDataLayout();

  ofstream fw(config_t::instance()->extract_data_layout, std::ofstream::out);
  if (fw.is_open()) {
    for (auto st : m->getIdentifiedStructTypes()) {
      fw << st->getName().str() << " ";
      if (st->isSized()) {
        uint64_t storeSize = data_layout.getTypeStoreSizeInBits(st);
        fw << storeSize << " ";
        fw << TypeMatcher::compute_hash(st) << " ";
        fw << is_fuzzing_compatible(st) << "\n";
      } else {
        fw << "0 <random> 0\n";
      }
    }
    fw.close();

    SVFUtil::outs() << "[INFO] struct data layout done!\n";
  }
}

condition_extractor_t::~condition_extractor_t() noexcept {
  AndersenWaveDiff::releaseAndersenWaveDiff();
  SVFIR::releaseSVFIR();

  // I am not sure I need this
  // LLVMModuleSet::getLLVMModuleSet()->dumpModulesToFile(".svf.bc");
  SVF::LLVMModuleSet::releaseLLVMModuleSet();
  // llvm::llvm_shutdown(); // This breaks testing since it destroys LLVM
  // parsing state globally
}
} // namespace liberator
