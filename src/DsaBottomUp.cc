#include "sea_dsa/BottomUp.hh"

#include "llvm/ADT/SCCIterator.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include "sea_dsa/AllocWrapInfo.hh"
#include "sea_dsa/CallGraphUtils.hh"
#include "sea_dsa/CallSite.hh"
#include "sea_dsa/Cloner.hh"
#include "sea_dsa/Graph.hh"
#include "sea_dsa/Local.hh"
#include "sea_dsa/config.h"
#include "sea_dsa/support/Debug.h"

using namespace llvm;

namespace sea_dsa {
bool NoBUFlowSensitiveOpt;
}

static llvm::cl::opt<bool, true> XNoBUFlowSensitiveOpt(
    "sea-dsa-no-bu-flow-sensitive-opt",
    llvm::cl::desc("Disable partial flow sensitivity in bottom up"),
    llvm::cl::location(sea_dsa::NoBUFlowSensitiveOpt), llvm::cl::init(false),
    llvm::cl::Hidden);

namespace sea_dsa {

static const Value *findUniqueReturnValue(const Function &F) {
  const Value *onlyRetVal = nullptr;

  for (const auto &BB : F) {
    auto *TI = BB.getTerminator();
    auto *RI = dyn_cast<ReturnInst>(TI);
    if (!RI)
      continue;

    const Value *rv = RI->getOperand(0)->stripPointerCastsNoFollowAliases();
    if (onlyRetVal && onlyRetVal != rv)
      return nullptr;

    onlyRetVal = rv;
  }

  return onlyRetVal;
}

// Clone callee nodes into caller and resolve arguments
void BottomUpAnalysis::cloneAndResolveArguments(const DsaCallSite &CS,
                                                Graph &calleeG, Graph &callerG) {
  CloningContext context(*CS.getInstruction(), CloningContext::BottomUp);
  auto options = Cloner::BuildOptions(Cloner::StripAllocas);
  Cloner C(callerG, context, options);
  assert(context.m_cs);

  // clone and unify globals
  for (auto &kv : calleeG.globals()) {
    Node &calleeN = *kv.second->getNode();
    // We don't care if globals got unified together, but have to respect the
    // points-to relations introduced by the callee introduced.
#if 0
    if (!NoBUFlowSensitiveOpt)
      if (calleeN.getNumLinks() == 0 || !calleeN.isModified() ||
          llvm::isa<ConstantData>(kv.first))
        continue;
#endif

    const Value &global = *kv.first;
    Node &n = C.clone(calleeN, false, kv.first);
    Cell c(n, kv.second->getRawOffset());
    Cell &nc = callerG.mkCell(*kv.first, Cell());
    nc.unify(c);
  }

  // clone and unify return
  const Function &callee = *CS.getCallee();
  if (calleeG.hasRetCell(callee)) {
    Cell &nc = callerG.mkCell(*CS.getInstruction(), Cell());

    // Clone the return value directly, if we know that it corresponds to a
    // single allocation site (e.g., return value of a malloc, a global, etv.).
    const Value *onlyAllocSite = findUniqueReturnValue(callee);
    if (onlyAllocSite && !calleeG.hasAllocSiteForValue(*onlyAllocSite))
      onlyAllocSite = nullptr;
    if (NoBUFlowSensitiveOpt)
      onlyAllocSite = nullptr;

    const Cell &ret = calleeG.getRetCell(callee);
    Node &n = C.clone(*ret.getNode(), false, onlyAllocSite);
    Cell c(n, ret.getRawOffset());
    nc.unify(c);
  }

  // clone and unify actuals and formals
  DsaCallSite::const_actual_iterator AI = CS.actual_begin(),
                                     AE = CS.actual_end();
  for (DsaCallSite::const_formal_iterator FI = CS.formal_begin(),
                                          FE = CS.formal_end();
       FI != FE && AI != AE; ++FI, ++AI) {
    const Value *arg = (*AI).get();
    const Value *fml = &*FI;
    if (calleeG.hasCell(*fml)) {
      const Cell &formalC = calleeG.getCell(*fml);
      Node &n = C.clone(*formalC.getNode());
      Cell c(n, formalC.getRawOffset());
      Cell &nc = callerG.mkCell(*arg, Cell());
      nc.unify(c);
    }
  }

  callerG.compress();
}

bool BottomUpAnalysis::runOnModule(Module &M, GraphMap &graphs) {

  LOG("dsa-bu", errs() << "Started bottom-up analysis ... \n");

  LocalAnalysis la(m_dl, m_tli, m_allocInfo);

  for (auto it = scc_begin(&m_cg); !it.isAtEnd(); ++it) {
    auto &scc = *it;

    // -- compute a local graph shared between all functions in the scc
    GraphRef fGraph = nullptr;
    for (CallGraphNode *cgn : scc) {
      Function *fn = cgn->getFunction();
      if (!fn || fn->isDeclaration() || fn->empty())
        continue;

      if (!fGraph) {
        assert(graphs.find(fn) != graphs.end());
        fGraph = graphs[fn];
        assert(fGraph);
      }

      la.runOnFunction(*fn, *fGraph);
      graphs[fn] = fGraph;
    }

    std::vector<CallGraphNode *> cgns = call_graph_utils::SortedCGNs(scc);
    for (CallGraphNode *cgn : cgns) {
      Function *fn = cgn->getFunction();
      if (!fn || fn->isDeclaration() || fn->empty())
        continue;

      // -- resolve all function calls in the SCC
      auto callRecords = call_graph_utils::SortedCallSites(cgn);
      for (auto *callRecord : callRecords) {
        ImmutableCallSite CS(callRecord);
        DsaCallSite dsaCS(CS);
        const Function *callee = dsaCS.getCallee();
        if (!callee || callee->isDeclaration() || callee->empty())
          continue;

        assert(graphs.count(dsaCS.getCaller()) > 0);
        assert(graphs.count(dsaCS.getCallee()) > 0);

        Graph &callerG = *(graphs.find(dsaCS.getCaller())->second);
        Graph &calleeG = *(graphs.find(dsaCS.getCallee())->second);

        static int cnt = 0;
        ++cnt;
        LOG("dsa-bu", llvm::errs() << "BU #" << cnt << ": "
                                   << dsaCS.getCaller()->getName() << " <- "
                                   << dsaCS.getCallee()->getName() << "\n");
        LOG("dsa-bu", llvm::errs()
                          << "\tCallee size: " << calleeG.numNodes()
                          << ", caller size:\t" << callerG.numNodes() << "\n");
        LOG("dsa-bu", llvm::errs()
                          << "\tCallee collapsed: " << calleeG.numCollapsed()
                          << ", caller collapsed:\t" << callerG.numCollapsed()
                          << "\n");

        cloneAndResolveArguments(dsaCS, calleeG, callerG);
        LOG("dsa-bu", llvm::errs()
                          << "\tCaller size after clone: " << callerG.numNodes()
                          << ", collapsed: " << callerG.numCollapsed() << "\n");
      }
    }

    if (fGraph)
      fGraph->compress();
  }

  LOG(
      "dsa-bu-graph", for (auto &kv
                           : graphs) {
        errs() << "### Bottom-up Dsa graph for " << kv.first->getName() << "\n";
        kv.second->write(errs());
        errs() << "\n";
      });

  LOG("dsa-bu", errs() << "Finished bottom-up analysis\n");
  return false;
}

BottomUp::BottomUp() : ModulePass(ID), m_dl(nullptr), m_tli(nullptr) {}

void BottomUp::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<AllocWrapInfo>();
  AU.setPreservesAll();
}

bool BottomUp::runOnModule(Module &M) {
  m_dl = &M.getDataLayout();
  m_tli = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  m_allocInfo = &getAnalysis<AllocWrapInfo>();
  CallGraph &cg = getAnalysis<CallGraphWrapperPass>().getCallGraph();

  BottomUpAnalysis bu(*m_dl, *m_tli, *m_allocInfo, cg);
  for (auto &F : M) { // XXX: the graphs must be created here
    if (F.isDeclaration() || F.empty())
      continue;

    GraphRef fGraph = std::make_shared<Graph>(*m_dl, m_setFactory);
    m_graphs[&F] = fGraph;
  }

  return bu.runOnModule(M, m_graphs);
}

Graph &BottomUp::getGraph(const Function &F) const {
  return *(m_graphs.find(&F)->second);
}

bool BottomUp::hasGraph(const Function &F) const {
  return m_graphs.count(&F) > 0;
}

char BottomUp::ID = 0;
} // namespace sea_dsa

static llvm::RegisterPass<sea_dsa::BottomUp> X("seadsa-bu",
                                               "Bottom-up DSA pass");
