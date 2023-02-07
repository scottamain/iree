// Copyright 2020 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/Passes.h"

#include "iree-dialects/Dialect/LinalgExt/Passes/Passes.h"
#include "iree-dialects/Dialect/LinalgTransform/Passes.h"
#include "iree/compiler/Codegen/Interfaces/PartitionableLoopsInterface.h"
#include "iree/compiler/Codegen/LLVMCPU/KernelDispatch.h"
#include "iree/compiler/Codegen/PassDetail.h"
#include "iree/compiler/Codegen/Sandbox/Passes.h"
#include "iree/compiler/Codegen/Transforms/Transforms.h"
#include "iree/compiler/Codegen/Utils/Utils.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Dialect/Arith/Transforms/Passes.h"
#include "mlir/Dialect/Func/Transforms/Passes.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"

#define DEBUG_TYPE "iree-llvm-cpu-lowering-pass-pipeline"

namespace mlir {
namespace iree_compiler {

/// Command line options used purely for development purposes. Not to be relied
/// on in any way.
static llvm::cl::opt<bool> clCheckIRBeforeLLVMConversion(
    "iree-codegen-check-ir-before-llvm-conversion",
    llvm::cl::desc("Runs the pass to check the IR generated from LLVMCPU "
                   "before conversion to LLVM IR"),
    llvm::cl::init(true));

static llvm::cl::opt<bool> clCheckLinalgVectorization(
    "iree-llvmcpu-check-linalg-vectorization",
    llvm::cl::desc(
        "Runs the pass to check if all the Linalg ops are vectorized"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> clEnableHoistPadding(
    "iree-llvmcpu-enable-hoist-padding",
    llvm::cl::desc("Flag to enable hoist padding"), llvm::cl::init(false));

// TODO(#10820): Delete the flag. This should be a nop pass to default pipeline
// while tensor.pad op is lowered to fill + insert_slice before Codegen.
// However, it causes regressions in terms of compilation time. Skip the passes
// for now.
static llvm::cl::opt<bool> clEnablePadConsumerFusion(
    "iree-llvmcpu-enable-pad-consumer-fusion",
    llvm::cl::desc("Flag to enable the fusion for pad + consumer"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> clEnableMicrokernelsDecomposeLinalgGeneric(
    "iree-vmvx-enable-microkernels-decompose-linalg-generic",
    llvm::cl::desc("Enables decomposition of linalg.generic ops when "
                   "microkernels are enabled (experimental)"),
    llvm::cl::init(true));

static llvm::cl::opt<bool> clEnableReassociateFpReductions(
    "iree-llvmcpu-reassociate-fp-reductions",
    llvm::cl::desc("Enables reassociation for FP reductions"),
    llvm::cl::init(false));

// MLIR file containing a top-level module that specifies the transformations to
// apply to form dispatch regions.
// Defined externally in KernelDispatch.cpp to control the codegen pass
// pipeline.
extern llvm::cl::opt<std::string> clCPUCodegenTransformDialectFileName;
extern llvm::cl::opt<std::string> clCPUCodegenTransformDialectDebugPayloadTag;
extern llvm::cl::opt<std::string> clCPUCodegenTransformDialectDebugTransformTag;

//===---------------------------------------------------------------------===//
// Default Linalg code generation options for CPU backend
//===---------------------------------------------------------------------===//

struct LinalgCPUVectorLoweringPassOptions : LinalgVectorLoweringPassOptions {
  LinalgCPUVectorLoweringPassOptions() : LinalgVectorLoweringPassOptions() {
    lowerVectorTransposeTo = "shuffle";
    lowerVectorMultiReductionTo = "innerreduction";
  }
};

//===---------------------------------------------------------------------===//
// Default allocation functions for CPU backend
//===---------------------------------------------------------------------===//

// Allocation callbacks to use with upstream comprehensive bufferization
static FailureOr<Value> cpuAllocationFn(OpBuilder &builder, Location loc,
                                        MemRefType memRefType,
                                        ValueRange dynamicSizes,
                                        unsigned alignment) {
  auto funcOp = builder.getInsertionPoint()->getParentOfType<func::FuncOp>();
  if (funcOp) {
    std::optional<Value> hoistedAllocation = hoistStaticallyBoundAllocations(
        funcOp, builder, loc, memRefType, dynamicSizes, alignment);
    if (hoistedAllocation) {
      return hoistedAllocation.value();
    }
  }
  return builder
      .create<memref::AllocaOp>(loc, memRefType, dynamicSizes,
                                builder.getI64IntegerAttr(alignment))
      .getResult();
}

static LogicalResult cpuDeallocationFn(OpBuilder &builder, Location loc,
                                       Value allocation) {
  return success();
}

static LogicalResult cpuCopyFn(OpBuilder &builder, Location loc, Value from,
                               Value to) {
  createLinalgCopyOp(builder, loc, from, to);
  return success();
}

static void addBufferizePasses(OpPassManager &passManager) {
  BufferizationOptions::AllocationFn allocationFn = cpuAllocationFn;
  BufferizationOptions::DeallocationFn deallocationFn = cpuDeallocationFn;
  BufferizationOptions::MemCpyFn memcpyFn = cpuCopyFn;
  addIREEComprehensiveBufferizePasses(passManager, allocationFn, deallocationFn,
                                      memcpyFn);
  // TODO: Remove the following pass the plumb support for #hal.descriptor_type
  // memory space through the stack.
  passManager.addNestedPass<func::FuncOp>(
      createEraseHALDescriptorTypeFromMemRefPass());
}

static void addTileAndDistributePasses(
    OpPassManager &pm, bool useFuseTensorPadWithConsumerPass = true) {
  pm.addPass(createTileAndDistributeToWorkgroupsPass());
  auto &nestedModulePM = pm.nest<ModuleOp>();
  nestedModulePM.addNestedPass<func::FuncOp>(
      IREE::LinalgExt::createTileAndDecomposeAttentionPass());
  nestedModulePM.addNestedPass<func::FuncOp>(
      IREE::LinalgExt::createDecomposeSoftmaxPass());
  if (clEnablePadConsumerFusion && useFuseTensorPadWithConsumerPass) {
    nestedModulePM.addNestedPass<func::FuncOp>(
        createFuseTensorPadWithConsumerPass());
  }
  nestedModulePM.addNestedPass<func::FuncOp>(
      createConvertToDestinationPassingStylePass());
  nestedModulePM.addNestedPass<func::FuncOp>(
      createFoldAffineMinInDistributedLoopsPass());
  nestedModulePM.addPass(createCanonicalizerPass());
  nestedModulePM.addPass(createCSEPass());
  nestedModulePM.addNestedPass<func::FuncOp>(
      IREE::LinalgExt::createTileAndDecomposeWinogradTransformPass());
}

//===---------------------------------------------------------------------===//
// Codegen configuration verifications.
//===---------------------------------------------------------------------===//

static bool isValidInterchange(ArrayRef<int64_t> interchange, int numLoops) {
  if (interchange.empty()) return true;
  llvm::SmallDenseSet<int64_t> s;
  s.insert(interchange.begin(), interchange.end());
  for (int i = 0; i < numLoops; ++i) {
    if (!s.contains(i)) return false;
  }
  return true;
}

LogicalResult verifyDoubleTilingExpertPassPipelineConfig(
    Operation *op, IREE::Codegen::LoweringConfigAttr loweringConfig,
    IREE::Codegen::TranslationInfoAttr translationInfo,
    ArrayRef<int64_t> workgroupSize) {
  if (!workgroupSize.empty()) {
    return op->emitOpError(
        "expected workgroup size to be empty for CPU pipelines");
  }

  // Verify that the translation info is using the right pipeline.
  if (translationInfo.getDispatchLoweringPassPipeline() !=
          IREE::Codegen::DispatchLoweringPassPipeline::CPUDoubleTilingExpert &&
      translationInfo.getDispatchLoweringPassPipeline() !=
          IREE::Codegen::DispatchLoweringPassPipeline::
              CPUDoubleTilingPadExpert) {
    return op->emitOpError("expected pipeline in translation_info to be ")
           << stringifyEnum(IREE::Codegen::DispatchLoweringPassPipeline::
                                CPUDoubleTilingExpert)
           << " or "
           << stringifyEnum(IREE::Codegen::DispatchLoweringPassPipeline::
                                CPUDoubleTilingPadExpert);
  }

  if (loweringConfig.getTileSizes().size() !=
      static_cast<unsigned>(StrategyTilingLevel::NumStrategyTileLevels)) {
    return op->emitOpError("expected three tiling sizes, got ")
           << loweringConfig.getTileSizes().size();
  }

  auto interfaceOp = dyn_cast_or_null<TilingInterface>(op);
  if (interfaceOp) {
    llvm::SmallDenseSet<unsigned> pLoopsSet;
    for (auto [index, iteratorType] :
         llvm::enumerate(interfaceOp.getLoopIteratorTypes())) {
      if (iteratorType == utils::IteratorType::parallel) {
        pLoopsSet.insert(index);
      }
    }

    SmallVector<int64_t> secondLevelTileSizes = loweringConfig.getTileSizeVals(
        static_cast<unsigned>(StrategyTilingLevel::ParallelTiles));
    for (auto [index, tileSize] : llvm::enumerate(secondLevelTileSizes)) {
      if (tileSize != 0 && !pLoopsSet.contains(index)) {
        return op->emitOpError(
                   "expected only parallel dims to be set in the "
                   "second tiling sizes, got ")
               << index << "-th tile size set";
      }
    }

    SmallVector<int64_t> thirdLevelTileSizes = loweringConfig.getTileSizeVals(
        static_cast<unsigned>(StrategyTilingLevel::ReductionTiles));
    for (auto [index, tileSize] : llvm::enumerate(thirdLevelTileSizes)) {
      if (tileSize != 0 && pLoopsSet.contains(index)) {
        return op->emitOpError(
                   "expected only reduction dims to be set in the third "
                   "tiling sizes, got ")
               << index << "-th tile size set";
      }
    }
  }

  // Verify interchange
  if (!loweringConfig.getTileInterchange().empty()) {
    for (auto level : llvm::seq<unsigned>(
             0, static_cast<unsigned>(
                    loweringConfig.getTileInterchange().size()))) {
      auto tileSizes = loweringConfig.getTileSizeVals(level);
      auto interchange = loweringConfig.getTileInterchangeVals(level);
      if (!isValidInterchange(interchange, tileSizes.size())) {
        return op->emitOpError("expected [0, ")
               << tileSizes.size()
               << ") to be set exactly once in interchange #" << level;
      }
    }
  }

  // Verify that native vector size is empty.
  SmallVector<int64_t> nativeVectorSize =
      loweringConfig.getNativeVectorSizeVals();
  if (!nativeVectorSize.empty()) {
    return op->emitOpError("native_vector_size must be empty");
  }
  return success();
}

LogicalResult verifyConvTileAndDecomposeExpertConfig(
    Operation *op, IREE::Codegen::LoweringConfigAttr loweringConfig,
    IREE::Codegen::TranslationInfoAttr translationInfo,
    ArrayRef<int64_t> workgroupSize) {
  if (loweringConfig.getTileSizes().size() !=
      static_cast<unsigned>(StrategyTilingLevel::NumStrategyTileLevels)) {
    return op->emitOpError("expected three tiling sizes, got ")
           << loweringConfig.getTileSizes().size();
  }

  linalg::LinalgOp linalgOp = cast<linalg::LinalgOp>(op);
  SmallVector<int64_t> shape = linalgOp.getStaticLoopRanges();
  for (auto sizes : loweringConfig.getTileSizeVals()) {
    for (auto [i, size] : llvm::enumerate(sizes)) {
      if (size == 1) shape[i] = 1;
      if (shape[i] == -1 || size == 0) continue;
      if (shape[i] % size != 0) {
        shape[i] = -1;
      } else {
        shape[i] = size;
      }
    }
  }

  int64_t khSize, kwSize, ohSize, owSize;
  auto isSizeExtracted =
      TypeSwitch<Operation *, LogicalResult>(op)
          .Case<linalg::Conv2DNhwcHwcfOp, linalg::DepthwiseConv2DNhwcHwcOp,
                linalg::PoolingNhwcSumOp, linalg::PoolingNhwcMaxOp,
                linalg::PoolingNhwcMaxUnsignedOp, linalg::PoolingNhwcMinOp,
                linalg::PoolingNhwcMinUnsignedOp, linalg::PoolingNchwSumOp,
                linalg::PoolingNchwMaxOp>([&](auto) {
            // Shape: N, OH, OW, OC, KH, KW, (IC)
            khSize = shape[4];
            kwSize = shape[5];
            ohSize = shape[1];
            owSize = shape[2];
            return success();
          })
          .Case<linalg::Conv2DNchwFchwOp>([&](auto) {
            // Shape: N, OC, OH, OW, (IC), KH, KW
            khSize = shape[5];
            kwSize = shape[6];
            ohSize = shape[2];
            owSize = shape[3];
            return success();
          })
          .Case<linalg::PoolingNchwSumOp, linalg::PoolingNchwMaxOp>([&](auto) {
            // Shape: N, OC, OH, OW, KH, KW
            khSize = shape[4];
            kwSize = shape[5];
            ohSize = shape[2];
            owSize = shape[3];
            return success();
          })
          .Default([&](auto) { return failure(); });
  if (failed(isSizeExtracted)) {
    return op->emitOpError("unsupported conv types");
  }

  bool removeH = (khSize == 1 && ohSize == 1);
  bool removeW = (kwSize == 1 && owSize == 1);
  if (!removeH && !removeW) {
    return op->emitOpError("can't decompose the conv op");
  }

  return success();
}

//===---------------------------------------------------------------------===//
// Codegen pipelines.
//===---------------------------------------------------------------------===//

void addCPUBufferOpsTileAndVectorizePipeline(OpPassManager &passManager) {
  addTileAndDistributePasses(passManager);

  OpPassManager &nestedModulePM = passManager.nest<ModuleOp>();
  {
    // Skip tiling reduction loops because this is expected to apply on copy ops
    // only.
    LinalgSingleTilingExpertPassOptions options;
    options.tilingLevel =
        static_cast<int64_t>(StrategyTilingLevel::ParallelTiles);
    options.peel = true;
    options.vectorize = true;
    nestedModulePM.addNestedPass<func::FuncOp>(
        createLinalgSingleTilingExpertPass(options));
    nestedModulePM.addNestedPass<func::FuncOp>(createCanonicalizerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(createCSEPass());
  }

  // Run IREE specific passes before vector lowering expert.
  nestedModulePM.addNestedPass<func::FuncOp>(
      createRemoveSingleIterationLoopPass());

  // Add the vector lowering expert.
  {
    OpPassManager &nestedFuncPassManager = nestedModulePM.nest<func::FuncOp>();
    LinalgCPUVectorLoweringPassOptions options;
    options.splitVectorTransfersTo = "linalg-copy";
    addLowerToVectorTransforms(nestedFuncPassManager, options);
  }
}

void addDoubleTilingPadExpertPassPipeline(OpPassManager &passManager) {
  addTileAndDistributePasses(passManager,
                             /*useFuseTensorPadWithConsumerPass=*/false);

  OpPassManager &nestedModulePM = passManager.nest<ModuleOp>();
  {
    LinalgFusePassOptions options;
    options.tilingLevel =
        static_cast<int64_t>(StrategyTilingLevel::ParallelTiles);
    nestedModulePM.addNestedPass<func::FuncOp>(createLinalgFusePass(options));
    nestedModulePM.addNestedPass<func::FuncOp>(createCanonicalizerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(createCSEPass());
  }

  auto pad = [&](std::string anchorOpName, bool setAnchorOpToRootOp = false,
                 ArrayRef<int64_t> packPaddings = {}) {
    LinalgFusePassOptions options;
    options.padParallelDims = true;
    if (setAnchorOpToRootOp) {
      options.setAnchorOpToRootOp = true;
    } else {
      options.anchorOpName = anchorOpName;
    }
    options.packPaddings.assign(packPaddings.begin(), packPaddings.end());
    nestedModulePM.addNestedPass<func::FuncOp>(createLinalgFusePass(options));
  };

  pad("linalg.fill");
  pad("", /*setAnchorOpToRootOp=*/true);
  // TODO(hanchung): pack and hoist padding for linalg.generic op.
  pad("linalg.generic");

  {
    LinalgSingleTilingExpertPassOptions options;
    options.tilingLevel =
        static_cast<int64_t>(StrategyTilingLevel::ReductionTiles);
    nestedModulePM.addNestedPass<func::FuncOp>(
        createLinalgSingleTilingExpertPass(options));
    nestedModulePM.addNestedPass<func::FuncOp>(createCanonicalizerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(createCSEPass());
  }

  if (!clEnableHoistPadding) {
    LinalgFusePassOptions options;
    options.padReductionDims = true;
    options.setAnchorOpToRootOp = true;
    nestedModulePM.addNestedPass<func::FuncOp>(createLinalgFusePass(options));
  } else {
    {
      LinalgFusePassOptions options;
      options.padReductionDims = true;
      options.setAnchorOpToRootOp = true;
      options.packPaddings = {1, 1, 0};
      nestedModulePM.addNestedPass<func::FuncOp>(createLinalgFusePass(options));
    }

    LinalgFusePassOptions options;
    options.pad = true;
    options.setAnchorOpToRootOp = true;
    options.hoistPaddings = SmallVector<int64_t>{2, 3, 0};
    nestedModulePM.addNestedPass<func::FuncOp>(createLinalgFusePass(options));
    nestedModulePM.addNestedPass<func::FuncOp>(createCanonicalizerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(createCSEPass());
  }

  // Fold dim(pad) away before vectorization.
  nestedModulePM.addPass(memref::createResolveShapedTypeResultDimsPass());

  {
    LinalgSingleTilingExpertPassOptions options;
    options.vectorize = true;
    options.vectorizePadding = true;
    nestedModulePM.addNestedPass<func::FuncOp>(
        createLinalgSingleTilingExpertPass(options));
    nestedModulePM.addNestedPass<func::FuncOp>(createCanonicalizerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(createCSEPass());
  }

  addBufferizePasses(nestedModulePM);

  // Run IREE specific passes before vector lowering expert.
  nestedModulePM.addNestedPass<func::FuncOp>(
      createRemoveSingleIterationLoopPass());

  // Add the vector lowering expert.
  {
    OpPassManager &nestedFuncPassManager = nestedModulePM.nest<func::FuncOp>();
    LinalgCPUVectorLoweringPassOptions options;
    options.splitVectorTransfersTo = "linalg-copy";
    addLowerToVectorTransforms(nestedFuncPassManager, options);
  }
}

void addVMVXDefaultPassPipeline(OpPassManager &passManager,
                                bool enableMicrokernels) {
  addTileAndDistributePasses(passManager,
                             /*useFuseTensorPadWithConsumerPass=*/false);

  // Tensor-level micro-kernel optimizations.
  // Note that this must be done post-tiling because it changes the structure
  // of the dispatch region such that tiling is not always possible.
  if (enableMicrokernels && clEnableMicrokernelsDecomposeLinalgGeneric) {
    passManager.nest<ModuleOp>().nest<func::FuncOp>().addPass(
        createDecomposeLinalgGenericPass());
  }

  // Lower to buffers.
  OpPassManager &nestedModulePM = passManager.nest<ModuleOp>();
  addBufferizePasses(nestedModulePM);

  // Cleanup the IR that may now have unused loops.
  nestedModulePM.addNestedPass<func::FuncOp>(
      createRemoveSingleIterationLoopPass());

  // Convert buffer-level microkernels.
  if (enableMicrokernels) {
    nestedModulePM.addNestedPass<func::FuncOp>(
        createVMVXLowerLinalgMicrokernelsPass());
  }
}

void addMultiTilingExpertPassPipeline(OpPassManager &passManager,
                                      int64_t numLevels, bool enablePeeling,
                                      bool lowerToAVX2) {
  addTileAndDistributePasses(passManager);

  OpPassManager &nestedModulePM = passManager.nest<ModuleOp>();

  // This is a temporary solution for handling aggressive fusion heuristics.
  // This rematerializes parallel ops into the consumers to avoid stack
  // allocation.
  nestedModulePM.addNestedPass<func::FuncOp>(
      createRematerializeParallelOpsPass());

  {
    LinalgFusePassOptions options;
    // Run SplitReductionPass before the final reduction Fuse pass, because
    // SplitReductionPass takes care of banked-tiling.
    for (int64_t i = 1; i < numLevels - 1; ++i) {
      options.tilingLevel = i;
      nestedModulePM.addNestedPass<func::FuncOp>(createLinalgFusePass(options));
    }
  }
  nestedModulePM.addNestedPass<func::FuncOp>(
      createLinalgSplitReductionPass(clEnableReassociateFpReductions));
  {
    LinalgFusePassOptions options;
    options.tilingLevel = numLevels - 1;
    nestedModulePM.addNestedPass<func::FuncOp>(createLinalgFusePass(options));
  }

  if (clEnablePadConsumerFusion) {
    nestedModulePM.addNestedPass<func::FuncOp>(
        createFuseTensorPadWithConsumerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(
        createConcretizePadResultShapePass());
    nestedModulePM.addNestedPass<func::FuncOp>(createVectorizePadPass());
  }

  {
    LinalgSingleTilingExpertPassOptions options;
    options.peel = enablePeeling;
    options.vectorize = true;
    nestedModulePM.addNestedPass<func::FuncOp>(
        createLinalgSingleTilingExpertPass(options));
    nestedModulePM.addNestedPass<func::FuncOp>(createCanonicalizerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(createCSEPass());
  }

  addBufferizePasses(nestedModulePM);

  // Run IREE specific passes before vector lowering expert.
  nestedModulePM.addNestedPass<func::FuncOp>(
      createRemoveSingleIterationLoopPass());

  // Add the vector lowering expert.
  {
    OpPassManager &nestedFuncPassManager = nestedModulePM.nest<func::FuncOp>();
    LinalgCPUVectorLoweringPassOptions options;
    options.lowerVectorTransposeToAVX2 = lowerToAVX2;
    options.splitVectorTransfersTo = "linalg-copy";
    addLowerToVectorTransforms(nestedFuncPassManager, options);
  }
}

void addConvTileAndDecomposeExpertPassPipeline(OpPassManager &passManager) {
  addTileAndDistributePasses(passManager);

  OpPassManager &nestedModulePM = passManager.nest<ModuleOp>();
  // Run LinalgFusePass firstly in case that we have fill + conv + generic
  // ops. At this stage, we do not apply vectorization. The reduction dim won't
  // get tiled if the case is conv + generic op. In this case, we have to tile
  // along reduction dim again, which needs them to be Linalg ops form.
  {
    LinalgFusePassOptions options;
    options.tilingLevel =
        static_cast<int64_t>(StrategyTilingLevel::ParallelTiles);
    nestedModulePM.addNestedPass<func::FuncOp>(createLinalgFusePass(options));
    nestedModulePM.addNestedPass<func::FuncOp>(createCanonicalizerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(createCSEPass());
  }

  // Add the sandbox single tiling expert to tile.
  {
    LinalgSingleTilingExpertPassOptions options;
    options.decomposeToLowerDimOp = true;
    options.tilingLevel =
        static_cast<int64_t>(StrategyTilingLevel::ReductionTiles);
    nestedModulePM.addNestedPass<func::FuncOp>(
        createLinalgSingleTilingExpertPass(options));
    nestedModulePM.addNestedPass<func::FuncOp>(createCanonicalizerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(createCSEPass());
  }

  if (clEnablePadConsumerFusion) {
    nestedModulePM.addNestedPass<func::FuncOp>(
        createFuseTensorPadWithConsumerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(
        createConcretizePadResultShapePass());
    nestedModulePM.addNestedPass<func::FuncOp>(createVectorizePadPass());
  }

  // Add the sandbox single tiling expert to vectorize.
  // We can't do the vectorization in the tiling expert above due to an issue in
  // codegen strategy pipeline. Since we are moving to the transform dialect, we
  // choose to have a workaround here by splitting them into two stages.
  {
    LinalgSingleTilingExpertPassOptions options;
    options.vectorize = true;
    options.vectorizePadding = true;
    nestedModulePM.addNestedPass<func::FuncOp>(
        createLinalgSingleTilingExpertPass(options));
    nestedModulePM.addNestedPass<func::FuncOp>(createCanonicalizerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(createCSEPass());
  }

  nestedModulePM.addNestedPass<func::FuncOp>(createCSEPass());
  nestedModulePM.addNestedPass<func::FuncOp>(createCanonicalizerPass());
  nestedModulePM.addNestedPass<func::FuncOp>(
      createOptimizeVectorTransferPass(/*flatten=*/false));
  addBufferizePasses(nestedModulePM);

  // Run IREE specific passes before vector lowering expert.
  nestedModulePM.addNestedPass<func::FuncOp>(
      createRemoveSingleIterationLoopPass());

  // Add the vector lowering expert.
  {
    OpPassManager &nestedFuncPassManager = nestedModulePM.nest<func::FuncOp>();
    LinalgCPUVectorLoweringPassOptions options;
    options.splitVectorTransfersTo = "shuffle";
    addLowerToVectorTransforms(nestedFuncPassManager, options);
  }
}

void addMmt4dTilingExpertPassPipeline(OpPassManager &passManager) {
  addTileAndDistributePasses(passManager);

  OpPassManager &nestedModulePM = passManager.nest<ModuleOp>();
  {
    LinalgFusePassOptions options;
    options.tilingLevel =
        static_cast<int64_t>(StrategyTilingLevel::ParallelTiles);
    nestedModulePM.addNestedPass<func::FuncOp>(createLinalgFusePass(options));
  }

  // Run SplitReductionPass before the final reduction Fuse pass, because
  // SplitReductionPass takes care of banked-tiling.
  nestedModulePM.addNestedPass<func::FuncOp>(
      createLinalgSplitReductionPass(clEnableReassociateFpReductions));
  {
    LinalgSingleTilingExpertPassOptions options;
    options.tilingLevel =
        static_cast<int64_t>(StrategyTilingLevel::ReductionTiles);
    nestedModulePM.addNestedPass<func::FuncOp>(
        createLinalgSingleTilingExpertPass(options));
  }

  if (clEnablePadConsumerFusion) {
    nestedModulePM.addNestedPass<func::FuncOp>(
        createFuseTensorPadWithConsumerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(
        createConcretizePadResultShapePass());
    nestedModulePM.addNestedPass<func::FuncOp>(createVectorizePadPass());
  }

  {
    LinalgSingleTilingExpertPassOptions options;
    options.vectorize = true;
    nestedModulePM.addNestedPass<func::FuncOp>(
        createLinalgSingleTilingExpertPass(options));
  }

  addBufferizePasses(nestedModulePM);

  nestedModulePM.addNestedPass<func::FuncOp>(
      createLLVMCPUMmt4dVectorLoweringPass());
  nestedModulePM.addNestedPass<func::FuncOp>(
      createOptimizeVectorTransferPass(/*flatten=*/true));
}

void addCPUDataTilingPipeline(OpPassManager &passManager) {
  addTileAndDistributePasses(passManager);
  OpPassManager &nestedModulePM = passManager.nest<ModuleOp>();
  nestedModulePM.addNestedPass<func::FuncOp>(
      IREE::LinalgExt::createLinalgExtVectorizationPass());
  addBufferizePasses(nestedModulePM);
  nestedModulePM.addNestedPass<func::FuncOp>(
      createSplitFullPartialTransferPass("linalg-copy"));
}

void addCPUDefaultPassPipeline(OpPassManager &passManager) {
  addTileAndDistributePasses(passManager);
  OpPassManager &nestedModulePM = passManager.nest<ModuleOp>();
  addBufferizePasses(nestedModulePM);
  if (clEnablePadConsumerFusion) {
    nestedModulePM.addNestedPass<func::FuncOp>(
        createFuseTensorPadWithConsumerPass());
    nestedModulePM.addNestedPass<func::FuncOp>(
        createConcretizePadResultShapePass());
    nestedModulePM.addNestedPass<func::FuncOp>(createVectorizePadPass());
  }
}

void addTransformDialectPasses(OpPassManager &passManager) {
  // Give control to the transform dialect.
  passManager.addPass(
      mlir::iree_compiler::createTransformDialectInterpreterPass(
          clCPUCodegenTransformDialectFileName,
          clCPUCodegenTransformDialectDebugPayloadTag,
          clCPUCodegenTransformDialectDebugTransformTag));
  // Dropping the schedule is needed:
  //   1. if we want to embed the transform in the module: we should drop the
  //      schedule once applied.
  //   2. if transform.do_not_dce_operands ops are introduced.
  passManager.addPass(createDropSchedulePass());
}

static void addLowerToLLVMPasses(OpPassManager &passManager) {
  // LinalgExt -> SCF
  passManager.addNestedPass<func::FuncOp>(
      IREE::LinalgExt::createLinalgExtToLoopsPass());

  // Linalg -> SCF
  passManager.addNestedPass<func::FuncOp>(createMemrefCopyToLinalgPass());
  if (clCheckLinalgVectorization) {
    passManager.addNestedPass<func::FuncOp>(
        createLLVMCPUEmitVectorizationRemarksPass());
  }
  passManager.addNestedPass<func::FuncOp>(createConvertLinalgToLoopsPass());
  passManager.addNestedPass<func::FuncOp>(createCanonicalizerPass());
  passManager.addNestedPass<func::FuncOp>(createCSEPass());

  // Handled tensor-type constants.
  passManager.addPass(arith::createConstantBufferizePass());
  passManager.addPass(createFoldTensorExtractOpPass());

  // math dialect elementry functions -> polynomial form.
  passManager.addNestedPass<func::FuncOp>(createPolynomialApproximationPass());

  passManager.addNestedPass<func::FuncOp>(
      createHoistStaticallyBoundAllocationsPass());

  // Checking stack allocation before converting to CF dialect is easier.
  // Do not check allocation if hoist-padding is enabled. It intends to allocate
  // big stack buffers for better accessing.
  if (clCheckIRBeforeLLVMConversion && !clEnableHoistPadding) {
    passManager.addPass(createLLVMCPUCheckIRBeforeLLVMConversionPass());
  }

  // SCF -> CF
  passManager.addNestedPass<func::FuncOp>(createConvertSCFToCFPass());
  passManager.addNestedPass<func::FuncOp>(createCanonicalizerPass());
  passManager.addNestedPass<func::FuncOp>(createCSEPass());

  // (HAL, IREE, Linalg, CF) -> LLVM
  passManager.addNestedPass<func::FuncOp>(arith::createArithExpandOpsPass());
  passManager.addNestedPass<func::FuncOp>(memref::createExpandOpsPass());
  passManager.addPass(createConvertToLLVMPass(clEnableReassociateFpReductions));
  passManager.addPass(createReconcileUnrealizedCastsPass());

  // We rely on MLIR symbol visibility being correct after this point and need
  // to mirror the LLVM linkage that was assigned during conversion.
  passManager.addPass(createLLVMCPUSynchronizeSymbolVisibilityPass());

  passManager.addPass(createCanonicalizerPass());
  passManager.addPass(createCSEPass());
}

void buildLLVMCPUCodegenPassPipeline(OpPassManager &passManager) {
  passManager.addNestedPass<ModuleOp>(
      createVerifyLinalgTransformLegalityPass());
  passManager.nest<ModuleOp>().addNestedPass<func::FuncOp>(
      createTypePropagationPass());
  passManager.nest<ModuleOp>().addNestedPass<func::FuncOp>(
      createLLVMCPUMaterializeEncodingPass());
  passManager.addNestedPass<ModuleOp>(createBufferizeCopyOnlyDispatchesPass());
  // TODO: Remove the following pass the plumb support for #hal.descriptor_type
  // memory space through the stack.
  passManager.nest<ModuleOp>().addNestedPass<func::FuncOp>(
      createEraseHALDescriptorTypeFromMemRefPass());

  passManager.addPass(createLLVMCPULowerExecutableTargetPass());
  OpPassManager &nestedModulePM = passManager.nest<ModuleOp>();
  addLowerToLLVMPasses(nestedModulePM);

  LLVM_DEBUG({
    llvm::dbgs() << "Using LLVMCPU pass pipeline:\n";
    passManager.printAsTextualPipeline(llvm::dbgs());
    llvm::dbgs() << "\n";
  });
}

// NOTE: this runs on the top-level program module containing all
// hal.executable ops.
void buildLLVMCPULinkingPassPipeline(OpPassManager &passManager) {
  // Link together executables. This may produce some IR duplication.
  passManager.addPass(createLLVMCPULinkExecutablesPass());

  // Cleanup IR duplication.
  passManager.addNestedPass<IREE::HAL::ExecutableOp>(
      mlir::createCanonicalizerPass());

  // Assign final executable constant and import ordinals.
  auto &variantPM = passManager.nest<IREE::HAL::ExecutableOp>()
                        .nest<IREE::HAL::ExecutableVariantOp>();
  variantPM.addPass(createLLVMCPUAssignConstantOrdinalsPass());
  variantPM.addPass(createLLVMCPUAssignImportOrdinalsPass());
}

}  // namespace iree_compiler
}  // namespace mlir
