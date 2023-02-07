// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/PassDetail.h"
#include "iree/compiler/Codegen/Passes.h"
#include "iree/compiler/Codegen/SPIRV/Utils.h"
#include "iree/compiler/Dialect/HAL/IR/HALTypes.h"
#include "mlir/Conversion/MemRefToSPIRV/MemRefToSPIRV.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVDialect.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVEnums.h"
#include "mlir/Dialect/SPIRV/IR/TargetAndABI.h"
#include "mlir/IR/Attributes.h"
#include "mlir/Transforms/DialectConversion.h"

#define DEBUG_TYPE "iree-spirv-map-memref-storage-class"

namespace mlir {
namespace iree_compiler {
namespace {

Optional<spirv::StorageClass> mapHALDescriptorTypeForVulkan(Attribute attr) {
  auto dtAttr = attr.dyn_cast_or_null<IREE::HAL::DescriptorTypeAttr>();
  if (dtAttr) {
    switch (dtAttr.getValue()) {
      case IREE::HAL::DescriptorType::UniformBuffer:
        return spirv::StorageClass::Uniform;
      case IREE::HAL::DescriptorType::StorageBuffer:
        return spirv::StorageClass::StorageBuffer;
      default:
        return std::nullopt;
    }
  }
  if (auto gpuAttr = attr.dyn_cast_or_null<gpu::AddressSpaceAttr>()) {
    switch (gpuAttr.getValue()) {
      case gpu::AddressSpace::Workgroup:
        return spirv::StorageClass::Workgroup;
      default:
        return std::nullopt;
    }
  };
  return spirv::mapMemorySpaceToVulkanStorageClass(attr);
}

Optional<spirv::StorageClass> mapHALDescriptorTypeForOpenCL(Attribute attr) {
  auto dtAttr = attr.dyn_cast_or_null<IREE::HAL::DescriptorTypeAttr>();
  if (!dtAttr) return spirv::mapMemorySpaceToOpenCLStorageClass(attr);
  switch (dtAttr.getValue()) {
    case IREE::HAL::DescriptorType::UniformBuffer:
      return spirv::StorageClass::Uniform;
    case IREE::HAL::DescriptorType::StorageBuffer:
      return spirv::StorageClass::CrossWorkgroup;
  }
  return std::nullopt;
}

struct SPIRVMapMemRefStorageClassPass final
    : public SPIRVMapMemRefStorageClassBase<SPIRVMapMemRefStorageClassPass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<spirv::SPIRVDialect>();
  }

  void runOnOperation() override {
    MLIRContext *context = &getContext();
    Operation *op = getOperation();

    spirv::MemorySpaceToStorageClassMap memorySpaceMap;

    if (spirv::TargetEnvAttr attr = getSPIRVTargetEnvAttr(op)) {
      spirv::TargetEnv targetEnv(attr);
      if (targetEnv.allows(spirv::Capability::Shader)) {
        memorySpaceMap = mapHALDescriptorTypeForVulkan;
      } else if (targetEnv.allows(spirv::Capability::Kernel)) {
        memorySpaceMap = mapHALDescriptorTypeForOpenCL;
      }
    }
    if (!memorySpaceMap) {
      op->emitError("missing storage class map for unknown client API");
      return signalPassFailure();
    }

    auto target = spirv::getMemorySpaceToStorageClassTarget(*context);
    spirv::MemorySpaceToStorageClassConverter converter(memorySpaceMap);

    RewritePatternSet patterns(context);
    spirv::populateMemorySpaceToStorageClassPatterns(converter, patterns);

    if (failed(applyFullConversion(op, *target, std::move(patterns))))
      return signalPassFailure();
  }
};

}  // namespace

std::unique_ptr<OperationPass<func::FuncOp>>
createSPIRVMapMemRefStorageClassPass() {
  return std::make_unique<SPIRVMapMemRefStorageClassPass>();
}

}  // namespace iree_compiler
}  // namespace mlir
