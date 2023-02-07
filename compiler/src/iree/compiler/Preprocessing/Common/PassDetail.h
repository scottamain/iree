// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_COMPILER_PREPROCESSING_COMMON_PASS_DETAIL_H_
#define IREE_COMPILER_PREPROCESSING_COMMON_PASS_DETAIL_H_

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/FunctionInterfaces.h"
#include "mlir/Pass/Pass.h"

namespace mlir {
namespace iree_compiler {
namespace IREE {

#define GEN_PASS_CLASSES
#include "iree/compiler/Preprocessing/Common/Passes.h.inc"  // IWYU pragma: keep

}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir

#endif  // IREE_COMPILER_PREPROCESSING_COMMON_PASS_DETAIL_H_
