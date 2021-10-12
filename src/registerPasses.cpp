//===- src/registerPasses.cpp - LLVM-pass registry boilerplate --*- C++ -*-===//
//
// Part of the RV Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "rv/registerPasses.h"
#include "rv/passes.h"
#include "rv/legacy/passes.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "rv/passes/WFVPass.h"
#include "rv/passes/LoopVectorizer.h"
#include "rv/passes/irPolisher.h"

using namespace llvm;

cl::OptionCategory rvCategory("RV Options", "Configure the Region Vectorizer");

static cl::opt<bool> rvLowerBuiltins("rv-lower",
                                     cl::desc("Lower RV specific builtins"),
                                     cl::init(false), cl::ZeroOrMore,
                                     cl::cat(rvCategory));

static cl::opt<bool>
    rvLoopVecEnabled("rv-loopvec",
                     cl::desc("Enable RV's outer-loop vectorizer."),
                     cl::init(false), cl::ZeroOrMore, cl::cat(rvCategory));

static cl::opt<bool>
    rvWFVEnabled("rv-wfv", cl::desc("Enable RV's whole-function vectorizer."),
                 cl::init(false), cl::ZeroOrMore, cl::cat(rvCategory));

static cl::opt<bool> rvOnlyPolish("rv-polish",
                                  cl::desc("Only run RV's polish phase."),
                                  cl::init(false), cl::ZeroOrMore,
                                  cl::cat(rvCategory));

static cl::opt<bool> rvVectorizeEnabled(
    "rv",
    cl::desc("Enable Whole-Function and Outer-Loop Vectorization with RV "
             "(implies -rv-wfv and -rv-loopvec)."),
    cl::init(false), cl::ZeroOrMore, cl::cat(rvCategory));

static bool mayVectorize() {
  return rvWFVEnabled || rvLoopVecEnabled || rvVectorizeEnabled;
}
static bool shouldRunWFVPass() { return rvWFVEnabled || rvVectorizeEnabled; }
static bool shouldRunLoopVecPass() {
  return rvLoopVecEnabled || rvVectorizeEnabled;
}
static bool shouldLowerBuiltins() { return rvLowerBuiltins; }

///// Legacy PM pass registration /////
static void registerLegacyRVPasses(const llvm::PassManagerBuilder &Builder,
                             llvm::legacy::PassManagerBase &PM) {
  if (rvOnlyPolish) {
    PM.add(rv::createIRPolisherLegacyPass());
    return;
  }

  if (mayVectorize()) {
    rv::addPreparatoryLegacyPasses(PM);
  }

  if (shouldRunWFVPass()) {
    PM.add(rv::createWFVLegacyPass());
  }
  if (shouldRunLoopVecPass()) {
    PM.add(rv::createLoopVectorizerLegacyPass());
  }

  if (mayVectorize()) {
    rv::addCleanupLegacyPasses(PM);
  }
}

static void registerLastRVLegacyPasses(const llvm::PassManagerBuilder &Builder,
                                 llvm::legacy::PassManagerBase &PM) {
  if (shouldLowerBuiltins())
    PM.add(rv::createLowerRVIntrinsicsLegacyPass());
}

static llvm::RegisterStandardPasses
    RegisterRV_MidPipeline(llvm::PassManagerBuilder::EP_VectorizerStart,
                           registerLegacyRVPasses);

static llvm::RegisterStandardPasses
    RegisterRV_Last(llvm::PassManagerBuilder::EP_OptimizerLast,
                    registerLastRVLegacyPasses);


///// New PM setup /////

void rv::addConfiguredRVPasses(PassBuilder &PB) {
  PB.registerVectorizerStartEPCallback(
      [&](llvm::FunctionPassManager &FPM,
          llvm::PassBuilder::OptimizationLevel Level) {
        if (rvOnlyPolish) {
          FPM.addPass(rv::IRPolisherWrapperPass());
          return;
        }
        
        if (mayVectorize())
          rv::addPreparatoryPasses(FPM);

        if (shouldRunLoopVecPass()) {
          FPM.addPass(rv::LoopVectorizerWrapperPass());
        }
      });

  PB.registerOptimizerLastEPCallback(
      [&](llvm::ModulePassManager &MPM,
          llvm::PassBuilder::OptimizationLevel Legel) {
        if (shouldRunWFVPass())
          MPM.addPass(rv::WFVWrapperPass());
        if(shouldLowerBuiltins()){
          llvm::FunctionPassManager FPM;
          rv::addLowerBuiltinsPass(FPM);
          MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
        }
        if (mayVectorize())
          rv::addCleanupPasses(MPM);
      });
  // PB.registerPipelineParsingCallback(buildDefaultRVPipeline);
}

#if 0
void addFunctionPasses(ModulePassManager &MPM,
                       std::function<void(llvm::FunctionPassManager &)> Adder) {
  llvm::FunctionPassManager FPM;
  Adder(FPM);
  MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
}

static bool
buildDefaultRVPipeline(StringRef, ModulePassManager &MPM,
                       ArrayRef<PassBuilder::PipelineElement> Elems) {

  // normalize loops
  rv::addPreparatoryPasses(MPM);

  // vectorize scalar functions that have VectorABI attributes
  rv::addWholeFunctionVectorizer(MPM);

  // vectorize annotated loops
  addFunctionPasses(MPM, [](auto &FPM) {
      rv::addOuterLoopVectorizer(FPM);
  });

  // DCE, instcombine, ..
  rv::addCleanupLegacyPasses(MPM);

  return true; // FIXME
}
#endif

namespace rv{
void keepPassRegistration() {};
}