// Copyright 2020 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/LLVMCPU/DispatchABI.h"
#include "iree/compiler/Codegen/PassDetail.h"
#include "iree/compiler/Codegen/Passes.h"
#include "iree/compiler/Codegen/Utils/Utils.h"
#include "iree/compiler/Dialect/HAL/IR/HALDialect.h"
#include "iree/compiler/Dialect/HAL/IR/HALOps.h"
#include "iree/compiler/Dialect/Util/IR/UtilDialect.h"
#include "iree/compiler/Dialect/Util/IR/UtilOps.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Analysis/DataLayoutAnalysis.h"
#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ArmNeon2dToIntr/ArmNeon2dToIntr.h"
#include "mlir/Conversion/ComplexToLLVM/ComplexToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/LLVMCommon/ConversionTarget.h"
#include "mlir/Conversion/LLVMCommon/LoweringOptions.h"
#include "mlir/Conversion/LLVMCommon/Pattern.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Conversion/LinalgToLLVM/LinalgToLLVM.h"
#include "mlir/Conversion/MathToLLVM/MathToLLVM.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Conversion/TosaToArith/TosaToArith.h"
#include "mlir/Conversion/VectorToLLVM/ConvertVectorToLLVM.h"
#include "mlir/Conversion/VectorToSCF/VectorToSCF.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ArmNeon/ArmNeonDialect.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Func/Transforms/Passes.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/Math/Transforms/Passes.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Dialect/Tosa/IR/TosaOps.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

