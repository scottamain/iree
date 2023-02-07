// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree-dialects/Dialect/LinalgExt/Transforms/Transforms.h"
#include "iree-dialects/Dialect/LinalgExt/Transforms/Utils.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;
using namespace mlir::iree_compiler::IREE::LinalgExt;

namespace {

SmallVector<Value> getValuesToYield(scf::PerformConcurrentlyOp op) {
  return llvm::to_vector(
      llvm::map_range(op.getYieldingOps(), [](Operation &op) -> Value {
        return cast<tensor::ParallelInsertSliceOp>(&op).getDest();
      }));
}

} // namespace

FailureOr<scf::ForOp> ForeachThreadOpToScfForRewriter::returningMatchAndRewrite(
    scf::ForeachThreadOp foreachThreadOp, PatternRewriter &rewriter) const {
  if (foreachThreadOp.getNumResults() > 0)
    return foreachThreadOp->emitError(
        "only bufferized scf.foreach_thread lowers to scf.for");

  if (foreachThreadOp.getNumThreads().size() > 1)
    return foreachThreadOp->emitError(
        "only single-dimension scf.foreach_thread lowers to scf.for");

  // Construct the loop bounds based on the canonical arithmetic progression.
  Location loc = foreachThreadOp.getLoc();
  Value zero = rewriter.create<arith::ConstantIndexOp>(loc, 0);
  Value one = rewriter.create<arith::ConstantIndexOp>(loc, 1);
  // TODO: allow multi-dim.
  Value numThreads = foreachThreadOp.getNumThreads().front();

  // Construct the op without a body builder: we need to clone the ops in the
  // body explicitly after having access to the new bbArgs.
  // As a consequence, `ensureTerminator` is not called and the `forOp` body
  // has no terminator.
  scf::PerformConcurrentlyOp performConcurrentlyOp =
      foreachThreadOp.getTerminator();
  SmallVector<Value> valuesToYield = getValuesToYield(performConcurrentlyOp);
  scf::ForOp forOp =
      rewriter.create<scf::ForOp>(loc, zero, numThreads, one, valuesToYield);

  // Move the body while replacing the threadId by the forOp iv.
  SmallVector<Value> bbArgsTranslated{forOp.getInductionVar()};
  Block *body = forOp.getBody();
  bool hasTerminator =
      !body->empty() && body->back().hasTrait<OpTrait::IsTerminator>();
  if (hasTerminator) {
    rewriter.mergeBlockBefore(&foreachThreadOp.getRegion().front(),
                              body->getTerminator(), bbArgsTranslated);
  } else {
    rewriter.mergeBlocks(&foreachThreadOp.getRegion().front(), body,
                         bbArgsTranslated);
  }

  rewriter.setInsertionPointToStart(body);
  IRMapping bvm;
  bvm.map(valuesToYield, forOp.getRegionIterArgs());

  // Create sequential insertSlice ops.
  SmallVector<Value> toYield;
  rewriter.setInsertionPoint(performConcurrentlyOp);
  for (Operation &operation : performConcurrentlyOp.getYieldingOps()) {
    tensor::ParallelInsertSliceOp op =
        cast<tensor::ParallelInsertSliceOp>(&operation);
    toYield.push_back(rewriter.createOrFold<tensor::InsertSliceOp>(
        loc, op.getSource(), bvm.lookup(op.getDest()), op.getMixedOffsets(),
        op.getMixedSizes(), op.getMixedStrides()));
  }

  // performConcurrentlyOp.yieldedValues come from above, not from bbArgs.
  // There is no rewriter method to make mergeBlocks update non-bbArgs.
  // Need to manually clone + bvm all uses that are now nested under forOp.
  // Warning: this replacement is currently optimistic and may change the
  // semantics as explained in the pass description in Passes.td.
  SmallVector<Operation *> opsToReplace;
  for (Value toReplace : valuesToYield) {
    for (OpOperand &u : toReplace.getUses()) {
      Operation *op = u.getOwner();
      if (!forOp->isProperAncestor(op))
        continue;
      opsToReplace.push_back(op);
    }
  }
  for (Operation *op : opsToReplace) {
    OpBuilder::InsertionGuard g(rewriter);
    rewriter.setInsertionPoint(op);
    Operation *cloned = rewriter.clone(*op, bvm);
    rewriter.replaceOp(op, cloned->getResults());
  }

  // Insert terminator.
  if (!hasTerminator) {
    rewriter.setInsertionPointToEnd(body);
    rewriter.create<scf::YieldOp>(loc, toYield);
  }

  // Cleanup and replace.
  rewriter.eraseOp(performConcurrentlyOp);
  rewriter.replaceOp(foreachThreadOp, forOp.getResults());

  return forOp;
}
