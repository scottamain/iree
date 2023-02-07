// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/PassDetail.h"
#include "iree/compiler/Codegen/Passes.h"
#include "iree/compiler/Codegen/Utils/Utils.h"
#include "mlir/Dialect/GPU/Transforms/Passes.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"

namespace mlir {
namespace iree_compiler {

/// Padd out the inner dimension of the allocOp in order reduce the chances to
/// have bank conflicts when reading 2D shapes within shared memory.
static void padAlloc(memref::AllocOp allocOp, int64_t paddingSizeBits) {
  int64_t innerDim = allocOp.getType().getShape().back();
  if (ShapedType::isDynamic(innerDim)) return;
  Type elType = allocOp.getType().getElementType();
  unsigned bitwidth =
      mlir::DataLayout::closest(allocOp).getTypeSizeInBits(elType);
  // Pad with 128bits==16bytes so that accesses are still aligned on 16bytes.
  int64_t paddingSize = paddingSizeBits / bitwidth;
  SmallVector<int64_t> shape = llvm::to_vector(allocOp.getType().getShape());
  shape.back() = shape.back() + paddingSize;
  MemRefType allocType =
      MemRefType::get(shape, elType, MemRefLayoutAttrInterface{},
                      allocOp.getType().getMemorySpace());
  OpBuilder builder(allocOp);
  Location loc = allocOp.getLoc();
  Value paddedAlloc = builder.create<memref::AllocOp>(loc, allocType);
  SmallVector<int64_t> offsets(shape.size(), 0);
  SmallVector<int64_t> strides(shape.size(), 1);
  Value subview = builder.create<memref::SubViewOp>(
      loc, paddedAlloc, offsets, allocOp.getType().getShape(), strides);
  replaceMemrefUsesAndPropagateType(allocOp, subview, builder);
  allocOp->erase();
}

namespace {

/// Pass to reduce the number of bank conflicts when accessing shared memory in
/// a 2D manner. This is a simple version just padding allocation.
/// This doesn't fully remove bank conflicts and increase the shared memory
/// usage. In order to get better memory access patterns we should do shared
/// memory swizzling which requires more complex transformations. This pass can
/// be removed once the better solution is implemented.
struct GPUReduceBankConflictsPass
    : public GPUReduceBankConflictsBase<GPUReduceBankConflictsPass> {
 private:
  int64_t paddingSizeBits;

 public:
  GPUReduceBankConflictsPass(int64_t paddingSizeBits)
      : paddingSizeBits(paddingSizeBits) {}

  void runOnOperation() override {
    auto funcOp = getOperation();
    SmallVector<memref::AllocOp> sharedMemAllocs;
    // Collect all the alloc operations.
    funcOp.walk([&](memref::AllocOp allocOp) {
      auto addressSpaceAttr = allocOp.getType()
                                  .getMemorySpace()
                                  .dyn_cast_or_null<gpu::AddressSpaceAttr>();
      if (addressSpaceAttr &&
          addressSpaceAttr.getValue() ==
              gpu::GPUDialect::getWorkgroupAddressSpace() &&
          allocOp.getType().hasStaticShape()) {
        sharedMemAllocs.push_back(allocOp);
      }
    });
    for (memref::AllocOp alloc : sharedMemAllocs)
      padAlloc(alloc, paddingSizeBits);
  }
};
}  // namespace

std::unique_ptr<OperationPass<func::FuncOp>>
createGPUReduceSharedMemoryBankConflicts(int64_t paddingSizeBits) {
  return std::make_unique<GPUReduceBankConflictsPass>(paddingSizeBits);
}

}  // namespace iree_compiler
}  // namespace mlir
