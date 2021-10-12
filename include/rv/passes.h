//===- rv/LinkAllPasses.h - create RV passes --*- C++ -*-===//
//
// Part of the RV Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef RV_PASSES_H
#define RV_PASSES_H

#include "llvm/Pass.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "rv/config.h"

namespace rv {

// add normalization passes required by RV (BEFORE)
void addPreparatoryPasses(llvm::FunctionPassManager &FPM);

// add cleanup passes to run after RV (AFTER)
void addCleanupPasses(llvm::ModulePassManager &MPM);

// insert a pass that
void addLowerBuiltinsPass(llvm::FunctionPassManager &FPM);



} // namespace rv


#endif
