#ifndef GLOBALSTRUCTANALYSIS_H_
#define GLOBALSTRUCTANALYSIS_H_

#include "Config.h"
#include "FastCluster/fastcluster.h"
#include "Graphs/SVFGOPT.h"
#include "MSSA/SVFGBuilder.h"
#include "MemoryModel/PointerAnalysis.h"
#include "MemoryModel/PointerAnalysisImpl.h"
#include "WPA/WPAFSSolver.h"
#include <WPA/Andersen.h>
#include <WPA/FlowSensitive.h>
#include <llvm/IR/Value.h>

using namespace SVF;
using namespace SVFUtil;

/// Runs the points-to analysis selected with -pta (config_t::pta_kind) and
/// then resolves indirect calls through function pointers in global constants
/// that it left without targets (empty points-to set).
///
/// The choice reaches further than GlobalStruct::analyze(): the full SVFG is
/// built from pta() (its points-to sets decide which memory regions every load
/// and store touches, i.e. the indirect SVFG edges), and its call graph
/// resolves indirect calls for the ICFG, the SVFG and the bottom-up SCC walk.
///  - andersen: AndersenWaveDiff. Flow-insensitive, builds only a constraint
///    graph; far cheaper, but its points-to sets can be larger.
///  - fs: FlowSensitive, as in the original condition extractor. Refines
///    Andersen with strong updates on its own PTR-only SVFG and MemSSA and
///    keeps points-to state per program point; on large targets that
///    dominates the run's time and memory.
class GlobalStruct {

public:
  typedef PointerAnalysis::CallEdgeMap CallEdgeMap;

  /// Create the single instance with the given points-to analysis. Note:
  /// this only constructs it; the caller must invoke analyze() exactly once.
  static GlobalStruct *createSGWPA(SVFIR *_pag, pta_kind_e kind) {
    if (gspta == nullptr)
      gspta = std::unique_ptr<GlobalStruct>(new GlobalStruct(_pag, kind));
    assert(gspta->kind_ == kind && "GlobalStruct already created with "
                                   "another points-to analysis");
    return gspta.get();
  }

  /// Release the analysis
  static void releaseFSWPA() { gspta = nullptr; }

  /// Run the points-to analysis, then resolve the global function pointers.
  void analyze();

  /// The points-to analysis, with the call edges analyze() added.
  BVDataPTAImpl *pta() const { return pta_.get(); }

  CallGraph *getCallGraph() const { return pta_->getCallGraph(); }

  pta_kind_e kind() const { return kind_; }

  CallEdgeMap get_new_edges() { return new_edges; }

private:
  GlobalStruct(SVFIR *_pag, pta_kind_e kind);

  static std::unique_ptr<GlobalStruct> gspta;

  std::unique_ptr<BVDataPTAImpl> pta_;
  pta_kind_e kind_;
  CallEdgeMap new_edges;

  void get_function_pointers(
      const llvm::Value *,
      std::map<std::string, std::set<const llvm::Function *>> &);
};

#endif /* GLOBALSTRUCTANALYSIS_H_ */
