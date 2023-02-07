// Copyright 2019 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <utility>

#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "iree/compiler/Dialect/Flow/Transforms/PassDetail.h"
#include "iree/compiler/Dialect/Flow/Transforms/Passes.h"
#include "iree/compiler/Dialect/Util/IR/UtilOps.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/IR/LinalgInterfaces.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"

#define DEBUG_TYPE "iree-dispatch"

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace Flow {
namespace {

// Estimates the evaluation cost of a linalg op using a heuristic cost model.
static int64_t estimateLinalgOpCost(linalg::LinalgOp op) {
  if (op.hasDynamicShape()) {
    // Note: bounded dynamic shapes would be interesting, if the compiler used
    // them. For now just treat dynamic shapes as arbitrarily large.
    return INT64_MAX;
  }

  int64_t cost = 1;
  for (auto loopRange : op.getStaticLoopRanges()) {
    cost *= loopRange;
  }
  LLVM_DEBUG(llvm::dbgs() << "// " << op->getName() << " cost: " << cost
                          << "\n");
  return cost;
}

static std::string loopRangeToString(int64_t loopRange) {
  // Note: normally we'd use '?', but that isn't a valid character for function
  // names on a variety of targets, so we stick to [a-Z0-9_] characters.
  return ShapedType::isDynamic(loopRange) ? "D" : llvm::itostr(loopRange);
}

// Returns a string like "512xDx128" representing a linalg op's loop ranges.
static std::string getLinalgOpLoopRanges(linalg::LinalgOp op) {
  auto loopRanges = op.getStaticLoopRanges();
  std::string outputString;
  llvm::raw_string_ostream sstream(outputString);
  llvm::interleave(
      loopRanges,
      [&](int64_t loopRange) { sstream << loopRangeToString(loopRange); },
      [&] { sstream << "x"; });
  return outputString;
}

static std::string summarizeLinalgOp(linalg::LinalgOp op) {
  auto opName = op->getName().getStringRef();
  if (!opName.consume_front("linalg.")) return "";
  std::string opSuffix = getLinalgOpLoopRanges(op);
  // TODO(scotttodd): include element type(s) in this string
  return opName.str() + (opSuffix.empty() ? "" : "_" + opSuffix);
}

// Summarizes the contents of a dispatch into a short string.
// This uses heuristics to aid developer debugging.
static std::string summarizeDispatchWorkgroupsOp(
    DispatchWorkgroupsOp regionOp) {
  // The goal here is to build a relatively concise description that gives
  // enough information to developers to see roughly what sort of computation a
  // dispatch region performs. Multiple approaches are valid here, depending on
  // what a developer wants to highlight.
  //
  // Currently, this uses a cost model to estimate which individual operation
  // is the most computationally expensive, then a summary is generated which
  // includes some of that operation's parameters.
  //
  // Other metrics to determine which single op is the "best" or which list of
  // ops is most interesting (e.g. to highlight large data movements) could be
  // used instead.

  Operation *bestOp = NULL;
  int64_t bestEstimatedCost = -1;
  regionOp.getBodyRegion().walk([&](Operation *op) {
    TypeSwitch<Operation *>(op)
        .Case<linalg::LinalgOp>([&](auto op) {
          int64_t estimatedCost = estimateLinalgOpCost(op);
          if (estimatedCost < bestEstimatedCost) return;
          bestEstimatedCost = estimatedCost;
          bestOp = op;
          LLVM_DEBUG(llvm::dbgs() << "// new best op: '" << bestOp->getName()
                                  << "', cost: " << bestEstimatedCost << "\n");
        })
        // TODO(scotttodd): IREE::LinalgExt::LinalgExtOp
        .Default([&](Operation *op) {
          // No cost estimation implemented, skip.
        });
  });
  if (!bestOp) return "";

  std::string bestSummary = "";
  TypeSwitch<Operation *>(bestOp)
      .Case<linalg::LinalgOp>(
          [&](auto op) { bestSummary = summarizeLinalgOp(op); })
      // TODO(scotttodd): IREE::LinalgExt::LinalgExtOp
      .Default([&](Operation *op) {
        // No summarization implemented, default to the op's name.
        bestSummary = op->getName().getStringRef().str();
      });

  LLVM_DEBUG(llvm::dbgs() << "// best op summary: '" << bestSummary << "'\n");
  return bestSummary;
}

// Creates a flow.executable out of a set of functions, pulling in all other
// functions reachable by the provided functions.
static ExecutableOp createExecutable(Location loc, StringRef executableName,
                                     ArrayRef<mlir::func::FuncOp> funcOps,
                                     ModuleOp parentModuleOp) {
  assert(!funcOps.empty() && "must have at least one entry function");

  // Create the executable that will contain the outlined region.
  // NOTE: this will get uniquified if we have multiple in the same block.
  OpBuilder parentModuleBuilder(&parentModuleOp.getBody()->back());
  auto executableOp =
      parentModuleBuilder.create<IREE::Flow::ExecutableOp>(loc, executableName);

  // Create the inner ModuleOp that contains the original functions. We need
  // to provide this shim as some ops (like std.call) look for the
  // containing module to provide symbol resolution.
  OpBuilder executableBuilder(executableOp);
  executableBuilder.setInsertionPointToStart(&executableOp.getBlock());
  auto innerModule = executableBuilder.create<mlir::ModuleOp>(loc);
  for (auto funcOp : funcOps) {
    innerModule.push_back(funcOp);
  }

  // Copy all reachable functions into the executable.
  // Linker passes may dedupe these later on.
  OpBuilder innerModuleBuilder = OpBuilder::atBlockEnd(innerModule.getBody());
  innerModuleBuilder.setInsertionPoint(innerModule.getBody(),
                                       ++innerModule.getBody()->begin());

  return executableOp;
}

// Converts a dispatch region op into a dispatch op to the outlined region.
static LogicalResult convertToDispatchOp(DispatchWorkgroupsOp regionOp,
                                         ExecutableOp executableOp,
                                         ExecutableExportOp exportOp) {
  // Insert at the same place as the original region.
  OpBuilder builder(regionOp);

  // Create the dispatch op to the executable function.
  // Note that we copy the tied operand indices from the workgroups op - it
  // lines up 1:1 with the dispatch once we've outlined things.
  auto dispatchOp = builder.create<DispatchOp>(
      regionOp.getLoc(), exportOp, regionOp.getWorkload(),
      regionOp.getResultTypes(), regionOp.getResultDims(),
      regionOp.getArguments(), regionOp.getArgumentDims(),
      regionOp.getTiedOperandsAttr());
  dispatchOp->setDialectAttrs(regionOp->getDialectAttrs());

  // Replace uses of the existing results with the new results.
  for (int i = 0; i < regionOp.getNumResults(); ++i) {
    regionOp.getResult(i).replaceAllUsesWith(dispatchOp.getResult(i));
  }

  // Erase original region.
  regionOp.erase();

  return success();
}

// Converts a dispatch region body to a free-floating function.
static mlir::func::FuncOp createWorkgroupFunc(Location loc,
                                              StringRef functionName,
                                              Region &region) {
  // Build function type matching the region signature.
  auto functionType = FunctionType::get(
      region.getContext(), region.getArgumentTypes(), /*results=*/{});

  // Clone region into the function body.
  auto funcOp = mlir::func::FuncOp::create(loc, functionName, functionType);
  IRMapping mapping;
  region.cloneInto(&funcOp.getFunctionBody(), mapping);

  // Replace flow.return with std.return.
  // NOTE: in the dispatch workgroups case the return should have no values.
  for (auto &block : funcOp.getBlocks()) {
    if (auto returnOp = dyn_cast<IREE::Flow::ReturnOp>(block.back())) {
      OpBuilder builder(returnOp);
      builder.create<mlir::func::ReturnOp>(
          returnOp.getLoc(), llvm::to_vector<4>(returnOp.getOperands()));
      returnOp.erase();
    }
  }

  return funcOp;
}

// Outlines a dispatch region into a flow.executable and replaces the region op
// with a dispatch to that outlined executable.
static LogicalResult outlineDispatchWorkgroupsOp(
    std::string executableOpName, std::string exportOpName,
    DispatchWorkgroupsOp regionOp) {
  // Convert the region to a free-floating function.
  auto workgroupFuncOp = createWorkgroupFunc(regionOp.getLoc(), exportOpName,
                                             regionOp.getWorkgroupBody());
  if (!workgroupFuncOp) {
    return failure();
  }

  // Create the executable with the region cloned into it.
  auto parentFuncOp = regionOp->getParentOfType<FunctionOpInterface>();
  auto executableOp =
      createExecutable(regionOp.getLoc(), executableOpName, {workgroupFuncOp},
                       parentFuncOp->getParentOfType<mlir::ModuleOp>());
  executableOp.getOperation()->moveBefore(parentFuncOp);
  executableOp.setPrivate();

  // Add an export pointing at the entry point function.
  OpBuilder builder(executableOp.getBody());
  auto exportOp = builder.create<ExecutableExportOp>(
      regionOp.getLoc(), workgroupFuncOp.getName(),
      SymbolRefAttr::get(workgroupFuncOp));
  if (!regionOp.getWorkgroupCount().empty())
    exportOp.getWorkgroupCount().takeBody(regionOp.getWorkgroupCount());

  // Move over the workgroup count region, if present.
  if (!regionOp.getWorkgroupCount().empty()) {
    exportOp.getWorkgroupCount().takeBody(regionOp.getWorkgroupCount());
  }

  // Finally convert the dispatch region into a dispatch to the outlined func.
  return convertToDispatchOp(regionOp, executableOp, exportOp);
}

}  // namespace

class OutlineDispatchRegionsPass
    : public OutlineDispatchRegionsBase<OutlineDispatchRegionsPass> {
 public:
  OutlineDispatchRegionsPass() = default;

  void runOnOperation() override {
    // Convert each dispatch region into a flow.executable + dispatch op.
    int initializerCount = 0;
    for (auto it :
         llvm::enumerate(getOperation().getOps<FunctionOpInterface>())) {
      FunctionOpInterface op = it.value();
      Operation *operation = op;

      // Generate a nice name if possible.
      std::string namePrefix;
      if (auto funcOp = llvm::dyn_cast<mlir::func::FuncOp>(operation)) {
        namePrefix = funcOp.getName().str();
      } else if (llvm::isa<IREE::Util::InitializerOp>(operation)) {
        namePrefix =
            std::string("_initializer_") + std::to_string(initializerCount++);
      } else {
        namePrefix =
            std::string("_function_like_") + std::to_string(it.index());
      }

      auto &bodyRegion = op.getFunctionBody();
      // Outline all of the dispatch regions ops in this function.
      auto dispatchWorkgroupsOps =
          llvm::to_vector<8>(bodyRegion.getOps<DispatchWorkgroupsOp>());
      for (int i = 0; i < dispatchWorkgroupsOps.size(); ++i) {
        std::string executableOpName =
            (namePrefix + "_dispatch_" + llvm::Twine(i)).str();
        // Add a summary of the op as a suffix, if one can be generated.
        // Note: the executable names omit this suffix so their names are more
        // predictable.
        LLVM_DEBUG(llvm::dbgs()
                   << "//--- summarizing '" << executableOpName << "' ---//\n");
        std::string opSummary =
            summarizeDispatchWorkgroupsOp(dispatchWorkgroupsOps[i]);
        LLVM_DEBUG(llvm::dbgs()
                   << "//--- opSummary: '" << opSummary << "' ---//\n\n");
        std::string opSuffix = opSummary.empty() ? "" : "_" + opSummary;
        std::string exportOpName = executableOpName + opSuffix;
        if (failed(outlineDispatchWorkgroupsOp(executableOpName, exportOpName,
                                               dispatchWorkgroupsOps[i]))) {
          return signalPassFailure();
        }
      }
    }
  }
};

std::unique_ptr<OperationPass<mlir::ModuleOp>>
createOutlineDispatchRegionsPass() {
  return std::make_unique<OutlineDispatchRegionsPass>();
}

}  // namespace Flow
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir
