#ifndef GLOBALSTRUCTANALYSIS_H_
#define GLOBALSTRUCTANALYSIS_H_

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

/// Base pointer analysis backing GlobalStruct.
///
/// FlowSensitive::initialize() builds a PTR-only SVFG plus its MemSSA and then
/// keeps per-program-point points-to state for every node in it. On large
/// targets that dominates the whole run's memory. The only consumer of the
/// flow-sensitive result in this codebase is the `getPts(...).empty()` test in
/// GlobalStruct::analyze(); the two sites that read points-to contents
/// (AccessTypeHandler.cpp, ConditionExtractor.cpp) already ask Andersen.
/// AndersenBase::initialize() only builds a ConstraintGraph - no SVFG.
using GlobalStructPTA = AndersenWaveDiff;

class GlobalStruct : public GlobalStructPTA {

public:
  /// Constructor
  explicit GlobalStruct(SVFIR *_pag) : GlobalStructPTA(_pag) {}

  /// Destructor
  ~GlobalStruct() override = default;

  /// Create single instance of flow-sensitive pointer analysis.
  /// Note: this only constructs the instance. The caller must invoke analyze()
  /// exactly once. Calling it here as well made FlowSensitive::analyze() run
  /// twice, and it has no guard - it re-runs initialize(), which rebuilds the
  /// PTR-only SVFG and MemSSA from scratch, then re-solves the fixpoint.
  static GlobalStruct *createSGWPA(SVFIR *_pag) {
    if (gspta == nullptr) {
      // FIXME: creates flow sensitive points-to analysis
      gspta = std::unique_ptr<GlobalStruct>(new GlobalStruct(_pag));
    }
    return gspta.get();
  }

  /// Release flow-sensitive pointer analysis
  static void releaseFSWPA() { gspta = nullptr; }

  /// We start from here
  virtual bool runOnModule(llvm::Module *) { return false; }

  /// GlobalStruct analysis
  void analyze() override;

  /**
   * Build the AndersenConstraintGraph
   */
  void initialize() override;

  /// Finalize analysis
  void finalize() override;

  CallEdgeMap get_new_edges() { return new_edges; }

protected:
  static std::unique_ptr<GlobalStruct> gspta;

  CallEdgeMap new_edges;

  void get_function_pointers(
      const llvm::Value *,
      std::map<std::string, std::set<const llvm::Function *>> &);
};

#endif /* GLOBALSTRUCTANALYSIS_H_ */
