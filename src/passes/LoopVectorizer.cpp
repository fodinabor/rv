//===- LoopVectorizer.cpp - Vectorize Loops  ----------------===//
//
//                     The Region Vectorizer
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include "rv/passes/LoopVectorizer.h"
#include "rv/legacy/LinkAllPasses.h"

#include "rv/rv.h"
#include "rv/vectorMapping.h"
#include "rv/region/LoopRegion.h"
#include "rv/region/Region.h"
#include "rv/resolver/resolvers.h"
#include "rv/analysis/loopAnnotations.h"
#include "rv/analysis/reductionAnalysis.h"
#include "rv/analysis/costModel.h"
#include "rv/annotations.h"
#include "rv/transform/remTransform.h"

#include "rv/config.h"
#include "rvConfig.h"
#include "rv/rvDebug.h"

#include "llvm/InitializePasses.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Passes/PassBuilder.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"

#include "llvm/Transforms/Utils/Cloning.h"

#include "report.h"
#include <map>

using namespace rv;
using namespace llvm;

// typedef DomTreeNodeBase<BasicBlock*> DomTreeNode;



bool LoopVectorizer::canVectorizeLoop(Loop &L) {
  if (!L.isAnnotatedParallel())
    return false;

  BasicBlock *ExitingBlock = L.getExitingBlock();
  if (!ExitingBlock)
    return false;

  if (!isa<BranchInst>(ExitingBlock->getTerminator()))
    return false;

  return true;
}

int LoopVectorizer::getTripAlignment(Loop &L) {
  int tripCount = getTripCount(L);
  if (tripCount > 0) return tripCount;
  return 1;
}


bool LoopVectorizer::canAdjustTripCount(Loop &L, int VectorWidth, int TripCount) {
  if (VectorWidth == TripCount)
    return true;

  return false;
}

int LoopVectorizer::getTripCount(Loop &L) {
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto *BTC = dyn_cast<SCEVConstant>(SE.getBackedgeTakenCount(&L));
  if (!BTC)
    return -1;

  int64_t BTCVal = BTC->getValue()->getSExtValue();
  if (BTCVal <= 1 || ((int64_t)((int) BTCVal)) != BTCVal)
    return -1;

  return BTCVal + 1;
}

Loop*
LoopVectorizer::transformToVectorizableLoop(Loop &L, int VectorWidth, int tripAlign, ValueSet & uniformOverrides) {
  IF_DEBUG { errs() << "\tCreating scalar remainder Loop for " << L.getName() << "\n"; }

  // try to applu the remainder transformation
  RemainderTransform remTrans(F, *DT, *PDT, *LI, *reda, PB);
  auto * preparedLoop = remTrans.createVectorizableLoop(L, uniformOverrides, VectorWidth, tripAlign);

  return preparedLoop;
}

static
bool
IsSupportedReduction(Loop & L, Reduction & red) {
  // check that all users of the reduction are either (a) part of it or (b) outside the loop
  for (auto * inst : red.elements) {
    for (auto itUser : inst->users()) {
      auto * userInst = dyn_cast<Instruction>(itUser);
      if (!userInst) return false; // unsupported
      if (L.contains(userInst->getParent()) &&
        !red.elements.count(userInst))  {
        errs() << "Unsupported user of reduction: "; Dump(*userInst); errs() << "\n"; 
        return false;
      }
    }
  }

  return true;
}

