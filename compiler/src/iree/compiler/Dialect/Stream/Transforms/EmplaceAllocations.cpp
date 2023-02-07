// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <utility>

#include "iree/compiler/Dialect/Stream/IR/StreamDialect.h"
#include "iree/compiler/Dialect/Stream/IR/StreamOps.h"
#include "iree/compiler/Dialect/Stream/Transforms/PassDetail.h"
#include "iree/compiler/Dialect/Stream/Transforms/Passes.h"
#include "iree/compiler/Dialect/Util/IR/UtilDialect.h"
#include "iree/compiler/Dialect/Util/IR/UtilOps.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#define DEBUG_TYPE "iree-stream-emplace-allocations"

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace Stream {
namespace {

//===----------------------------------------------------------------------===//
// Emplacement
//===----------------------------------------------------------------------===//

static bool tryEmplaceDispatchOp(IREE::Stream::AsyncDispatchOp dispatchOp) {
  bool didChange = false;
  for (auto [resultIndex, result] : llvm::enumerate(dispatchOp.getResults())) {
    // Ignore results with multiple users. We could potentially place these but
    // that makes tracking much more complicated.
    if (!result.hasOneUse()) continue;
    // Ignore already-tied operands.
    // TODO(benvanik): update tied range if we want to place into a superset?
    auto operandIndex = dispatchOp.getTiedResultOperandIndex(resultIndex);
    if (operandIndex.has_value()) continue;

    // Find potential.
    Value targetResource;
    Value targetResourceSize;
    Value targetOffset;
    Value targetEnd;
    Value targetLength;
    Value targetResult;
    Value targetResultSize;
    Operation *userOp = *result.user_begin();
    if (auto updateOp = dyn_cast<IREE::Stream::AsyncUpdateOp>(userOp)) {
      if (updateOp.getUpdate() != result) continue;
      if (!IREE::Util::tryMoveProducerBefore(updateOp.getTarget(),
                                             dispatchOp)) {
        // Failed to move while keeping valid SSA dominance.
        continue;
      }
      targetResource = updateOp.getTarget();
      if (targetResource.getDefiningOp() == dispatchOp) {
        // NOTE: we may have already replaced the update target with one of our
        // results - if so we need to find the operand to capture tied to that
        // new result instead of our own new result (which would make a cycle).
        targetResource = dispatchOp.getTiedResultOperand(targetResource);
      }
      targetResourceSize = updateOp.getTargetSize();
      targetOffset = updateOp.getTargetOffset();
      targetEnd = updateOp.getTargetEnd();
      targetLength = updateOp.getUpdateSize();
      targetResult = updateOp.getResult();
      targetResultSize = updateOp.getTargetSize();
    }
    if (!targetResource) continue;

    // Add operand and tie the result.
    operandIndex = dispatchOp.getResourceOperands().size();
    dispatchOp.getResourceOperandsMutable().append(targetResource);
    dispatchOp.getResourceOperandSizesMutable().append(targetResourceSize);
    dispatchOp.getResourceOperandOffsetsMutable().append(targetOffset);
    dispatchOp.getResourceOperandEndsMutable().append(targetEnd);
    dispatchOp.getResourceOperandLengthsMutable().append(targetLength);
    dispatchOp.setTiedResultOperandIndex(resultIndex, operandIndex);

    // Update result size (requires this dance as [] is a no-op!).
    SmallVector<Value> resultSizes = dispatchOp.getResultSizes();
    resultSizes[resultIndex] = targetResultSize;
    dispatchOp.getResultSizesMutable().assign(resultSizes);

    // Replace users with the result of the dispatch op.
    targetResult.replaceAllUsesWith(result);
    userOp->erase();

    didChange = true;
  }
  return didChange;
}

// Emplaces allocations within |region|.
// Returns true if any allocations were elided by way of emplacement.
static bool emplaceAllocationsInRegion(Region &region) {
  bool didChange = false;
  for (auto &block : region.getBlocks()) {
    for (auto &op : block) {
      if (!op.hasTrait<OpTrait::IREE::Stream::AsyncPhaseOp>()) continue;
      // TODO(benvanik): support placement for more ops e.g. copies/collectives.
      didChange = TypeSwitch<Operation *, bool>(&op)
                      // TODO(#11249): support in-place collective ops.
                      .Case<IREE::Stream::AsyncDispatchOp>(
                          [&](auto op) { return tryEmplaceDispatchOp(op); })
                      .Default(false) ||
                  didChange;
    }
  }
  return didChange;
}

//===----------------------------------------------------------------------===//
// -iree-stream-emplace-allocations
//===----------------------------------------------------------------------===//

class EmplaceAllocationsPass
    : public EmplaceAllocationsBase<EmplaceAllocationsPass> {
 public:
  EmplaceAllocationsPass() = default;

  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<mlir::func::FuncDialect>();
    registry.insert<mlir::arith::ArithDialect>();
    registry.insert<IREE::Stream::StreamDialect>();
    registry.insert<IREE::Util::UtilDialect>();
  }

  void runOnOperation() override {
    bool didChange = false;
    getOperation()->walk([&](Region *region) {
      didChange = emplaceAllocationsInRegion(*region) || didChange;
    });
    // TODO(benvanik): run canonicalization patterns inline if anything changed.
    (void)didChange;
  }
};

}  // namespace

std::unique_ptr<OperationPass<>> createEmplaceAllocationsPass() {
  return std::make_unique<EmplaceAllocationsPass>();
}

}  // namespace Stream
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir
