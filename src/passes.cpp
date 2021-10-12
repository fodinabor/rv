//===- src/passes.cpp - create RV passes --*- C++ -*-===//
//
// Part of the RV Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "rv/passes.h"

#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"

#include "rv/passes/LoopVectorizer.h"
#include "rv/passes/WFVPass.h"
#include "rv/passes/loopExitCanonicalizer.h"
#include "rv/passes/lowerRVIntrinsics.h"

#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>

using namespace llvm;

namespace rv {

///// New PM Registration /////
void
addPreparatoryPasses(FunctionPassManager & FPM) {
  FPM.addPass(LoopSimplifyPass());
  FPM.addPass(LCSSAPass());
  FPM.addPass(rv::LoopExitCanonicalizerWrapperPass()); // required for divLoopTrans
}

void
addCleanupPasses(ModulePassManager & MPM) {
  // post rv cleanup
  MPM.addPass(AlwaysInlinerPass());
  llvm::FunctionPassManager FPM;
  FPM.addPass(AggressiveInstCombinePass());
  FPM.addPass(ADCEPass());
  MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
}

void addRVPasses(ModulePassManager &MPM) {
  // normalize loops
  llvm::FunctionPassManager FPM;
  addPreparatoryPasses(FPM);
  MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));

  // vectorize scalar functions that have VectorABI attributes
  MPM.addPass(WFVWrapperPass());

  // vectorize annotated loops
  FPM.addPass(LoopVectorizerWrapperPass());
  MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));

  // DCE, instcombine, ..
  addCleanupPasses(MPM);
}

void addLowerBuiltinsPass(llvm::FunctionPassManager &FPM) {
  // lower builtins
  FPM.addPass(LowerRVIntrinsicsWrapperPass());
}

}