namespace mlir {
namespace iree_compiler {

namespace {

template <typename OpT>
struct ConvertOpToLLVMWithABIPattern : public ConvertOpToLLVMPattern<OpT> {
  ConvertOpToLLVMWithABIPattern(HALDispatchABI &abi,
                                LLVMTypeConverter &typeConverter,
                                PatternBenefit benefit = 1)
      : ConvertOpToLLVMPattern<OpT>(typeConverter, benefit), abi(abi) {}
  HALDispatchABI &abi;
};

/// Converts Standard MLIR FuncOps to LLVMFuncOps matching the IREE HAL ABI.
/// This is an IREE-specific conversion that assumes the input function is
/// `() -> ()` and that hal.interface.* ops are used to access all state.
///
/// Source function:
///
/// ```
/// func.func @foo() {
///   %0 = hal.interface.binding.subspan ...
/// }
/// ```
///
/// into:
///
/// ```
/// llvm.func foo(%state: !llvm.ptr<!...>,
///               %workgroup_id : !llvm.ptr<!llvm.array<i32, 3>>) {
///   %0 = <GEP/loads to access binding in %state>
/// }
/// ```
///
/// See `iree/hal/local/executable_library.h` for more information.
///
/// NOTE: we bump the benefit of the pattern to 100 to pick this pattern instead
/// of a competing pattern inserted by `populateFuncToLLVMConversionPatterns`.
struct ConvertHALEntryPointFuncOp
    : public ConvertOpToLLVMWithABIPattern<func::FuncOp> {
  ConvertHALEntryPointFuncOp(HALDispatchABI &abi,
                             LLVMTypeConverter &typeConverter)
      : ConvertOpToLLVMWithABIPattern(abi, typeConverter,
                                      /*benefit=*/100) {}
  LogicalResult matchAndRewrite(
      func::FuncOp stdFuncOp, func::FuncOpAdaptor operands,
      ConversionPatternRewriter &rewriter) const override {
    if (!stdFuncOp.isPublic()) return failure();
    FunctionType fnType = stdFuncOp.getFunctionType();
    if (fnType.getNumInputs() != 0 || fnType.getNumResults() != 0) {
      stdFuncOp->emitWarning()
          << "public functions on executables must be () -> ()";
      return failure();
    }

    // Convert the function signature to take the HAL ABI LLVM pointers.
    TypeConverter::SignatureConversion signatureConverter(/*numOrigInputs=*/0);
    MLIRContext *context = rewriter.getContext();
    auto abiInputTypes =
        HALDispatchABI::getInputTypes(context, getTypeConverter());
    signatureConverter.addInputs(abiInputTypes);

    // Copy all attributes onto the LLVM function except the ones handled by
    // MLIR implicitly.
    SmallVector<NamedAttribute, 4> funcAttrs;
    for (auto attr : stdFuncOp->getAttrs()) {
      if (attr.getName() == SymbolTable::getSymbolAttrName() ||
          attr.getName() == stdFuncOp.getFunctionTypeAttrName()) {
        continue;
      }
      funcAttrs.push_back(attr);
    }

    // Clone the function as an LLVMFuncOp and convert all interior types.
    auto int32Type = IntegerType::get(rewriter.getContext(), 32);
    auto llvmFuncType = LLVM::LLVMFunctionType::get(int32Type, abiInputTypes);
    auto llvmFuncOp = rewriter.create<LLVM::LLVMFuncOp>(
        stdFuncOp.getLoc(), stdFuncOp.getName(), llvmFuncType,
        LLVM::Linkage::External, /*dso_local=*/false, /*cconv*/ LLVM::CConv::C,
        funcAttrs);
    rewriter.inlineRegionBefore(stdFuncOp.getFunctionBody(),
                                llvmFuncOp.getFunctionBody(), llvmFuncOp.end());
    if (failed(rewriter.convertRegionTypes(&llvmFuncOp.getFunctionBody(),
                                           *typeConverter,
                                           &signatureConverter))) {
      return failure();
    }

    // Tag all arguments so LLVM can reason about our exports it otherwise
    // cannot analyze. We do this early on so that MLIR-based LLVM transforms
    // can use the attributes.
    // (%arg0: environment, %arg1: dispatch_state, %arg2: workgroup_state)
    for (unsigned i = 0; i <= 2; ++i) {
      llvmFuncOp.setArgAttr(i, LLVM::LLVMDialect::getNoAliasAttrName(),
                            rewriter.getUnitAttr());
      llvmFuncOp.setArgAttr(i, LLVM::LLVMDialect::getAlignAttrName(),
                            rewriter.getI64IntegerAttr(16));
    }

    // Add default zero return value.
    // TODO(ataei): do something meaningful with the return value; non-zero will
    // have the runtime bail out with an error.
    for (auto returnOp : llvm::make_early_inc_range(
             llvmFuncOp.getOps<mlir::func::ReturnOp>())) {
      rewriter.setInsertionPoint(returnOp);
      auto returnValue = rewriter.createOrFold<mlir::arith::ConstantIntOp>(
          returnOp.getLoc(), 0, 32);
      rewriter.replaceOpWithNewOp<mlir::func::ReturnOp>(returnOp, returnValue);
    }

    // Populate debug info for the subprogram signature. This is required in
    // order to get any debug information (including just line tables) from MLIR
    // into LLVM IR.
    auto scopeAttr = HALDispatchABI::buildScopeAttr(
        llvmFuncOp->getParentOfType<mlir::ModuleOp>(), llvmFuncOp.getName(),
        getTypeConverter());
    llvmFuncOp->setLoc(FusedLoc::get(llvmFuncOp.getContext(),
                                     {llvmFuncOp->getLoc()}, scopeAttr));

    rewriter.eraseOp(stdFuncOp);
    return success();
  }
};

/// Rewrites hal.interface.constant.load to ops loading from the ABI structs.
/// Because ordinals are not yet available we emit a placeholder global that
/// later gets updated with the value after linking.
///
/// The parent LLVMFuncOp must be compatible with HALDispatchABI.
struct ConvertHALExecutableConstantLoadOp
    : public ConvertOpToLLVMWithABIPattern<
          IREE::HAL::ExecutableConstantLoadOp> {
  using ConvertOpToLLVMWithABIPattern::ConvertOpToLLVMWithABIPattern;
  LogicalResult matchAndRewrite(
      IREE::HAL::ExecutableConstantLoadOp loadOp,
      IREE::HAL::ExecutableConstantLoadOpAdaptor operands,
      ConversionPatternRewriter &rewriter) const override {
    auto resultType =
        typeConverter->convertType(loadOp->getResult(0).getType());
    rewriter.replaceOp(
        loadOp, abi.loadExecutableConstant(loadOp, loadOp.getKey(), resultType,
                                           rewriter));
    return success();
  }
};

/// Rewrites hal.interface.workgroup.id to ops loading from the ABI structs.
///
/// The parent LLVMFuncOp must be compatible with HALDispatchABI.
struct ConvertHALInterfaceWorkgroupIDOp
    : public ConvertOpToLLVMWithABIPattern<IREE::HAL::InterfaceWorkgroupIDOp> {
  using ConvertOpToLLVMWithABIPattern::ConvertOpToLLVMWithABIPattern;
  LogicalResult matchAndRewrite(
      IREE::HAL::InterfaceWorkgroupIDOp idOp,
      IREE::HAL::InterfaceWorkgroupIDOpAdaptor operands,
      ConversionPatternRewriter &rewriter) const override {
    int32_t dim = (int32_t)idOp.getDimension().getZExtValue();
    auto resultType = typeConverter->convertType(idOp->getResult(0).getType());
    rewriter.replaceOp(idOp,
                       abi.loadWorkgroupID(idOp, dim, resultType, rewriter));
    return success();
  }
};

/// Rewrites hal.interface.workgroup.size to ops loading from the ABI structs.
///
/// The parent LLVMFuncOp must be compatible with HALDispatchABI.
struct ConvertHALInterfaceWorkgroupSizeOp
    : public ConvertOpToLLVMWithABIPattern<
          IREE::HAL::InterfaceWorkgroupSizeOp> {
  using ConvertOpToLLVMWithABIPattern::ConvertOpToLLVMWithABIPattern;
  LogicalResult matchAndRewrite(
      IREE::HAL::InterfaceWorkgroupSizeOp sizeOp,
      IREE::HAL::InterfaceWorkgroupSizeOpAdaptor operands,
      ConversionPatternRewriter &rewriter) const override {
    int32_t dim = (int32_t)sizeOp.getDimension().getZExtValue();
    auto resultType =
        typeConverter->convertType(sizeOp->getResult(0).getType());
    rewriter.replaceOp(
        sizeOp, abi.loadWorkgroupSize(sizeOp, dim, resultType, rewriter));
    return success();
  }
};

/// Rewrites hal.interface.workgroup.count to ops loading from the ABI structs.
///
/// The parent LLVMFuncOp must be compatible with HALDispatchABI.
struct ConvertHALInterfaceWorkgroupCountOp
    : public ConvertOpToLLVMWithABIPattern<
          IREE::HAL::InterfaceWorkgroupCountOp> {
  using ConvertOpToLLVMWithABIPattern::ConvertOpToLLVMWithABIPattern;
  LogicalResult matchAndRewrite(
      IREE::HAL::InterfaceWorkgroupCountOp countOp,
      IREE::HAL::InterfaceWorkgroupCountOpAdaptor operands,
      ConversionPatternRewriter &rewriter) const override {
    int32_t dim = (int32_t)countOp.getDimension().getZExtValue();
    auto resultType =
        typeConverter->convertType(countOp->getResult(0).getType());
    rewriter.replaceOp(
        countOp, abi.loadWorkgroupCount(countOp, dim, resultType, rewriter));
    return success();
  }
};

/// Rewrites hal.interface.constant.load to ops loading from the ABI structs.
///
/// The parent LLVMFuncOp must be compatible with HALDispatchABI.
struct ConvertHALInterfaceConstantLoadOp
    : public ConvertOpToLLVMWithABIPattern<IREE::HAL::InterfaceConstantLoadOp> {
  using ConvertOpToLLVMWithABIPattern::ConvertOpToLLVMWithABIPattern;
  LogicalResult matchAndRewrite(
      IREE::HAL::InterfaceConstantLoadOp loadOp,
      IREE::HAL::InterfaceConstantLoadOpAdaptor operands,
      ConversionPatternRewriter &rewriter) const override {
    int64_t index = loadOp.getIndex().getZExtValue();
    auto resultType =
        typeConverter->convertType(loadOp->getResult(0).getType());
    rewriter.replaceOp(
        loadOp, abi.loadPushConstant(loadOp, index, resultType, rewriter));
    return success();
  }
};

/// Rewrites hal.interface.binding.subspan to ops loading from the ABI structs.
///
/// The parent LLVMFuncOp must be compatible with HALDispatchABI.
struct ConvertHALInterfaceBindingSubspanOp
    : public ConvertOpToLLVMWithABIPattern<
          IREE::HAL::InterfaceBindingSubspanOp> {
  using ConvertOpToLLVMWithABIPattern::ConvertOpToLLVMWithABIPattern;
  LogicalResult matchAndRewrite(
      IREE::HAL::InterfaceBindingSubspanOp subspanOp,
      IREE::HAL::InterfaceBindingSubspanOpAdaptor operands,
      ConversionPatternRewriter &rewriter) const override {
    MemRefType memRefType =
        subspanOp->getResult(0).getType().dyn_cast<MemRefType>();
    if (!memRefType) {
      return rewriter.notifyMatchFailure(
          subspanOp,
          "failed to convert interface.binding.subspan result to memref type");
    }
    auto memRefDesc = abi.loadBinding(
        subspanOp, operands.getBindingAttr().getInt(), operands.getByteOffset(),
        memRefType, operands.getDynamicDims(), rewriter);
    rewriter.replaceOp(subspanOp, {memRefDesc});
    return success();
  }
};

/// Rewrites calls to extern functions to dynamic library import calls.
/// The parent LLVMFuncOp must be compatible with HALDispatchABI.
///
/// Note: this is an LLVM::CallOp -> LLVM::CallOp rewrite that is introduced
/// after all conversions are done. Importantly, this is not a conversion
/// pattern.
struct RewriteExternCallOpToDynamicImportCallOp
    : public OpRewritePattern<LLVM::CallOp> {
  RewriteExternCallOpToDynamicImportCallOp(HALDispatchABI &abi,
                                           LLVMTypeConverter &typeConverter)
      : OpRewritePattern(&typeConverter.getContext()),
        abi(abi),
        typeConverter(typeConverter) {}
  LogicalResult matchAndRewrite(LLVM::CallOp callOp,
                                PatternRewriter &rewriter) const override {
    // Ignore indirect calls (they're probably already converted imports).
    auto symbol = callOp.getCallableForCallee().dyn_cast<SymbolRefAttr>();
    auto flatSymbol = symbol.dyn_cast_or_null<FlatSymbolRefAttr>();
    if (!flatSymbol) return failure();

    // Ensure the target function is extern.
    // To support conversion inserting calls in local patterns that can't add
    // global function symbols we assume any missing callee is extern.
    auto calleeOp =
        SymbolTable::lookupNearestSymbolFrom<LLVM::LLVMFuncOp>(callOp, symbol);
    if (calleeOp && !calleeOp.isExternal()) {
      return rewriter.notifyMatchFailure(
          callOp,
          "callee is not external; treating as a normal call and skipping "
          "import logic");
    }

    // If the function is marked as statically linked we don't touch it. That'll
    // let it fall through to the linker stage where it can be picked up either
    // from the runtime build (in the case of us producing static libraries) or
    // the user-specified object files (when producing dynamic libraries).
    if (calleeOp->hasAttr("hal.import.static")) {
      return rewriter.notifyMatchFailure(callOp,
                                         "external function is marked static "
                                         "and does not need an import wrapper");
    }

    // TODO(benvanik): way to determine if weak (maybe via linkage?).
    bool weak = false;

    // Rewrite the call to a dynamic import call.
    SmallVector<Value> results = abi.wrapAndCallImport(
        callOp, flatSymbol.getValue(), weak, callOp->getResultTypes(),
        callOp->getOperands(), rewriter);

    rewriter.replaceOp(callOp, results);
    return success();
  }
  HALDispatchABI &abi;
  LLVMTypeConverter &typeConverter;
};

class ConvertToLLVMPass : public ConvertToLLVMBase<ConvertToLLVMPass> {
 public:
  ConvertToLLVMPass(bool reassociateFpReductions) {
    targetReassociateFpReductions.setValue(reassociateFpReductions);
  }
  ConvertToLLVMPass(const ConvertToLLVMPass &pass) {}
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<LLVM::LLVMDialect, arm_neon::ArmNeonDialect>();
  }

