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
#include <memory>
#include <ranges>
#include <utility>

using namespace SVF;
using namespace SVFUtil;
using namespace LLVMUtil;

namespace {
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

  llvmModuleSet->buildSVFModule(module_name_vec);
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
  PAG *pag = ir_builder.build();
  ICFG *icfg = pag->getICFG();
  /// Create Andersen's pointer analysis
  // Andersen* point_to_analysys =
  // AndersenWaveDiff::createAndersenWaveDiff(pag); FlowSensitive*
  // point_to_analysys = FlowSensitive::createFSWPA(pag); AndersenSCD*
  // point_to_analysys = AndersenSCD::createAndersenSCD(pag); TypeAnalysis*
  // point_to_analysys = new TypeAnalysis(pag);
  auto point_to_analyses = GlobalStruct::createSGWPA(pag);
  point_to_analyses->analyze();
  SVFUtil::outs() << "[INFO] Points-to analysis done!\n";

  GlobalStruct::CallEdgeMap newEdges = point_to_analyses->get_new_edges();
  // NOTE: copy callsite->target relation in a neutral structure
  for (auto x : newEdges) {
    // callsite
    auto cs = x.first;
    // callee
    for (auto t : x.second)
      // typedef std::map<const CallICFGNode *, std::set<const FunObjVar *>>
      ValueMetadata::myCallEdgeMap_inst[cs].insert(t);
  }

  // update both callgraphs with added new edges from GlobalStruct::analyze
  CallGraph *callgraph = point_to_analyses->getCallGraph();
  ir_builder.updateCallGraph(callgraph);
  icfg->updateCallGraph(callgraph);

  // icfg->dump("icfg_extractor");

  /// Sparse value-flow graph (SVFG)
  auto svfBuilder = make_unique<SVFGBuilder>();
  auto svfg = svfBuilder->buildFullSVFG(point_to_analyses);
  svfg->updateCallGraph(point_to_analyses);

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

function_condition_set_t condition_extractor_t::extract_function_conditions() {
  ScopedTimer t_total("Total Extraction");
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
    ScopedTimer t_func("Process Function: " + f);
    FunctionConditions fun_conds;

    const string prog =
        std::to_string(num_function++) + "/" + std::to_string(tot_function);
    SVFUtil::outs() << "[INFO " << prog << "] processing return for " << f
                    << "\n";

    fun_conds.setFunctionName(f);

    {
      ScopedTimer t_params("Process Parameters");
      // Look up the specific function object instead of iterating all
      auto svf_fun = pag->getFunObjVar(f);
      if (svf_fun && fun_param_map.find(svf_fun) != fun_param_map.end()) {
        const auto &params = fun_param_map[svf_fun];
        for (const auto param : params) {
          if (config_t::instance()->verbose >= Verbosity::v1)
            SVFUtil::outs() << "[INFO] param: " << param->toString() << "\n";

          auto formal_param_llvm = llvm_module_set->getLLVMValue(param);
          auto seek_type = formal_param_llvm->getType();

          ValueMetadata param_metadata = extractParameterMetadata(
              *svfg, formal_param_llvm, seek_type, param->getId());

          if (param_metadata.isArray()) {
            auto depends_on = extractLenDependencyParameter(
                param, param_metadata, *svfg, svf_fun);
            if (depends_on.empty())
              param_metadata.setLenDependency(depends_on);
          }

          auto set_by_vect = extractDependencyAmongParameters(
              param, param_metadata, *svfg, svf_fun);

          for (auto &d : set_by_vect) {
            param_metadata.addSetByDependency(d);
          }

          fun_conds.addParameterMetadata(param_metadata);
        }
      }
    }

    {
      ScopedTimer t_ret("Process Return");
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
      ScopedTimer t_doms("Process Dominators");
      SVF::Module::const_iterator it = svfModule->begin();
      SVF::Module::const_iterator eit = svfModule->end();
      for (; it != eit; ++it) {
        const Function &fun = *it;
        if (fun.getName() != f)
          continue;

        if (fun.isDeclaration())
          continue;

        std::string fun_name = fun.getName().str();

        SVFUtil::outs() << "[INFO] computing dominators for: " << fun_name
                        << "\n";

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
          ScopedTimer t_dom_create("Create Dominator");
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
          ScopedTimer t_postdom_create("Create PostDominator");
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
  llvm::llvm_shutdown();
}
} // namespace liberator