bool
LoopVectorizer::vectorizeLoop(Loop &L) {
// check the dependence distance of this loop
  LoopMD mdAnnot = GetLoopAnnotation(L);

  if (enableDiagOutput) { Report() << "loopVecPass: "; mdAnnot.print(ReportContinue()) << "\n"; }

  // only trigger on hipSYCL WI loops
  // if(!mdAnnot.hipsyclWiLoop) {
  //   if (enableDiagOutput) Report() << "loopVecPass skip " << L.getName() << " . no hipSYCL WI loop.\n";
  //   return false;
  // }

  // Trivial case.
  if (L.isAnnotatedParallel()) {
    mdAnnot.minDepDist = ParallelDistance;
    mdAnnot.vectorizeEnable = true;
  }

  if (enableDiagOutput) {
    Report() << "loopVecPass: ";
    mdAnnot.print(ReportContinue()) << "\n";
  }

  // only trigger on annotated loops
  if (!mdAnnot.vectorizeEnable.safeGet(false)) {
    if (enableDiagOutput) Report() << "loopVecPass skip " << L.getName() << " . not explicitly triggered.\n";
    return false;
  }

  // skip if already vectorized
  if (mdAnnot.alreadyVectorized.safeGet(false)) {
    // too verbose
    if (enableDiagOutput) Report() << "loopVecPass skip " << L.getName() << " . already vectorized.\n";
    return false;
  }

  iter_t depDist = mdAnnot.minDepDist.safeGet(ParallelDistance);

  // skip if iteration dependence distance precludes vectorization
  if (depDist <= 1) {
    if (enableDiagOutput) Report() << "loopVecPass skip " << L.getName() << " . Min dependence distance was " << depDist << "\n";
    return false;
  }

  this->DT = &FAM.getResult<DominatorTreeAnalysis>(F);
  this->PDT = &FAM.getResult<PostDominatorTreeAnalysis>(F);
  this->PB = &FAM.getResult<BranchProbabilityAnalysis>(F);

  auto invalidator = [this](int*) {
    PreservedAnalyses PA;
    PA.preserve<LoopAnalysis>();
    FAM.invalidate(F, PA);
  };
  std::unique_ptr<int, decltype(invalidator)> invalidateGuard{new int(0), std::move(invalidator)};

  if(CheckFlag("LV_VIEW_CFG"))
    L.getHeader()->getParent()->viewCFG();

  int tripAlign = getTripAlignment(L);

  // use the explicitVectorWidth (if specified). Otherwise
  bool hasFixedWidth = mdAnnot.explicitVectorWidth.isSet();
  int VectorWidth = hasFixedWidth ? mdAnnot.explicitVectorWidth.get() : depDist;

  // environment user override
  char * userWidthText = getenv("RV_FORCE_WIDTH");
  if (userWidthText) {
    hasFixedWidth = true;
    VectorWidth = atoi(userWidthText);
    if (enableDiagOutput) Report() << "loopVecPass: with user-provided vector width (RV_FORCE_WIDTH=" << VectorWidth << ")\n";
  }

// pick a vectorization factor (unless user override is set)
  if (!hasFixedWidth) {
    size_t initialWidth = VectorWidth == 0 ? depDist : VectorWidth;

    // Re-fine using cost model
    CostModel costModel(vectorizer->getPlatformInfo(), RVConfig);
    LoopRegion tmpLoopRegionImpl(L);
    Region tmpLoopRegion(tmpLoopRegionImpl);
    size_t refinedWidth = costModel.pickWidthForRegion(tmpLoopRegion, initialWidth); // TODO run VA first

    if (refinedWidth <= 1) {
      if (enableDiagOutput) { Report() << "loopVecPass, costModel: vectorization not beneficial\n"; }
      return false;
    } else if (refinedWidth != (size_t) VectorWidth) {
      if (enableDiagOutput) {
        Report() << "loopVecPass, costModel: refined vector width to " << DepDistToString(refinedWidth) << " from ";
        if (VectorWidth > 1) Report() << VectorWidth << "\n";
        else ReportContinue() << " unbounded\n";
      }
      VectorWidth = refinedWidth;
    }
  }

  Report() << "loopVecPass: Vectorize " << L.getName()
           << " in " << L.getHeader()->getParent()->getName()
           << " with VW: " << VectorWidth
           << " , Dependence Distance: " << DepDistToString(depDist)
           << " and TripAlignment: " << tripAlign << "\n";

// analyze the recurrsnce patterns of this loop
  reda.reset(new ReductionAnalysis(F, FAM));
  reda->analyze(L);

// match vector loop structure
  ValueSet uniOverrides;
  auto * PreparedLoop = transformToVectorizableLoop(L, VectorWidth, tripAlign, uniOverrides);
  if (!PreparedLoop) {
    Report() << "loopVecPass: Can not prepare vectorization of the loop\n";
    return false;
  }

  // mark the remainder loop as un-vectorizable
  LoopMD llvmLoopMD;
  llvmLoopMD.alreadyVectorized = true;
  SetLLVMLoopAnnotations(L, std::move(llvmLoopMD));

  // clear loop annotations from our copy of the lop
  ClearLoopVectorizeAnnotations(*PreparedLoop);

  // print configuration banner once
  if (!introduced) {
    Report() << " rv::Config: ";
    RVConfig.print(ReportContinue());
    introduced = true;
  }

// start vectorizing the prepared loop
  IF_DEBUG { errs() << "rv: Vectorizing loop " << L.getName() << "\n"; }
  
  if(CheckFlag("LV_VIEW_CFG"))
    L.getHeader()->getParent()->viewCFG();

  VectorMapping targetMapping(&F, &F, VectorWidth, CallPredicateMode::SafeWithoutPredicate);
  LoopRegion LoopRegionImpl(*PreparedLoop);
  Region LoopRegion(LoopRegionImpl);

  VectorizationInfo vecInfo(F, VectorWidth, LoopRegion);

// Check reduction patterns of vector loop phis
  // configure initial shape for induction variable
  for (auto & inst : *PreparedLoop->getHeader()) {
    auto * phi = dyn_cast<PHINode>(&inst);
    if (!phi) continue;

    rv::StridePattern * pat = reda->getStrideInfo(*phi);
    VectorShape phiShape;
    if (pat) {
      IF_DEBUG { pat->dump(); }
      phiShape = pat->getShape(VectorWidth);
    } else {
      rv::Reduction * redInfo = reda->getReductionInfo(*phi);
      IF_DEBUG { errs() << "loopVecPass: header phi  " << *phi << " : "; }

      // failure to derive a reduction descriptor
      if (!redInfo) {
        Report() << "\n\tskip: unrecognized phi use in vector loop " << L.getName() << "\n";
        return false;
      }

      if (!IsSupportedReduction(*PreparedLoop, *redInfo)) {
        Report() << " unsupported reduction: "; redInfo->print(ReportContinue()); ReportContinue() << "\n";
        return false;
      }

      // unsupported reduction kind
      if (redInfo->kind == RedKind::Top) {
        Report() << " can not vectorize this non-trivial SCC: "; redInfo->print(ReportContinue()); ReportContinue() << "\n";
        return false;
      }

      // FIXME rv codegen only supports trivial recurrences at the moment
      if (redInfo->kind == RedKind::Bot) {
        Report() << " can not vectorize this non-affine recurrence: "; redInfo->print(ReportContinue()); ReportContinue() << "\n";
        return false;
      }

      // Otw, this is a privatizable reduction pattern
      IF_DEBUG { redInfo->dump(); }
      phiShape = redInfo->getShape(VectorWidth);
    }

    IF_DEBUG { errs() << "header phi " << phi->getName() << " has shape " << phiShape.str() << "\n"; }

    if (phiShape.isDefined()) vecInfo.setPinnedShape(*phi, phiShape);
  }

  // set uniform overrides
  IF_DEBUG { errs() << "-- Setting remTrans uni overrides --\n"; }
  for (auto * val : uniOverrides) {
    IF_DEBUG { errs() << "- " << *val << "\n"; }
    vecInfo.setPinnedShape(*val, VectorShape::uni());
  }

  //DT.verify();
  //LI.verify(DT);

  verifyFunction(F, &errs());
  IF_DEBUG {
    verifyFunction(F, &errs());
    DT->verify();
    PDT->print(errs());
    LI->print(errs());
    // LI->verify(*DT); // FIXME unreachable blocks
  }

// early math func lowering
  vectorizer->lowerRuntimeCalls(vecInfo, FAM);
  DT->recalculate(F);
  PDT->recalculate(F);

// Vectorize
  // vectorizationAnalysis
  vectorizer->analyze(vecInfo, FAM);

  if (enableDiagOutput) {
    errs() << "-- VA result --\n";
    vecInfo.dump();
    errs() << "-- EOF --\n";
  }

  IF_DEBUG Dump(F);
  assert(L.getLoopPreheader());

  // control conversion
  vectorizer->linearize(vecInfo, FAM);

  // vectorize the prepared loop embedding it in its context
  ValueToValueMapTy vecMap;

  ScalarEvolutionAnalysis adhocAnalysis;
  adhocAnalysis.run(F, FAM);

  bool vectorizeOk = vectorizer->vectorize(vecInfo, FAM, &vecMap);
  if (!vectorizeOk)
    llvm_unreachable("vector code generation failed");

  if (enableDiagOutput) {
    errs() << "-- Vectorized --\n";
    for (const BasicBlock * BB : PreparedLoop->blocks()) {
      const BasicBlock * vecB = cast<const BasicBlock>(vecMap[BB]);
      Dump(*vecB);
    }
    errs() << "-- EOF --\n";
  }

  if(CheckFlag("LV_VIEW_CFG"))
    L.getHeader()->getParent()->viewCFG();
  verifyFunction(F, &errs());


  return true;
}

