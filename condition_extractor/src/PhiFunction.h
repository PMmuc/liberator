
#ifndef INCLUDE_PHI_FUNCTION_H_
#define INCLUDE_PHI_FUNCTION_H_

#include "Graphs/ICFG.h"
#include "Graphs/SVFG.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "WPA/Andersen.h"
#include <Graphs/GenericGraph.h>

using namespace SVF;
using namespace llvm;
using namespace std;

typedef std::map<CallCFGEdge *, RetCFGEdge *> PHIFun;
typedef std::map<RetCFGEdge *, CallCFGEdge *> PHIFunInv;

void getPhiFunction(Module *, ICFG *, PHIFun *, PHIFunInv *);

#endif
