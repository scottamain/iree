// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/Utils/GPUUtils.h"

#include "iree/compiler/Codegen/Utils/MarkerUtils.h"
#include "llvm/Support/Debug.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#define DEBUG_TYPE "iree-codegen-gpu-utils"

static constexpr unsigned kShuffleBitWidth = 32;

namespace mlir {
namespace iree_compiler {

//===----------------------------------------------------------------------===//
// GPU processor IDs and sizes
//===----------------------------------------------------------------------===//

llvm::SmallVector<mlir::linalg::ProcInfo, 2> getGPUThreadIdsAndCounts(
    mlir::OpBuilder &builder, mlir::Location loc, unsigned numDims,
    llvm::ArrayRef<int64_t> workgroupSize) {
  assert(numDims <= kNumGPUDims);
  llvm::SmallVector<mlir::linalg::ProcInfo, 2> procInfo(numDims);
  std::array<gpu::Dimension, kNumGPUDims> dimAttr{
      gpu::Dimension::x, gpu::Dimension::y, gpu::Dimension::z};
  mlir::Type indexType = builder.getIndexType();
  for (unsigned i = 0; i < numDims; ++i) {
    procInfo[numDims - 1 - i] = {
        builder.create<mlir::gpu::ThreadIdOp>(loc, indexType, dimAttr[i]),
        builder.create<mlir::arith::ConstantOp>(
            loc, builder.getIndexAttr(workgroupSize[i])),
        linalg::DistributionMethod::Cyclic};
  }
  return procInfo;
}

llvm::SmallVector<mlir::linalg::ProcInfo, 2> getSubgroupIdsAndCounts(
    mlir::OpBuilder &builder, mlir::Location loc, unsigned warpSize,
    unsigned numDims, llvm::ArrayRef<int64_t> numSubgroups) {
  assert(numDims <= kNumGPUDims);
  llvm::SmallVector<mlir::linalg::ProcInfo, 2> procInfo(numDims);
  std::array<gpu::Dimension, kNumGPUDims> dimAttr{
      gpu::Dimension::x, gpu::Dimension::y, gpu::Dimension::z};
  mlir::Type indexType = builder.getIndexType();
  for (unsigned i = 0; i < numDims; ++i) {
    mlir::Value subgroupId =
        builder.create<mlir::gpu::ThreadIdOp>(loc, indexType, dimAttr[i]);
    if (i == 0) {
      mlir::AffineExpr d0 = builder.getAffineDimExpr(0);
      subgroupId = mlir::makeComposedAffineApply(
          builder, loc, d0.floorDiv(builder.getAffineConstantExpr(warpSize)),
          {subgroupId});
    }
    procInfo[numDims - 1 - i] = {
        subgroupId,
        builder.create<mlir::arith::ConstantOp>(
            loc, builder.getIndexAttr(numSubgroups[i])),
        linalg::DistributionMethod::Cyclic};
  }
  return procInfo;
}

std::array<int64_t, 3> getWorkgroupSize(mlir::func::FuncOp funcOp) {
  std::array<int64_t, 3> workgroupSize;
  FailureOr<IREE::HAL::ExecutableExportOp> exportOp =
      mlir::iree_compiler::getEntryPoint(funcOp);
  llvm::Optional<mlir::ArrayAttr> workgroupSizeAttr =
      exportOp->getWorkgroupSize();
  assert(workgroupSizeAttr.has_value());
  for (auto [index, attr] : llvm::enumerate(workgroupSizeAttr.value())) {
    workgroupSize[index] =
        attr.cast<mlir::IntegerAttr>().getValue().getZExtValue();
  }
  return workgroupSize;
}

//===----------------------------------------------------------------------===//
// GPU vectorization
//===----------------------------------------------------------------------===//

bool canPerformVectorAccessUsingAllThreads(ArrayRef<int64_t> shape,
                                           int64_t threadCount,
                                           int64_t vectorSize) {
  // Verify that each dimension of the shape can be distributed on the
  // threads
  int64_t threadsAvailable = threadCount;
  for (auto &[index, dim] : llvm::enumerate(llvm::reverse(shape))) {
    int64_t numElementPerThread = index == 0 ? vectorSize : 1;
    int64_t numThreads = dim / numElementPerThread;
    if (numThreads == 0) return false;
    if (numThreads > threadsAvailable) {
      // If there are no enough remaining threads to distribute the current
      // dimension, try to use all remaining threads. But we still need to make
      // sure all work can be distributed to these threads evenly.
      if (numThreads % threadsAvailable != 0) return false;
      numThreads = threadsAvailable;
    }
    if (threadsAvailable % numThreads != 0) return false;
    threadsAvailable = threadsAvailable / numThreads;
    if (threadsAvailable == 1) break;
  }
  return threadsAvailable == 1;
}

//===----------------------------------------------------------------------===//
// GPU workgroup memory
//===----------------------------------------------------------------------===//

Optional<Value> allocateWorkgroupMemory(OpBuilder &builder,
                                        memref::SubViewOp subview,
                                        ArrayRef<Value> sizeBounds,
                                        DataLayout &) {
  OpBuilder::InsertionGuard guard(builder);

  func::FuncOp funcOp = subview->getParentOfType<func::FuncOp>();
  if (!funcOp) return std::nullopt;

  // The subview size bounds are expected to be constant; they specify the shape
  // of the allocation.
  SmallVector<int64_t, 2> shape;
  for (Value bound : sizeBounds) {
    APInt value;
    if (!matchPattern(bound, m_ConstantInt(&value))) return std::nullopt;
    shape.push_back(value.getSExtValue());
  }

  builder.setInsertionPoint(&funcOp.front(), funcOp.front().begin());
  auto type = MemRefType::get(
      shape, subview.getType().getElementType(), MemRefLayoutAttrInterface{},
      gpu::AddressSpaceAttr::get(builder.getContext(),
                                 gpu::GPUDialect::getWorkgroupAddressSpace()));
  Value buffer = builder.create<memref::AllocOp>(funcOp.getLoc(), type);
  return buffer;
}

LogicalResult deallocateWorkgroupMemory(OpBuilder &, Value /*buffer*/) {
  return success();
}

LogicalResult copyToWorkgroupMemory(OpBuilder &b, Value src, Value dst) {
  Operation *copyOp = b.create<memref::CopyOp>(src.getLoc(), src, dst);
  setMarker(copyOp, getCopyToWorkgroupMemoryMarker());
  return success();
}

static bool propagateCopyDestIntoProducerFill(memref::CopyOp copyOp) {
  // Look for a fill Op writing into the copyOp source.
  Operation *prevOp = copyOp->getPrevNode();
  while (prevOp) {
    if (isMemoryEffectFree(prevOp)) {
      prevOp = prevOp->getPrevNode();
      continue;
    }

    auto fillOp = dyn_cast<linalg::FillOp>(prevOp);
    if (!fillOp) break;
    if (fillOp.output() != copyOp.getSource()) break;
    // Move the fillOp and change the destination to the copy destination.
    fillOp->moveBefore(copyOp);
    fillOp.getOutputsMutable().assign(copyOp.getTarget());
    return true;
  }
  return false;
}

// Split input/output operand from copy from shared memory into a separate
// input.
static void insertInputValueIntoGeneric(Value source, linalg::GenericOp op) {
  SmallVector<Value> newOperands;
  SmallVector<AffineMap> maps;
  for (OpOperand *in : op.getDpsInputOperands()) {
    newOperands.push_back(in->get());
    maps.push_back(op.getMatchingIndexingMap(in));
  }
  newOperands.push_back(source);
  assert(op.getNumDpsInits() == 1);
  OpOperand *outOperand = op.getDpsInitOperand(0);
  maps.push_back(op.getMatchingIndexingMap(outOperand));
  maps.push_back(op.getMatchingIndexingMap(outOperand));
  Location loc = op.getLoc();
  SmallVector<utils::IteratorType> iterTypes(op.getNumLoops(),
                                             utils::IteratorType::parallel);
  OpBuilder builder(op);
  auto newOp = builder.create<linalg::GenericOp>(
      loc, newOperands, outOperand->get(), maps, iterTypes);
  newOp.getRegion().getBlocks().splice(newOp.getRegion().begin(),
                                       op.getRegion().getBlocks());

  Block &payload = newOp.getRegion().front();
  payload.addArgument(payload.getArguments().back().getType(), loc);
  setMarker(newOp, getCopyToWorkgroupMemoryMarker());
}

/// Propagate the shared memory copy into the consumer op if it's a fully
/// parallel linalg.generic.
static bool propagateCopySourceIntoConsumerGeneric(
    memref::CopyOp copyOp, SmallVector<Operation *> &toDelete) {
  // Look for a generic Op reading the copyOp target.
  Operation *nextOp = copyOp->getNextNode();
  while (nextOp) {
    if (isMemoryEffectFree(nextOp)) {
      nextOp = nextOp->getNextNode();
      continue;
    }
    auto consumer = dyn_cast<linalg::GenericOp>(nextOp);
    if (!consumer || consumer.getNumDpsInits() != 1 ||
        !consumer.getMatchingIndexingMap(consumer.getDpsInitOperand(0))
             .isIdentity())
      break;
    if (*consumer.getOutputs().begin() != copyOp.getTarget()) break;
    insertInputValueIntoGeneric(copyOp.getSource(), consumer);
    toDelete.push_back(consumer);
    return true;
  }
  return false;
}

/// This is needed because we are doing promotion to shared memory on buffers.
/// This is a fragile and temporary solution until we move to be able to do this
/// kind of transformations on tensors.
void propagateSharedMemoryCopy(func::FuncOp funcOp) {
  SmallVector<Operation *> toDelete;
  funcOp.walk([&toDelete](memref::CopyOp copyOp) {
    if (hasMarker(copyOp, getCopyToWorkgroupMemoryMarker())) {
      if (propagateCopyDestIntoProducerFill(copyOp) ||
          propagateCopySourceIntoConsumerGeneric(copyOp, toDelete))
        toDelete.push_back(copyOp.getOperation());
    }
  });
  for (Operation *op : toDelete) op->erase();
}

void insertBarriersAroundSharedMemoryCopy(func::FuncOp funcOp) {
  OpBuilder builder(funcOp.getContext());
  // Insert barriers before and after copies to workgroup memory and skip
  // insert barriers between back to back copy to workgroup memory.
  funcOp.walk([&builder](Operation *copyOp) {
    if (hasMarker(copyOp, getCopyToWorkgroupMemoryMarker())) {
      Operation *prevOp = copyOp->getPrevNode();
      if (!prevOp || !hasMarker(prevOp, getCopyToWorkgroupMemoryMarker())) {
        builder.setInsertionPoint(copyOp);
        builder.create<gpu::BarrierOp>(copyOp->getLoc());
      }
      Operation *nextOp = copyOp->getNextNode();
      if (!nextOp || !hasMarker(nextOp, getCopyToWorkgroupMemoryMarker())) {
        builder.setInsertionPointAfter(copyOp);
        builder.create<gpu::BarrierOp>(copyOp->getLoc());
      }
    }
  });
}

//===----------------------------------------------------------------------===//
// Reduction utils
//===----------------------------------------------------------------------===//

/// Packs scalar element to it's vector equivalent.
/// (i.e f16 -> vector<1xf16> and f32 -> vector<1xf32>)
static Value promoteElementToVector(Location loc, OpBuilder &builder,
                                    Value input) {
  VectorType vectorTypeBroadcast = VectorType::get({1}, input.getType());
  Value vectorInput =
      builder.create<vector::BroadcastOp>(loc, vectorTypeBroadcast, input);
  return vectorInput;
}

/// Packs vector of lower precision into a single 32-bit width element.
/// (i.e <2xf16> -> i32 and <4xi8> -> i32)
static Value packVectorToSupportedWidth(Location loc, OpBuilder &builder,
                                        Value input) {
  LLVM_DEBUG({
    auto vecType = input.getType().cast<VectorType>();
    Type elementType = vecType.getElementType();
    assert(vecType.getDimSize(0) * elementType.getIntOrFloatBitWidth() ==
               kShuffleBitWidth &&
           "vecSize * vecBitWidth needs to packable into 32-bitwidth.");
    assert(elementType.isIntOrFloat() &&
           "Only int and float packing is supported.");
  });
  VectorType packed32Type = VectorType::get({1}, builder.getI32Type());
  Value packedInputVec =
      builder.create<vector::BitCastOp>(loc, packed32Type, input);
  Value packedInput = builder.create<vector::ExtractOp>(loc, packedInputVec, 0);
  return packedInput;
}

/// Unpack single scalar element into a target vector type.
/// (i.e i32 -> vector<4xi8> or f32 -> vector<2xf16>)
static Value unpackToVector(Location loc, OpBuilder &builder, Value packedInput,
                            VectorType targetVecType) {
  LLVM_DEBUG({
    Type packedType = packedInput.getType();
    assert(packedType.isIntOrFloat() && "Only ints and floats are unpackable.");
    Type elementType = targetVecType.getElementType();
    assert(targetVecType.getDimSize(0) * elementType.getIntOrFloatBitWidth() ==
               packedType.getIntOrFloatBitWidth() &&
           "packed width needs to be unpackable to vecSize * vecBitWidth.");
  });
  Value packedVector = promoteElementToVector(loc, builder, packedInput);
  Value unpackedVector =
      builder.create<vector::BitCastOp>(loc, targetVecType, packedVector);
  return unpackedVector;
}

/// Emit warp reduction code sequence for a given input.
static Value warpReduction(Location loc, OpBuilder &builder, Value input,
                           vector::CombiningKind kind, uint32_t warpSize,
                           uint32_t numLaneToReduce) {
  VectorType unpackedType = input.getType().dyn_cast<VectorType>();
  Value laneVal = input;
  assert(llvm::isPowerOf2_32(numLaneToReduce));
  // Parallel reduction using butterfly shuffles.
  for (uint64_t i = 1; i < numLaneToReduce; i <<= 1) {
    Value shuffleInput = laneVal;
    if (unpackedType) {
      shuffleInput = packVectorToSupportedWidth(loc, builder, laneVal);
    }
    Value shuffled = builder
                         .create<gpu::ShuffleOp>(loc, shuffleInput, i,
                                                 /*width=*/warpSize,
                                                 /*mode=*/gpu::ShuffleMode::XOR)
                         .getShuffleResult();
    if (unpackedType) {
      shuffled = unpackToVector(loc, builder, shuffled, unpackedType);
    }
    laneVal = makeArithReduction(builder, loc, kind, laneVal, shuffled);
  }
  // Broadcast the result to all the lanes.
  if (warpSize != numLaneToReduce) {
    if (unpackedType) {
      laneVal = packVectorToSupportedWidth(loc, builder, laneVal);
    }
    laneVal = builder
                  .create<gpu::ShuffleOp>(loc, laneVal, 0,
                                          /*width=*/warpSize,
                                          /*mode=*/gpu::ShuffleMode::IDX)
                  .getShuffleResult();
    if (unpackedType) {
      laneVal = unpackToVector(loc, builder, laneVal, unpackedType);
    }
  }
  return laneVal;
}

// List of identity elements by operation.
// https://en.wikipedia.org/wiki/Identity_element
static Attribute getCombiningKindIdentity(OpBuilder &builder,
                                          vector::CombiningKind combiningKind,
                                          Type type) {
  switch (combiningKind) {
    case vector::CombiningKind::ADD:
      return builder.getZeroAttr(type);
    case vector::CombiningKind::MUL: {
      if (type.isIntOrIndex()) {
        return builder.getIntegerAttr(type, 1);
      }
      return builder.getFloatAttr(type, 1);
    }
    case vector::CombiningKind::MINUI:
    case vector::CombiningKind::MINSI:
      return builder.getIntegerAttr(type, std::numeric_limits<int64_t>::max());
    case vector::CombiningKind::MAXUI:
    case vector::CombiningKind::MAXSI:
      return builder.getIntegerAttr(type, std::numeric_limits<int64_t>::min());
    case vector::CombiningKind::AND:
      return builder.getIntegerAttr(type, 1);
    case vector::CombiningKind::OR:
    case vector::CombiningKind::XOR:
      return builder.getZeroAttr(type);
    case vector::CombiningKind::MINF: {
      auto posInfApFloat = APFloat::getInf(
          type.cast<FloatType>().getFloatSemantics(), /*Negative=*/false);
      return builder.getFloatAttr(type, posInfApFloat);
    }
    case vector::CombiningKind::MAXF: {
      auto negInfApFloat = APFloat::getInf(
          type.cast<FloatType>().getFloatSemantics(), /*Negative=*/true);
      return builder.getFloatAttr(type, negInfApFloat);
    }
  }
  return Attribute();
}

/// Compute the value on a single thread to get per lane reduction value.
/// If bit-width is not supported on shuffle operations, and a lower precision,
/// we represent them as a vector S.T we can pack them into a single 32-bit
/// width for shuffles.
static Value reduceToSupportedWidth(Location loc, OpBuilder &builder,
                                    Value input, vector::CombiningKind kind) {
  auto vecType = input.getType().cast<VectorType>();
  Type elementType = vecType.getElementType();
  int64_t vecSize = vecType.getDimSize(0);
  unsigned bitWidth = elementType.getIntOrFloatBitWidth();
  // Simply reduce if it's already 32 bits.
  if (bitWidth == kShuffleBitWidth) {
    return builder.create<vector::ReductionOp>(loc, kind, input);
  }
  assert(kShuffleBitWidth % bitWidth == 0 &&
         "Bitwidth needs to be able to be packed into shuffle-bitwidth.");
  int64_t unrollCount = kShuffleBitWidth / bitWidth;
  // Original size needs to be divisble by or less than unroll count to
  // determine slice size.
  assert(vecSize % unrollCount == 0 || vecSize < unrollCount);
  unsigned sliceSize = vecSize / unrollCount;
  VectorType unrolledLaneValType = VectorType::get({unrollCount}, elementType);
  Value perLaneReduction = builder.create<arith::ConstantOp>(
      loc, builder.getZeroAttr(unrolledLaneValType));
  if (vecSize % unrollCount == 0) {
    // Unroll reductions s.t we can pack into a supported 32-bitWidth format.
    for (int64_t i = 0; i < unrollCount; i++) {
      Value laneValSlice = builder.create<vector::ExtractStridedSliceOp>(
          loc, input,
          /*offsets=*/ArrayRef<int64_t>{sliceSize * i},
          /*sizes=*/ArrayRef<int64_t>{sliceSize},
          /*strides=*/ArrayRef<int64_t>{1});
      Value reductionSlice =
          builder.create<vector::ReductionOp>(loc, kind, laneValSlice);
      SmallVector<int64_t> perLaneUnrollId = {i};
      perLaneReduction = builder.create<vector::InsertOp>(
          loc, reductionSlice, perLaneReduction, perLaneUnrollId);
    }
  } else {
    // In cases where vecSize < unrollCount, we would pad the vector
    // with identity elements until it's total bit size is 32.
    Attribute identityAttr =
        getCombiningKindIdentity(builder, kind, elementType);
    identityAttr = DenseElementsAttr::get(unrolledLaneValType, identityAttr);
    Value identity = builder.create<arith::ConstantOp>(loc, identityAttr,
                                                       unrolledLaneValType);
    perLaneReduction = builder.create<vector::InsertStridedSliceOp>(
        loc, input, identity, /*offsets=*/ArrayRef<int64_t>{0},
        /*strides=*/ArrayRef<int64_t>{1});
  }
  return perLaneReduction;
}

/// Emit identity variable.
static Value getCombiningIdentityValue(Location loc, OpBuilder &builder,
                                       vector::CombiningKind kind,
                                       Type identityType) {
  auto vectorType = identityType.dyn_cast<VectorType>();
  Type elementType = identityType;
  if (vectorType) {
    elementType = vectorType.getElementType();
  }
  Attribute identityAttr = getCombiningKindIdentity(builder, kind, elementType);
  if (vectorType) {
    identityAttr = DenseElementsAttr::get(vectorType, identityAttr);
  }
  assert(identityAttr && "Unknown identity value for the reduction");
  Value identity =
      builder.create<arith::ConstantOp>(loc, identityAttr, identityType);
  return identity;
}

/// Emit reduction across a group for a given input.
Value emitGPUGroupReduction(Location loc, OpBuilder &builder, Value input,
                            vector::CombiningKind kind, uint32_t size,
                            const int warpSize) {
  assert(
      size % warpSize == 0 &&
      "Group reduction only support for sizes aligned on warp size for now.");
  // First reduce on a single thread to get per lane reduction value.
  Value laneVal = reduceToSupportedWidth(loc, builder, input, kind);
  laneVal = warpReduction(loc, builder, laneVal, kind, warpSize, warpSize);
  // if we have more than one warp, reduce across warps.
  if (size > warpSize) {
    uint32_t numWarp = size / warpSize;
    assert(numWarp <= warpSize &&
           "Only support 1 level, need to implement recursive/loop for this "
           "case.");
    auto addressSpaceAttr = gpu::AddressSpaceAttr::get(
        builder.getContext(), gpu::GPUDialect::getWorkgroupAddressSpace());
    MemRefType memrefType =
        MemRefType::get(numWarp, laneVal.getType(), MemRefLayoutAttrInterface{},
                        addressSpaceAttr);
    Value alloc = builder.create<memref::AllocOp>(loc, memrefType);
    Value threadX = builder.create<gpu::ThreadIdOp>(loc, builder.getIndexType(),
                                                    gpu::Dimension::x);
    Value cstWarpSize = builder.create<arith::ConstantIndexOp>(loc, warpSize);
    Value warpId = builder.create<arith::DivUIOp>(loc, threadX, cstWarpSize);
    Value laneId = builder.create<arith::RemUIOp>(loc, threadX, cstWarpSize);
    Value zero = builder.create<arith::ConstantIndexOp>(loc, 0);
    Value lane0 = builder.create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq,
                                                laneId, zero);
    // Store the reduction for each warp.
    SmallVector<Value> indices = {warpId};
    builder.create<scf::IfOp>(loc, lane0, [&](OpBuilder &b, Location l) {
      b.create<memref::StoreOp>(l, laneVal, alloc, indices);
      b.create<scf::YieldOp>(l, std::nullopt);
    });
    builder.create<gpu::BarrierOp>(loc);
    // Further reduce the outputs from each warps with a single warp reduce.
    Value memrefSize = builder.create<arith::ConstantIndexOp>(loc, numWarp - 1);
    Value laneIdInBounds =
        builder.create<arith::MinUIOp>(loc, laneId, memrefSize);
    Value loadVal = builder.create<memref::LoadOp>(loc, alloc, laneIdInBounds);
    Value cstNumWarp = builder.create<arith::ConstantIndexOp>(loc, numWarp);
    if (!llvm::isPowerOf2_32(numWarp)) {
      // Pad with identity element if numel < warpSize for valid warp reduction.
      Value useIdentityElement = builder.create<arith::CmpIOp>(
          loc, arith::CmpIPredicate::sge, laneId, cstNumWarp);
      numWarp = llvm::PowerOf2Ceil(numWarp);
      Value identity =
          getCombiningIdentityValue(loc, builder, kind, loadVal.getType());
      loadVal = builder.create<arith::SelectOp>(loc, useIdentityElement,
                                                identity, loadVal);
    }
    laneVal = warpReduction(loc, builder, loadVal, kind, warpSize, numWarp);
  }
  // Handles cases for sub-32bit precision where output is still in vector form.
  if (laneVal.getType().isa<VectorType>()) {
    laneVal = builder.create<vector::ReductionOp>(loc, kind, laneVal);
  }
  return laneVal;
}

Optional<SmallVector<int64_t>> getWmmaNativeVectorSize(Operation *op) {
  // Currently hardcode the size of wmma operation. When more cases are
  // supported this should be picked based on what the backend supports.
  int64_t m = 16;
  int64_t n = 16;
  if (auto contract = dyn_cast<vector::ContractionOp>(op)) {
    int64_t k = contract.getLhsType().getElementType().isF16() ? 16 : 8;
    SmallVector<int64_t> nativeSize(contract.getIteratorTypes().size() - 3, 1);
    nativeSize.append({m, n, k});
    return nativeSize;
  }
  if (auto writeOp = dyn_cast<vector::TransferWriteOp>(op)) {
    SmallVector<int64_t> nativeSize(writeOp.getVectorType().getRank() - 2, 1);
    nativeSize.append({m, n});
    return nativeSize;
  }
  if (auto readOp = dyn_cast<vector::TransferReadOp>(op)) {
    // Transfer read ops may need different shapes based on how they are being
    // used. For simplicity just match the shape used by the extract strided op.
    VectorType sliceType;
    for (Operation *users : op->getUsers()) {
      auto extract = dyn_cast<vector::ExtractStridedSliceOp>(users);
      if (!extract) return std::nullopt;
      auto vecType = extract.getResult().getType().cast<VectorType>();
      if (sliceType && sliceType != vecType) return std::nullopt;
      sliceType = vecType;
    }
    return llvm::to_vector<>(sliceType.getShape());
  }
  if ((OpTrait::hasElementwiseMappableTraits(op) && op->getNumResults() == 1)) {
    if (auto vecType = op->getResultTypes()[0].dyn_cast<VectorType>()) {
      SmallVector<int64_t> nativeSize(vecType.getRank() - 2, 1);
      // Map elementwise ops to the output shape.
      nativeSize.append({m, n});
      return nativeSize;
    }
  }
  return std::nullopt;
}

}  // namespace iree_compiler
}  // namespace mlir
