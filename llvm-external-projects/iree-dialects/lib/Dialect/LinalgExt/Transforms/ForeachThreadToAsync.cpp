// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdlib>

#include "iree-dialects/Dialect/LinalgExt/Transforms/Transforms.h"
#include "iree-dialects/Dialect/LinalgExt/Transforms/Utils.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Async/IR/Async.h"
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

FailureOr<Operation *>
mlir::iree_compiler::IREE::LinalgExt::ForeachThreadOpToAsyncRewriter::
    returningMatchAndRewrite(scf::ForeachThreadOp foreachThreadOp,
                             PatternRewriter &rewriter) const {
  if (foreachThreadOp.getNumResults() > 0)
    return foreachThreadOp->emitError(
        "only bufferized scf.foreach_thread lowers to async");

  if (foreachThreadOp.getNumThreads().size() > 1)
    return foreachThreadOp->emitError(
        "only single-dimension scf.foreach_thread lowers to async");

  // Only consider the top level ForeachThreadOp op and skip if it already
  // contains an ExecuteOp.
  if (foreachThreadOp->getParentOfType<scf::ForeachThreadOp>() ||
      llvm::any_of(foreachThreadOp.getBody()->getOperations(),
                   [](Operation &op) { return isa<async::ExecuteOp>(&op); }))
    return failure();

  auto *ctx = foreachThreadOp.getContext();
  Location loc = foreachThreadOp.getLoc();
  Value zero = rewriter.create<arith::ConstantIndexOp>(loc, 0);
  Value one = rewriter.create<arith::ConstantIndexOp>(loc, 1);
  // TODO: allow multi-dim.
  Value numThreads = foreachThreadOp.getNumThreads().front();

  // Wrap the scf.foreach_thread into an async::ExecuteOp.
  // 1. Create the async::GroupType object on which we synchronize.
  Value asyncGroup = rewriter.create<async::CreateGroupOp>(
      loc, async::GroupType::get(ctx), numThreads);

  // 2. Create a bodyless forOp.
  scf::ForOp forOp = rewriter.create<scf::ForOp>(loc, zero, numThreads, one);
  rewriter.setInsertionPointToStart(forOp.getBody());

  // 3. Create an empty executeOp, nested within the forOp.
  auto noopExec = [&](OpBuilder &executeBuilder, Location executeLoc,
                      ValueRange executeArgs) {};
  auto executeOp =
      rewriter.create<async::ExecuteOp>(loc, /*resultTypes=*/TypeRange(),
                                        /*dependencies=*/ValueRange(),
                                        /*operands=*/ValueRange(), noopExec);

  // 3. Steal the ops nested under scf::ForeachThread, except the terminator,
  // into the body of the async::ExecuteOp, just before the terminator.
  SmallVector<Value> bbArgsTranslated{forOp.getInductionVar()};
  rewriter.mergeBlocks(&foreachThreadOp.getRegion().front(),
                       executeOp.getBody(), bbArgsTranslated);
  // 3.b. Erase the terminator stolen from foreachThreadOp.
  rewriter.eraseOp(&executeOp.getBody()->back());
  // 3.c. Erase foreachThreadOp.
  rewriter.eraseOp(foreachThreadOp);
  // 3.d. Add ExecuteOp terminator.
  rewriter.setInsertionPointToEnd(executeOp.getBody());
  rewriter.create<async::YieldOp>(loc, ValueRange{});
  // 3.e. Add to group within the loop.
  rewriter.setInsertionPoint(forOp.getBody()->getTerminator());
  rewriter.create<async::AddToGroupOp>(loc, rewriter.getIndexType(),
                                       executeOp.getToken(), asyncGroup);

  // 4. After the iree_compiler::IREE::LinalgExt::ForeachThread, await all async
  // tasks in `asyncGroup`.
  rewriter.setInsertionPointAfter(forOp);
  return rewriter.create<async::AwaitAllOp>(loc, asyncGroup).getOperation();
}
