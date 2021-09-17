//===- rv/registerPasses.h - RV pass plugin support (RVPLUG) --*- C++ -*-===//
//
// Part of the RV Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Callback into libRV for the RVPLUG passplug in populate the new PM.
//
//===----------------------------------------------------------------------===//

#ifndef RV_REGISTERPASSES_H
#define RV_REGISTERPASSES_H

#include "llvm/Passes/PassPlugin.h"
#include "llvm/Passes/PassBuilder.h"

namespace rv {
  void addRVPasses(llvm::ModulePassManager & MPM, llvm::PassBuilder::OptimizationLevel);
}

#endif // RV_REGISTERPASSES_H
