// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/Interfaces/BufferizationInterfaces.h"

#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtDialect.h"
#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtOps.h"
#include "iree/compiler/Dialect/Flow/IR/FlowDialect.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "iree/compiler/Dialect/Flow/IR/FlowTypes.h"
#include "iree/compiler/Dialect/HAL/IR/HALOps.h"
#include "mlir/Dialect/Arith/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Bufferization/IR/BufferizableOpInterface.h"
#include "mlir/Dialect/Bufferization/Transforms/FuncBufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Bufferization/Transforms/OneShotAnalysis.h"
#include "mlir/Dialect/Bufferization/Transforms/Transforms.h"
#include "mlir/Dialect/Linalg/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Tensor/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Vector/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Support/LLVM.h"

using mlir::bufferization::AnalysisState;
using mlir::bufferization::BufferizableOpInterface;
using mlir::bufferization::BufferizationAliasInfo;
using mlir::bufferization::BufferizationOptions;
using mlir::bufferization::eliminateEmptyTensors;
using mlir::bufferization::OneShotAnalysisState;
using mlir::bufferization::OneShotBufferizationOptions;
using mlir::bufferization::replaceOpWithBufferizedValues;
using mlir::bufferization::replaceOpWithNewBufferizedOp;

namespace mlir {
namespace iree_compiler {

//===----------------------------------------------------------------------===//
// Utility functions.
//===----------------------------------------------------------------------===//

static MemRefType getMemrefTypeForTensor(
    IREE::Flow::DispatchTensorType tensorType,
    MemRefLayoutAttrInterface layout = {}, Attribute memorySpace = {}) {
  return MemRefType::get(tensorType.getShape(),
                         tensorType.getBoundElementType(), layout, memorySpace);
}

/// Find the memref version of the given InterfaceBindingSubspanOp. If no such
/// op exists in the same block (before the given op), create a new op.
static Value findOrCreateSubspanBuffer(
    OpBuilder &b, IREE::HAL::InterfaceBindingSubspanOp subspanOp) {
  // Ensure that this a tensor subspan op.
  auto shapedType = subspanOp.getResult()
                        .getType()
                        .dyn_cast<IREE::Flow::DispatchTensorType>();
  assert(shapedType && shapedType.hasRank());

  auto memRefType = getMemrefTypeForTensor(shapedType, /*layout=*/{},
                                           subspanOp.getDescriptorTypeAttr());

  // Look for an existing op.
  Block *block = subspanOp->getBlock();
  for (Operation &op : *block) {
    if (&op == subspanOp.getOperation()) break;
    auto bufferSubspanOp = dyn_cast<IREE::HAL::InterfaceBindingSubspanOp>(&op);
    if (!bufferSubspanOp) continue;

    auto bufferMemrefType =
        bufferSubspanOp.getResult().getType().dyn_cast<MemRefType>();
    if (!bufferMemrefType) continue;

    if (bufferSubspanOp.getSet() != subspanOp.getSet() ||
        bufferSubspanOp.getBinding() != subspanOp.getBinding() ||
        bufferSubspanOp.getDescriptorType() != subspanOp.getDescriptorType() ||
        bufferSubspanOp.getByteOffset() != subspanOp.getByteOffset() ||
        !llvm::equal(bufferSubspanOp.getDynamicDims(),
                     subspanOp.getDynamicDims()) ||
        bufferSubspanOp.getAlignment() != subspanOp.getAlignment() ||
        memRefType != bufferMemrefType)
      continue;
    return bufferSubspanOp.getResult();
  }

  // None found, create a new op.
  OpBuilder::InsertionGuard g(b);
  b.setInsertionPoint(subspanOp);
  // Just change the result type of the InterfaceBindingSubspanOp.
  Value buffer = b.create<IREE::HAL::InterfaceBindingSubspanOp>(
      subspanOp->getLoc(), memRefType, subspanOp.getSet(),
      subspanOp.getBinding(), subspanOp.getDescriptorType(),
      subspanOp.getByteOffset(), subspanOp.getDynamicDims(),
      subspanOp.getAlignmentAttr(), subspanOp.getDescriptorFlagsAttr());
  b.create<memref::AssumeAlignmentOp>(subspanOp->getLoc(), buffer,
                                      subspanOp.calculateAlignment().value());
  return buffer;
}

namespace {

//===----------------------------------------------------------------------===//
// IREE specific External models for BufferizableOpInterface.
//===----------------------------------------------------------------------===//

/// Check if the two tensor types (with their respective dynamic dimension
/// values) have the same shape.
static bool equalTensorShape(RankedTensorType tensorType,
                             ValueRange tensorDynSizes,
                             IREE::Flow::DispatchTensorType dispatchTensorType,
                             ValueRange dispatchTensorDynSizes) {
  return llvm::equal(tensorType.getShape(), dispatchTensorType.getShape()) &&
         tensorDynSizes.size() == dispatchTensorDynSizes.size() &&
         llvm::equal(tensorDynSizes, dispatchTensorDynSizes);
}

struct DispatchTensorLoadOpInterface
    : public BufferizableOpInterface::ExternalModel<
          DispatchTensorLoadOpInterface, IREE::Flow::DispatchTensorLoadOp> {
  bool isWritable(Operation *op, Value value,
                  const AnalysisState &state) const {
    auto loadOp = cast<IREE::Flow::DispatchTensorLoadOp>(op);
    auto shapedType =
        loadOp.getSource().getType().dyn_cast<IREE::Flow::DispatchTensorType>();
    assert(shapedType && "unexpected source type");
    return shapedType.getAccess() != IREE::Flow::TensorAccess::ReadOnly;
  }

  LogicalResult bufferize(Operation *op, RewriterBase &rewriter,
                          const BufferizationOptions &options) const {
    auto loadOp = cast<IREE::Flow::DispatchTensorLoadOp>(op);
    auto tensorSubspanOp =
        loadOp.getSource()
            .getDefiningOp<IREE::HAL::InterfaceBindingSubspanOp>();
    assert(tensorSubspanOp && "expected that source is a SubspanOp");
    Value source = findOrCreateSubspanBuffer(rewriter, tensorSubspanOp);

    if (equalTensorShape(
            loadOp.getType(), loadOp.sizes(),
            loadOp.getSource().getType().cast<IREE::Flow::DispatchTensorType>(),
            loadOp.getSourceDims())) {
      // The entire tensor is loaded.
      replaceOpWithBufferizedValues(rewriter, op, source);
      return success();
    }

    // Bufferize to subview.
    auto subviewMemRefType = memref::SubViewOp::inferRankReducedResultType(
        loadOp.getType().getShape(), source.getType().cast<MemRefType>(),
        loadOp.getMixedOffsets(), loadOp.getMixedSizes(),
        loadOp.getMixedStrides());
    replaceOpWithNewBufferizedOp<memref::SubViewOp>(
        rewriter, op, subviewMemRefType.cast<MemRefType>(), source,
        loadOp.getMixedOffsets(), loadOp.getMixedSizes(),
        loadOp.getMixedStrides());

    return success();
  }
};

struct DispatchTensorStoreOpInterface
    : public BufferizableOpInterface::ExternalModel<
          DispatchTensorStoreOpInterface, IREE::Flow::DispatchTensorStoreOp> {
  bool bufferizesToMemoryRead(Operation *op, OpOperand &opOperand,
                              const AnalysisState &state) const {
    return true;
  }

  bool bufferizesToMemoryWrite(Operation *op, OpOperand &opOperand,
                               const AnalysisState &state) const {
    return false;
  }

  bufferization::AliasingOpResultList getAliasingOpResults(
      Operation *op, OpOperand &opOperand, const AnalysisState &state) const {
    return {};
  }

  LogicalResult bufferize(Operation *op, RewriterBase &rewriter,
                          const BufferizationOptions &options) const {
    auto storeOp = cast<IREE::Flow::DispatchTensorStoreOp>(op);
    auto tensorSubspanOp =
        storeOp.getTarget()
            .getDefiningOp<IREE::HAL::InterfaceBindingSubspanOp>();
    assert(tensorSubspanOp && "expected that target is a SubspanOp");
    Value target = findOrCreateSubspanBuffer(rewriter, tensorSubspanOp);

    if (!equalTensorShape(storeOp.getValue().getType().cast<RankedTensorType>(),
                          storeOp.getSizes(),
                          storeOp.getTarget()
                              .getType()
                              .cast<IREE::Flow::DispatchTensorType>(),
                          storeOp.getTargetDims())) {
      // Writing to a part of the tensor.
      auto subviewMemRefType =
          memref::SubViewOp::inferRankReducedResultType(
              storeOp.getValue().getType().cast<ShapedType>().getShape(),
              target.getType().cast<MemRefType>(), storeOp.getMixedOffsets(),
              storeOp.getMixedSizes(), storeOp.getMixedStrides())
              .cast<MemRefType>();

      target = rewriter.create<memref::SubViewOp>(
          storeOp->getLoc(), subviewMemRefType, target,
          storeOp.getMixedOffsets(), storeOp.getMixedSizes(),
          storeOp.getMixedStrides());
    }  // else: Writing the entire tensor, no subview required.

    auto maybeBuffer =
        getBuffer(rewriter, storeOp->getOpOperand(0).get(), options);
    if (failed(maybeBuffer)) return failure();
    Value srcMemref = *maybeBuffer;

    // If everything bufferized inplace, no copy is needed. We wrote to the
    // target buffer already. The copy folds away in that case.
    if (failed(options.createMemCpy(rewriter, storeOp->getLoc(), srcMemref,
                                    target)))
      return failure();

    rewriter.eraseOp(storeOp);
    return success();
  }
};
}  // namespace

/// Generic conversion for any LinalgExtOp on tensors.
static LogicalResult bufferizeLinalgExtOp(RewriterBase &rewriter,
                                          IREE::LinalgExt::LinalgExtOp op,
                                          const BufferizationOptions &options) {
  auto dspOp = dyn_cast<DestinationStyleOpInterface>(op.getOperation());
  if (!dspOp) {
    return op->emitOpError(
        "expected op to implement the `DestinationStyleOpInterface`");
  }

  // Take a guard before anything else.
  OpBuilder::InsertionGuard g(rewriter);
  rewriter.setInsertionPoint(op);

  // Nothing to do. This op is already bufferized.
  if (dspOp.hasBufferSemantics()) return success();

  // Ensure op has only tensors. Allow mixed tensor-buffer mode on a per-need
  // basis.
  if (!dspOp.hasTensorSemantics())
    return op->emitError() << "op does not have tensor semantics";

  // New input operands for the cloned op.
  SmallVector<Value> newInputBuffers;
  newInputBuffers.reserve(op.getNumInputs());
  for (OpOperand *opOperand : op.getInputOperands()) {
    if (dspOp.isScalar(opOperand)) {
      newInputBuffers.push_back(opOperand->get());
      continue;
    }
    auto maybeBuffer = getBuffer(rewriter, opOperand->get(), options);
    if (failed(maybeBuffer)) return failure();
    // Input operands are never written to.
    newInputBuffers.push_back(*maybeBuffer);
  }

  // New output operands for the cloned op.
  AnalysisState analysisState(options);
  SmallVector<Value> newOutputBuffers;
  for (OpResult opResult : op->getOpResults()) {
    SmallVector<OpOperand *> aliasingOpOperands =
        analysisState.getAliasingOpOperands(opResult);
    assert(aliasingOpOperands.size() == 1 && "expected 1 OpOperand");
    FailureOr<Value> resultBuffer =
        getBuffer(rewriter, aliasingOpOperands.front()->get(), options);
    if (failed(resultBuffer)) return failure();
    newOutputBuffers.push_back(*resultBuffer);
  }

  // Merge input/output operands.
  SmallVector<Value> newOperands = newInputBuffers;
  newOperands.append(newOutputBuffers.begin(), newOutputBuffers.end());

  // Append other operands besides ins and outs.
  for (auto nonDPSOperands : op.getNonInputOrOutputOperands()) {
    newOperands.push_back(nonDPSOperands->get());
  }

  // Set insertion point now that potential alloc/dealloc are introduced.
  rewriter.setInsertionPoint(op);
  // Clone the op, but use the new operands. Move the existing block into the
  // new op. Since the new op does not have any tensor results, it does not
  // return anything.
  auto newOp = cast<IREE::LinalgExt::LinalgExtOp>(mlir::cloneWithoutRegions(
      rewriter, op, /*resultTypes=*/TypeRange{}, newOperands));
  int64_t numRegions = op->getNumRegions();
  for (int64_t i = 0; i < numRegions; ++i) {
    rewriter.inlineRegionBefore(op->getRegion(i), newOp->getRegion(i),
                                newOp->getRegion(i).begin());
  }

  // Replace the results of the old op with the new output buffers.
  bufferization::replaceOpWithBufferizedValues(rewriter, op, newOutputBuffers);

  return success();
}

/// Bufferization of ops that implement the LinalgExtOp interface. Replace with
/// a new op that operates entirely on memrefs.
template <typename OpTy>
struct LinalgExtOpInterface
    : public BufferizableOpInterface::ExternalModel<LinalgExtOpInterface<OpTy>,
                                                    OpTy> {
  bool bufferizesToMemoryRead(Operation *op, OpOperand &opOperand,
                              const AnalysisState &state) const {
    // TODO: Implement payloadUsesValueFromOperand for individual ops. There
    // are a limited number of LinalgExt ops, so we hardcode them here. We don't
    // expect to add more LinalgExt ops.
    auto linalgExtOp = cast<IREE::LinalgExt::LinalgExtOp>(op);
    if (linalgExtOp.isInputTensor(&opOperand)) return true;
    return !isa<IREE::LinalgExt::ScatterOp, IREE::LinalgExt::ReverseOp>(op);
  }

  bool bufferizesToMemoryWrite(Operation *op, OpOperand &opOperand,
                               const AnalysisState &state) const {
    // Operand is written to if it has an aliasing OpResult.
    auto bufferizableOp = cast<BufferizableOpInterface>(op);
    return !bufferizableOp.getAliasingOpResults(opOperand, state).empty();
  }

  bufferization::AliasingOpOperandList getAliasingOpOperands(
      Operation *op, OpResult opResult, const AnalysisState &state) const {
    auto linalgExtOp = cast<IREE::LinalgExt::LinalgExtOp>(op);

    // The i-th OpResult may alias with the i-th "out" tensor.
    return {linalgExtOp.getOutputOperand(opResult.getResultNumber())};
  }

  bufferization::AliasingOpResultList getAliasingOpResults(
      Operation *op, OpOperand &opOperand, const AnalysisState &state) const {
    auto dspOp = cast<DestinationStyleOpInterface>(op);

    // The i-th "out" tensor may alias with the i-th OpResult.
    if (dspOp.isDpsInit(&opOperand)) return {dspOp.getTiedOpResult(&opOperand)};
    return {};
  }

  bufferization::BufferRelation bufferRelation(
      Operation *op, OpResult opResult, const AnalysisState &state) const {
    return bufferization::BufferRelation::Equivalent;
  }

  LogicalResult bufferize(Operation *op, RewriterBase &rewriter,
                          const BufferizationOptions &options) const {
    return bufferizeLinalgExtOp(
        rewriter, cast<IREE::LinalgExt::LinalgExtOp>(op), options);
  }
};

//===----------------------------------------------------------------------===//
// IREE specific post analysis transformations.
//===----------------------------------------------------------------------===//

/// Returns true if the value of a `storeOp` bufferizes to an equivalent
/// DispatchTensorLoadOp result that bufferizes inplace.
static bool isValueEquivalentToAnInplaceTensorLoadOp(
    const BufferizationAliasInfo &aliasInfo,
    IREE::Flow::DispatchTensorStoreOp storeOp) {
  bool foundOp = false;
  aliasInfo.applyOnEquivalenceClass(storeOp.getValue(), [&](Value value) {
    auto loadOp = value.getDefiningOp<IREE::Flow::DispatchTensorLoadOp>();
    // TODO: Assert that offsets, sizes and strides are the same.
    if (loadOp &&
        aliasInfo.areEquivalentBufferizedValues(loadOp.getResult(),
                                                storeOp.getValue()) &&
        loadOp.getSource() == storeOp.getTarget()) {
      foundOp = true;
    }
  });

  return foundOp;
}

/// Try to eliminate tensor::EmptyOps that are eventually fed into a
/// DispatchTensorStoreOp. Such tensor::EmptyOps are replaced with matching
/// DispatchTensorLoadOps. Two conditions must be met:
///
/// * The target must be a "readwrite" tensor.
/// * All ops along the reverse SSA use-def chain from the
///   DispatchTensorStoreOp to the tensor::EmptyOp must have bufferized
///   in-place.
LogicalResult storeTensorOpAnchoredEmptyTensorEliminationStep(
    RewriterBase &rewriter, Operation *op, AnalysisState &state) {
  return eliminateEmptyTensors(
      rewriter, op, state,
      /*anchorMatchFunc=*/
      [&](OpOperand &operand, SmallVector<Value> &neededValues) {
        auto storeOp =
            dyn_cast<IREE::Flow::DispatchTensorStoreOp>(operand.getOwner());
        if (!storeOp) return false;
        neededValues.push_back(storeOp.getTarget());
        neededValues.append(storeOp.getTargetDims().begin(),
                            storeOp.getTargetDims().end());
        neededValues.append(storeOp.getOffsets().begin(),
                            storeOp.getOffsets().end());
        neededValues.append(storeOp.getSizes().begin(),
                            storeOp.getSizes().end());
        neededValues.append(storeOp.getStrides().begin(),
                            storeOp.getStrides().end());
        return true;
      },
      /*rewriteFunc=*/
      [](OpBuilder &b, Location loc, OpOperand &operand) {
        auto storeOp =
            cast<IREE::Flow::DispatchTensorStoreOp>(operand.getOwner());
        auto loadOp = b.create<IREE::Flow::DispatchTensorLoadOp>(
            loc, storeOp.getValue().getType().cast<RankedTensorType>(),
            storeOp.getTarget(), storeOp.getTargetDims(),
            storeOp.getMixedOffsets(), storeOp.getMixedSizes(),
            storeOp.getMixedStrides());
        return loadOp.getResult();
      });
}

void registerBufferizationInterfaces(DialectRegistry &registry) {
  arith::registerBufferizableOpInterfaceExternalModels(registry);
  linalg::registerBufferizableOpInterfaceExternalModels(registry);
  scf::registerBufferizableOpInterfaceExternalModels(registry);
  bufferization::func_ext::registerBufferizableOpInterfaceExternalModels(
      registry);
  tensor::registerBufferizableOpInterfaceExternalModels(registry);
  vector::registerBufferizableOpInterfaceExternalModels(registry);

  // Register IREE operations.
  registry.addExtension(
      +[](MLIRContext *ctx, IREE::Flow::FlowDialect *dialect) {
        IREE::Flow::DispatchTensorLoadOp::attachInterface<
            DispatchTensorLoadOpInterface>(*ctx);
        IREE::Flow::DispatchTensorStoreOp::attachInterface<
            DispatchTensorStoreOpInterface>(*ctx);
      });
  registry.addExtension(+[](MLIRContext *ctx,
                            IREE::LinalgExt::IREELinalgExtDialect *dialect) {
    IREE::LinalgExt::FftOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::FftOp>>(*ctx);
    IREE::LinalgExt::PackOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::PackOp>>(*ctx);
    IREE::LinalgExt::UnPackOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::UnPackOp>>(*ctx);
    IREE::LinalgExt::ReverseOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::ReverseOp>>(*ctx);
    IREE::LinalgExt::ScanOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::ScanOp>>(*ctx);
    IREE::LinalgExt::ScatterOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::ScatterOp>>(*ctx);
    IREE::LinalgExt::SortOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::SortOp>>(*ctx);
    IREE::LinalgExt::TopkOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::TopkOp>>(*ctx);
    IREE::LinalgExt::WinogradInputTransformOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::WinogradInputTransformOp>>(*ctx);
    IREE::LinalgExt::WinogradOutputTransformOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::WinogradOutputTransformOp>>(*ctx);
    IREE::LinalgExt::SoftmaxOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::SoftmaxOp>>(*ctx);
    IREE::LinalgExt::AttentionOp::attachInterface<
        LinalgExtOpInterface<IREE::LinalgExt::AttentionOp>>(*ctx);
  });
}

}  // namespace iree_compiler
}  // namespace mlir