bool LoopVectorizer::vectorizeLoopOrSubLoops(Loop &L) {
  if (vectorizeLoop(L))
    return true;

  bool Changed = false;

  std::vector<Loop*> loops;
  for (Loop *SubL : L) loops.push_back(SubL);
  for (Loop* SubL : loops)
    Changed |= vectorizeLoopOrSubLoops(*SubL);

  return Changed;
}

LoopVectorizer::LoopVectorizer(Function &F, TargetTransformInfo &PassTTI,
                               TargetLibraryInfo &PassTLI,
                               OptimizationRemarkEmitter &PassORE)
    : RVConfig(Config::createForFunction(F)), F(F), PassTTI(PassTTI),
      PassTLI(PassTLI), PassORE(PassORE) {
  // have we introduced ourself? (reporting output)
  enableDiagOutput = CheckFlag("LV_DIAG");
  introduced = false;
}

bool LoopVectorizer::run() {
  if (getenv("RV_DISABLE"))
    return false;

  if (enableDiagOutput)
    Report() << "loopVecPass: run on " << F.getName() << "\n";

  if (CheckFlag("RV_PRINT_FUNCTION")) {
    Report() << "-- RV::LoopVectorizer --\n";
    F.print(Report());
  }

  IF_DEBUG {
    errs() << " -- module before RV --\n";
    Dump(*F.getParent());
  }

  if(!IsHipSYCLKernel(F))
    return false;

  // create private analysis infrastructure
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);

  this->DT = &FAM.getResult<DominatorTreeAnalysis>(F);
  this->PDT = &FAM.getResult<PostDominatorTreeAnalysis>(F);
  this->LI = &FAM.getResult<LoopAnalysis>(F);
  this->PB = &FAM.getResult<BranchProbabilityAnalysis>(F);

  // setup PlatformInfo
  PlatformInfo platInfo(*F.getParent(), &PassTTI, &PassTLI);
  vectorizer.reset(new VectorizerInterface(platInfo, RVConfig));

  // TODO translate fast-math flag to ULP error bound
  if (!CheckFlag("RV_NO_SLEEF")) {
    addSleefResolver(RVConfig, platInfo);
  }

  // enable inter-procedural vectorization
  if (RVConfig.enableGreedyIPV) {
    Report() << "Using greedy inter-procedural vectorization.\n";
    addRecursiveResolver(RVConfig, platInfo);
  }

  if (enableDiagOutput) {
    platInfo.print(ReportContinue());
  }

  bool Changed = false;

  std::vector<Loop*> loops;
  for (Loop *L : *LI) loops.push_back(L);
  for (auto * L : loops) Changed |= vectorizeLoopOrSubLoops(*L);


  IF_DEBUG { errs() << " -- module after RV --\n"; Dump(*F.getParent()); }

  // cleanup
  reda.reset();
  vectorizer.reset();
  return Changed;
}