  void runOnOperation() override;

 private:
  Option<std::string> targetTriple{
      *this, "target-triple", llvm::cl::desc("Code generation target triple."),
      llvm::cl::init("")};
  Option<std::string> targetDataLayout{
      *this, "target-data-layout",
      llvm::cl::desc("Code generation target data layout."),
      llvm::cl::init("")};
  Option<bool> targetReassociateFpReductions{
      *this, "target-reassociate-fp-reductions",
      llvm::cl::desc("Code generation target reassociate FP reductions."),
      llvm::cl::init("false")};
};

}  // namespace

static std::string getStringAttrFromTargetAttr(ModuleOp module,
                                               StringRef attrName) {
  auto targetAttr = IREE::HAL::ExecutableTargetAttr::lookup(module);
  auto stringAttr = getConfigStringAttr(targetAttr, attrName);
  return stringAttr ? stringAttr.value().str() : std::string("");
}

void ConvertToLLVMPass::runOnOperation() {
  auto module = getOperation();
  std::string dataLayoutStr = targetDataLayout.getValue();
  if (targetDataLayout.empty()) {
    dataLayoutStr = getStringAttrFromTargetAttr(module, "data_layout");
  }
  std::string targetTripleStr = targetTriple.getValue();
  if (targetTripleStr.empty()) {
    targetTripleStr = getStringAttrFromTargetAttr(module, "target_triple");
  }

  // Add required attributes to the module so that the lowering knows how to
  // handle structs and data layouts.
  module->setAttr(LLVM::LLVMDialect::getTargetTripleAttrName(),
                  StringAttr::get(module->getContext(), targetTripleStr));
  module->setAttr(LLVM::LLVMDialect::getDataLayoutAttrName(),
                  StringAttr::get(module->getContext(), dataLayoutStr));

  // Run Vector -> Vector transformations ahead of conversion to LLVM.
  {
    RewritePatternSet patterns(&getContext());
    vector::populateVectorToVectorCanonicalizationPatterns(patterns);
    vector::populateVectorBroadcastLoweringPatterns(patterns);
    vector::populateVectorContractLoweringPatterns(patterns);
    vector::populateVectorMaskMaterializationPatterns(
        patterns, /*force32BitVectorIndices=*/false);
    vector::populateVectorMaskOpLoweringPatterns(patterns);
    vector::populateVectorShapeCastLoweringPatterns(patterns);
    vector::populateVectorTransposeLoweringPatterns(patterns);
    populateConvertArmNeon2dToIntrPatterns(patterns);
    if (failed(applyPatternsAndFoldGreedily(getOperation(),
                                            std::move(patterns)))) {
      return signalPassFailure();
    }
  }
  {
    RewritePatternSet vectorToLoopsPatterns(&getContext());
    populateVectorToSCFConversionPatterns(
        vectorToLoopsPatterns, VectorTransferToSCFOptions().enableFullUnroll());
    if (failed(applyPatternsAndFoldGreedily(
            getOperation(), std::move(vectorToLoopsPatterns)))) {
      return signalPassFailure();
    }
  }

  const auto &dataLayoutAnalysis = getAnalysis<DataLayoutAnalysis>();
  LowerToLLVMOptions options(&getContext(),
                             dataLayoutAnalysis.getAtOrAbove(module));
  options.dataLayout = llvm::DataLayout(dataLayoutStr);
  options.overrideIndexBitwidth(options.dataLayout.getPointerSizeInBits());
  LLVMTypeConverter typeConverter(&getContext(), options, &dataLayoutAnalysis);

  RewritePatternSet patterns(&getContext());

  // Use the default 64-bit lowering for TOSA's ApplyScale operator:
  //   This lowering widens integer types to 64-bit an performs the non-fused
  //   operations, specifically multiply, add, and shift. Bit-widening
  //   is used to guarantee higher-order bits are not truncated during the
  //   multiply or add.
  //
  // TODO(bjacob): Use a lowering that uses specific ARM/X86 intrinsics.
  bool use32BitImpl = false;
  auto targetAttr = IREE::HAL::ExecutableTargetAttr::lookup(module);
  if (isRISCV(targetAttr)) {
    // Use the 32-bit lowering for RISC-V if 'zve32x' is specified and there is
    // no 64-bit integer vector support.
    // TODO(#9440) Simplify logic when 'cpu_features' is simplified.
    use32BitImpl = hasZve32xFeature(targetAttr) && !hasVFeature(targetAttr) &&
                   !hasZve64xFeature(targetAttr);
  }
  tosa::populateTosaRescaleToArithConversionPatterns(&patterns, use32BitImpl);

  populateAffineToStdConversionPatterns(patterns);
  populateSCFToControlFlowConversionPatterns(patterns);
  cf::populateControlFlowToLLVMConversionPatterns(typeConverter, patterns);
  populateExpandTanhPattern(patterns);

  populateComplexToLLVMConversionPatterns(typeConverter, patterns);
  populateMathToLLVMConversionPatterns(typeConverter, patterns);
  memref::populateExpandStridedMetadataPatterns(patterns);
  populateFinalizeMemRefToLLVMConversionPatterns(typeConverter, patterns);
  populateFuncToLLVMConversionPatterns(typeConverter, patterns);
  arith::populateArithToLLVMConversionPatterns(typeConverter, patterns);
  populateVectorToSCFConversionPatterns(patterns);
  populateVectorToLLVMMatrixConversionPatterns(typeConverter, patterns);
  populateVectorToLLVMConversionPatterns(
      typeConverter, patterns, targetReassociateFpReductions.getValue());
  populateLinalgToLLVMConversionPatterns(typeConverter, patterns);
  populateReconcileUnrealizedCastsPatterns(patterns);

  HALDispatchABI abi(&typeConverter);
  // clang-format off
  patterns.insert<
    ConvertHALEntryPointFuncOp,
    ConvertHALExecutableConstantLoadOp,
    ConvertHALInterfaceWorkgroupIDOp,
    ConvertHALInterfaceWorkgroupSizeOp,
    ConvertHALInterfaceWorkgroupCountOp,
    ConvertHALInterfaceConstantLoadOp,
    ConvertHALInterfaceBindingSubspanOp
  >(abi, typeConverter);
  // clang-format on

  LLVMConversionTarget target(getContext());
  target.addLegalOp<ModuleOp>();
  target.addIllegalDialect<func::FuncDialect, mlir::arith::ArithDialect,
                           IREE::Util::UtilDialect, IREE::HAL::HALDialect,
                           math::MathDialect, tosa::TosaDialect>();
  target.addIllegalOp<UnrealizedConversionCastOp>();

  if (failed(applyPartialConversion(module, target, std::move(patterns)))) {
    signalPassFailure();
    return;
  }

  // Rewrite any extern calls emitted to dynamic library imports.
  {
    RewritePatternSet patterns(&getContext());
    patterns.insert<RewriteExternCallOpToDynamicImportCallOp>(abi,
                                                              typeConverter);
    if (failed(applyPatternsAndFoldGreedily(module, std::move(patterns))))
      return signalPassFailure();
  }

  // Post conversion patterns.
  {
    RewritePatternSet postPatterns(&getContext());
    // TODO(ravishankarm): Move this to a separate pass.
    llvm::Triple triple(targetTripleStr);
    if (triple.isWasm()) {
      populateUnfusedFMAOpsPassPatterns(&getContext(), postPatterns);
      if (failed(
              applyPatternsAndFoldGreedily(module, std::move(postPatterns)))) {
        return signalPassFailure();
      }
    }
  }
}

std::unique_ptr<OperationPass<ModuleOp>> createConvertToLLVMPass(
    bool reassociateFpReductions) {
  return std::make_unique<ConvertToLLVMPass>(reassociateFpReductions);
}

}  // namespace iree_compiler
}  // namespace mlir
