// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Utils/ModuleUtils.h"

#include "iree/compiler/Utils/StringUtils.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Operation.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Support/LLVM.h"

namespace mlir {
namespace iree_compiler {

static llvm::Optional<FileLineColLoc> findFirstFileLoc(Location baseLoc) {
  if (auto loc = baseLoc.dyn_cast<FusedLoc>()) {
    for (auto &childLoc : loc.getLocations()) {
      auto childResult = findFirstFileLoc(childLoc);
      if (childResult) return childResult;
    }
  } else if (auto loc = baseLoc.dyn_cast<FileLineColLoc>()) {
    return loc;
  }
  return std::nullopt;
}

std::string guessModuleName(mlir::ModuleOp moduleOp, StringRef defaultName) {
  std::string moduleName = moduleOp.getName().value_or("").str();
  if (!moduleName.empty()) return moduleName;
  auto loc = findFirstFileLoc(moduleOp.getLoc());
  if (loc.has_value()) {
    return sanitizeSymbolName(
        llvm::sys::path::stem(loc.value().getFilename()).str());
  } else {
    return defaultName.str();
  }
}

// Renames |op| within |moduleOp| with a new name that is unique within both
// |moduleOp| and |symbolTable|.
static void renameWithDisambiguatedName(Operation *op, Operation *moduleOp,
                                        SymbolTable &symbolTable0,
                                        SymbolTable &symbolTable1) {
  StringRef originalName = SymbolTable::getSymbolName(op).getValue();

  // This could stand to be rewritten noting that nested symbol refs exist.

  // Iteratively try suffixes until we find one that isn't used.
  // It'd be nice if there was a SymbolTable::getUniqueName or something.
  std::string disambiguatedName;
  int uniqueingCounter = 0;
  do {
    disambiguatedName =
        llvm::formatv("{0}_{1}", originalName, uniqueingCounter++).str();
  } while (symbolTable0.lookup(disambiguatedName) ||
           symbolTable1.lookup(disambiguatedName));

  // HORRENDOUS: this needs to be rewritten; we're walking the entire module
  // each time to do this!
  SymbolTableCollection symbolTables;
  SymbolUserMap symbolUsers(symbolTables, moduleOp);
  mlir::StringAttr nameAttr =
      mlir::StringAttr::get(op->getContext(), disambiguatedName);
  symbolUsers.replaceAllUsesWith(op, nameAttr);
  SymbolTable::setSymbolName(op, disambiguatedName);
}

LogicalResult mergeModuleInto(Operation *sourceModuleOp,
                              Operation *targetModuleOp,
                              OpBuilder &targetBuilder) {
  // Capture source information we need prior to destructively merging.
  SymbolTable sourceSymbolTable(sourceModuleOp);
  SymbolTable targetSymbolTable(targetModuleOp);
  auto &sourceBlock = sourceModuleOp->getRegion(0).front();
  auto sourceOps = llvm::to_vector<8>(
      llvm::map_range(sourceBlock, [&](Operation &op) { return &op; }));

  // Resolve conflicts and move the op.
  for (auto &sourceOp : sourceOps) {
    if (sourceOp->hasTrait<OpTrait::IsTerminator>()) continue;
    if (auto symbolOp = dyn_cast<SymbolOpInterface>(sourceOp)) {
      auto symbolName = symbolOp.getName();

      // Resolve symbol name conflicts.
      if (auto targetOp = targetSymbolTable.lookup(symbolName)) {
        if (symbolOp.getVisibility() == SymbolTable::Visibility::Private) {
          // Private symbols can be safely folded into duplicates or renamed.
          if (OperationEquivalence::isEquivalentTo(
                  targetOp, sourceOp, OperationEquivalence::exactValueMatch,
                  /*markEquivalent=*/nullptr,
                  OperationEquivalence::Flags::IgnoreLocations)) {
            // Optimization: skip over duplicate private symbols.
            // We could let CSE do this later, but we may as well check here.
            continue;
          } else {
            // Preserve the op but give it a unique name.
            renameWithDisambiguatedName(sourceOp, sourceModuleOp,
                                        sourceSymbolTable, targetSymbolTable);
          }
        } else {
          // The source symbol has 'nested' or 'public' visibility.
          if (SymbolTable::getSymbolVisibility(targetOp) !=
              SymbolTable::Visibility::Private) {
            // Oops! Both symbols are public and we can't safely rename either.
            // If you hit this with ops that you think are safe to rename, mark
            // them private.
            return sourceOp->emitError()
                   << "multiple public symbols with the name: " << symbolName;
          } else {
            // Keep the original name for our new op, rename the target op.
            renameWithDisambiguatedName(targetOp, targetModuleOp,
                                        sourceSymbolTable, targetSymbolTable);
          }
        }
      }
      sourceOp->moveAfter(targetBuilder.getInsertionBlock(),
                          targetBuilder.getInsertionPoint());
      targetSymbolTable.insert(sourceOp);
      targetBuilder.setInsertionPoint(sourceOp);
    } else {
      sourceOp->moveAfter(targetBuilder.getInsertionBlock(),
                          targetBuilder.getInsertionPoint());
      targetBuilder.setInsertionPoint(sourceOp);
    }
  }

  return success();
}

LogicalResult mergeSourceModuleInto(Location loc, StringRef source,
                                    Operation *targetOp,
                                    OpBuilder &targetBuilder) {
  // Parse the module. This will only fail if the compiler was built wrong;
  // we're loading the embedded files from the compiler binary.
  auto sourceModuleRef = mlir::parseSourceString<mlir::ModuleOp>(
      source, targetBuilder.getContext());
  if (!sourceModuleRef) {
    return mlir::emitError(
        loc, "source module failed to parse; ensure dialects are registered");
  }

  // Merge all of the module contents.
  return mergeModuleInto(*sourceModuleRef, targetOp, targetBuilder);
}

}  // namespace iree_compiler
}  // namespace mlir