///// OldPM Wrapper Pass /////

void LoopVectorizerLegacyPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetTransformInfoWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<OptimizationRemarkEmitterWrapperPass>();
}

bool LoopVectorizerLegacyPass::runOnFunction(Function &F) {
  auto &TTI = getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  auto &ORE = getAnalysis<OptimizationRemarkEmitterWrapperPass>().getORE();

  LoopVectorizer LoopVec(F, TTI, TLI, ORE);
  return LoopVec.run();
}

char LoopVectorizerLegacyPass::ID = 0;

FunctionPass *rv::createLoopVectorizerLegacyPass() { return new LoopVectorizerLegacyPass(); }

INITIALIZE_PASS_BEGIN(LoopVectorizerLegacyPass, "rv-loop-vectorize",
                      "RV - Vectorize loops", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MemoryDependenceWrapperPass)
INITIALIZE_PASS_DEPENDENCY(PostDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(BranchProbabilityInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
// PlatformInfo
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(LoopVectorizerLegacyPass, "rv-loop-vectorize", "RV - Vectorize loops",
                    false, false)


///// NewPM Wrapper Pass /////

llvm::PreservedAnalyses
LoopVectorizerWrapperPass::run(llvm::Function &F,
                               llvm::FunctionAnalysisManager &FAM) {
  auto &TTI = FAM.getResult<TargetIRAnalysis>(F);
  auto &TLI = FAM.getResult<TargetLibraryAnalysis>(F);
  auto &ORE = FAM.getResult<OptimizationRemarkEmitterAnalysis>(F);

  LoopVectorizer LoopVec(F, TTI, TLI, ORE);
  if (LoopVec.run())
    return llvm::PreservedAnalyses::none();
  else
    return llvm::PreservedAnalyses::all();
}
