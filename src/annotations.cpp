//===- src/annotations.cpp - getter/sterr for IR augmentations --*- C++ -*-===//
//
// Part of the RV Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "rv/annotations.h"
#include "rv/analysis/reductions.h"

#include "rvConfig.h"
#include <cassert>

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"

using namespace llvm;

namespace {
  const char* rv_atomic_string = "rv_atomic";
  const char* rv_redkind_string  = "rv_redkind";
}

namespace rv {

bool
IsCriticalSection(const llvm::Function & func) {
  auto * mdNode = func.getMetadata(rv_atomic_string);
  return (bool) mdNode;
}

void
MarkAsCriticalSection(llvm::Function & func) {
  // mark function as "noinline" to survive O3
  func.removeAttribute(AttributeList::FunctionIndex, Attribute::AlwaysInline);
  func.addAttribute(AttributeList::FunctionIndex, Attribute::NoInline);
  func.setMetadata(rv_atomic_string, MDNode::get(func.getContext(), {}));
}

void
SetReductionHint(llvm::PHINode & loopHeaderPhi, RedKind redKind) {
  StringRef redKindText = to_string(redKind);
  auto & ctx = loopHeaderPhi.getContext();

  auto * redKindNode = MDString::get(ctx, redKindText);
  auto * boxedNode = MDNode::get(ctx, redKindNode);
  loopHeaderPhi.setMetadata(rv_redkind_string, boxedNode);
}

RedKind
ReadReductionHint(const llvm::PHINode & loopHeaderPhi) {
  auto * boxedHint = loopHeaderPhi.getMetadata(rv_redkind_string);
  if (!boxedHint) return RedKind::Bot; // unknown
  assert(boxedHint->getNumOperands() >= 1);

  auto * boxedRedCode = dyn_cast<MDString>(boxedHint->getOperand(0));
  assert(boxedRedCode);

  RedKind kind;
  bool parsed = from_string(boxedRedCode->getString(), kind);
  assert(parsed);
  return kind;
}

bool IsHipSYCLKernel(const llvm::Function &F) {
  for (auto &I : F.getParent()->globals()) {
    if (I.getName() == "llvm.global.annotations") {
      auto *CA = llvm::dyn_cast<llvm::ConstantArray>(I.getOperand(0));
      for (auto *OI = CA->op_begin(); OI != CA->op_end(); ++OI) {
        auto *CS = llvm::dyn_cast<llvm::ConstantStruct>(OI->get());
        auto *AnnotFunc = llvm::dyn_cast<llvm::Function>(CS->getOperand(0)->getOperand(0));
        auto *AnnotationGL = llvm::dyn_cast<llvm::GlobalVariable>(CS->getOperand(1)->getOperand(0));
        llvm::StringRef Annotation =
            llvm::dyn_cast<llvm::ConstantDataArray>(AnnotationGL->getInitializer())->getAsCString();
        if (Annotation.compare("hipsycl_nd_kernel") == 0 && AnnotFunc == &F) {
          return true;
        }
      }
    }
  }
  return false;
}

}
