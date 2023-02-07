// Copyright 2020 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Dialect/Util/IR/UtilTypes.h"

#include "iree/compiler/Dialect/Util/IR/UtilDialect.h"
#include "iree/compiler/Dialect/Util/IR/UtilOps.h"
#include "llvm/ADT/BitVector.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/FunctionInterfaces.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/TypeSupport.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Interfaces/CastInterfaces.h"
#include "mlir/Parser/Parser.h"

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace Util {

//===----------------------------------------------------------------------===//
// !util.buffer
//===----------------------------------------------------------------------===//

bool BufferType::isAccessStorageCompatible(Type accessType) const {
  return accessType.isa<IREE::Util::BufferType>();
}

Value BufferType::inferSizeFromValue(Location loc, Value value,
                                     OpBuilder &builder) const {
  return builder.createOrFold<IREE::Util::BufferSizeOp>(
      loc, builder.getIndexType(), value);
}

Value BufferType::createSubrangeOp(Location loc, Value resource,
                                   Value resourceSize, Value subrangeOffset,
                                   Value subrangeLength,
                                   OpBuilder &builder) const {
  return builder.create<IREE::Util::BufferSubspanOp>(
      loc, resource, resourceSize, subrangeOffset, subrangeLength);
}

//===----------------------------------------------------------------------===//
// !util.list<T>
//===----------------------------------------------------------------------===//

static LogicalResult parseListElementType(AsmParser &parser,
                                          Type &elementType) {
  if (succeeded(parser.parseOptionalQuestion())) {
    elementType = IREE::Util::VariantType::get(parser.getContext());
    return success();
  }
  Type type;
  if (succeeded(parser.parseType(type))) {
    elementType = type;
    return success();
  }
  return failure();
}

static void printListElementType(AsmPrinter &printer, Type elementType) {
  if (elementType.isa<IREE::Util::VariantType>()) {
    printer << "?";
  } else {
    printer << elementType;
  }
}

// static
bool ListType::isCompatible(Type type) { return true; }

// static
bool ListType::canImplicitlyCast(Type from, Type to) {
  if (from.isa<VariantType>() || to.isa<VariantType>()) {
    return true;
  } else if (from.isa<TensorType>() && to.isa<TensorType>()) {
    return true;
  }
  return from == to;
}

// static
LogicalResult ListType::verify(function_ref<InFlightDiagnostic()> emitError,
                               Type elementType) {
  if (!isCompatible(elementType)) {
    return emitError() << "invalid element type for a list: " << elementType;
  }
  return success();
}

//===----------------------------------------------------------------------===//
// !util.ptr<T>
//===----------------------------------------------------------------------===//

// static
bool PtrType::isCompatible(Type type) { return true; }

// static
LogicalResult PtrType::verify(function_ref<InFlightDiagnostic()> emitError,
                              Type targetType) {
  if (!isCompatible(targetType)) {
    return emitError() << "invalid target type for a pointer: " << targetType;
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Op utilities common in util patterns and folders
//===----------------------------------------------------------------------===//

// Walks up the ancestors of |sourceBlock| until |targetBlock| is reached.
// Returns an insertion point in the |targetBlock|.
static std::pair<Block *, Block::iterator> findCommonBlockInsertionPoint(
    Block *targetBlock, Block *sourceBlock,
    Block::iterator sourceInsertionPoint) {
  auto *ancestorOp = targetBlock->findAncestorOpInBlock(*sourceInsertionPoint);
  if (ancestorOp) {
    return std::make_pair(ancestorOp->getBlock(), Block::iterator(ancestorOp));
  }
  return std::make_pair(sourceBlock, sourceInsertionPoint);
}

bool isValueUsableForOp(Value value, Block *block,
                        Block::iterator insertionPoint) {
  // If the insertion point is nested within an op in the defining block we can
  // use the parent op as the insertion point to check.
  auto *definingBlock = value.getParentBlock();
  std::tie(block, insertionPoint) =
      findCommonBlockInsertionPoint(definingBlock, block, insertionPoint);
  if (block == nullptr) {
    // Op is not in a block; can't analyze (maybe?).
    return false;
  }
  if (definingBlock == block) {
    // Defined in the same block; ensure block order.
    if (value.isa<BlockArgument>()) return true;
    if (insertionPoint == block->end()) return true;
    if (value.getDefiningOp()->isBeforeInBlock(&*insertionPoint)) {
      return true;
    }
  } else if (definingBlock->isEntryBlock() &&
             llvm::isa<FunctionOpInterface>(definingBlock->getParentOp())) {
    // Function entry block always dominates - fast path for constants.
    return true;
  } else {
    // See if block the value is defined in dominates the forOp block.
    // TODO(benvanik): optimize this, it's terribly expensive to recompute.
    DominanceInfo dominanceInfo(block->getParentOp());
    return dominanceInfo.dominates(definingBlock, block);
  }
  return false;
}

bool isValueUsableForOp(Value value, Operation *op) {
  return isValueUsableForOp(value, op->getBlock(), Block::iterator(op));
}

bool tryMoveProducerBefore(Value value, Operation *consumerOp) {
  auto *producerOp = value.getDefiningOp();
  if (!producerOp) return true;  // block arg, ok to use
  if (producerOp->getBlock() == consumerOp->getBlock()) {
    if (producerOp->isBeforeInBlock(consumerOp)) return true;
    for (auto operand : producerOp->getOperands()) {
      if (!isValueUsableForOp(operand, consumerOp)) return false;
    }
    producerOp->moveBefore(consumerOp);
    return true;
  }
  // Could extend this - really need a shared helper function.
  return false;
}

//===----------------------------------------------------------------------===//
// Global and structural interface utilities
//===----------------------------------------------------------------------===//

// Returns true if the given |accessType| is compatible with the |globalType|.
// For example, this will return true if the global type is a tensor<?xf32>
// and the access is tensor<4xf32>.
static bool isGlobalTypeCompatible(Type globalType, Type accessType) {
  // If one is a shaped type, then they both must be and have compatible
  // shapes.
  if (globalType.isa<ShapedType>() && accessType.isa<ShapedType>()) {
    return succeeded(mlir::verifyCompatibleShape(globalType, accessType));
  }

  if (auto knownType = globalType.dyn_cast<IREE::Util::GlobalTypeInterface>()) {
    return knownType.isAccessStorageCompatible(accessType);
  }

  // Otherwise, the types must be the same.
  return globalType == accessType;
}

LogicalResult detail::verifyGlobalOp(IREE::Util::GlobalOpInterface globalOp) {
  auto initialValue = globalOp.getGlobalInitialValue();
  if (auto typedInitialValue = initialValue.dyn_cast_or_null<TypedAttr>()) {
    // Ensure the value is something we can convert to a const.
    if (!isGlobalTypeCompatible(globalOp.getGlobalType(),
                                typedInitialValue.getType())) {
      return globalOp->emitOpError()
             << "initial value type mismatch; global "
             << globalOp.getGlobalName() << " is " << globalOp.getGlobalType()
             << " but initial value provided is "
             << typedInitialValue.getType();
    }
  }
  return success();
}

LogicalResult detail::verifyGlobalAddressOp(
    GlobalAddressOpInterface addressOp, SymbolTableCollection &symbolTable) {
  if (!isa_and_nonnull<IREE::Util::GlobalOpInterface>(
          symbolTable.lookupNearestSymbolFrom(addressOp.getOperation(),
                                              addressOp.getGlobalAttr()))) {
    return addressOp->emitOpError(
        "attribute 'global' failed to satisfy constraint: flat symbol "
        "reference attribute referencing to a 'IREE::Util::GlobalOpInterface' "
        "symbol");
  }
  auto globalOp =
      lookupGlobalOp(addressOp, addressOp.getGlobalAttr(), symbolTable);
  if (!globalOp) {
    return addressOp->emitOpError()
           << "undefined global: " << addressOp.getGlobalAttr();
  }
  // TODO(benvanik): allow type conversion here? probably better on the indirect
  // access ops instead as it's then easier to fold the conversion.
  return success();
}

LogicalResult detail::verifyGlobalLoadOp(GlobalLoadOpInterface loadOp,
                                         SymbolTableCollection &symbolTable) {
  auto globalOp = lookupGlobalOp(loadOp, loadOp.getGlobalAttr(), symbolTable);
  if (!globalOp) {
    return loadOp->emitOpError()
           << "undefined global: " << loadOp.getGlobalAttr();
  }
  auto loadType = loadOp->getResult(0).getType();
  if (!isGlobalTypeCompatible(globalOp.getGlobalType(), loadType)) {
    return loadOp->emitOpError()
           << "global type mismatch; global " << globalOp.getGlobalName()
           << " is " << globalOp.getGlobalType() << " but load is " << loadType;
  }
  return success();
}

LogicalResult detail::verifyGlobalStoreOp(GlobalStoreOpInterface storeOp,
                                          SymbolTableCollection &symbolTable) {
  auto globalOp = lookupGlobalOp(storeOp, storeOp.getGlobalAttr(), symbolTable);
  if (!globalOp) {
    return storeOp->emitOpError()
           << "undefined global: " << storeOp.getGlobalAttr();
  }
  auto storeType = storeOp.getStoredGlobalValue().getType();
  if (globalOp.getGlobalType() != storeType) {
    return storeOp->emitOpError()
           << "global type mismatch; global " << globalOp.getGlobalName()
           << " is " << globalOp.getGlobalType() << " but store is "
           << storeType;
  }
  if (!globalOp.isGlobalMutable()) {
    // Allow stores to immutable globals in initializers.
    if (!storeOp->getParentOfType<IREE::Util::InitializerOpInterface>()) {
      return storeOp->emitOpError()
             << "global " << globalOp.getGlobalName()
             << " is not mutable and cannot be stored to";
    }
  }
  return success();
}

IREE::Util::GlobalOpInterface lookupGlobalOp(
    Operation *accessorOp, SymbolRefAttr globalRefAttr,
    SymbolTableCollection &symbolTable) {
  return symbolTable.lookupNearestSymbolFrom<IREE::Util::GlobalOpInterface>(
      accessorOp->getParentOp(), globalRefAttr);
}

//===----------------------------------------------------------------------===//
// IREE::Util::TiedOpInterface
//===----------------------------------------------------------------------===//

llvm::Optional<unsigned> detail::getTiedResultOperandIndex(
    Operation *op, unsigned resultIndex) {
  auto storageAttr =
      op->getAttrOfType<ArrayAttr>(TiedOpInterface::getStorageAttrName());
  if (!storageAttr) return std::nullopt;
  auto valueAttrs = storageAttr.getValue();
  if (valueAttrs.empty()) return std::nullopt;
  auto tiedOp = cast<TiedOpInterface>(op);
  auto indexAndLength = tiedOp.getTiedResultsIndexAndLength();
  if (resultIndex < indexAndLength.first) return std::nullopt;
  resultIndex -= indexAndLength.first;
  if (resultIndex >= indexAndLength.second) return std::nullopt;
  int64_t value = valueAttrs[resultIndex].cast<IntegerAttr>().getInt();
  if (value == TiedOpInterface::kUntiedIndex) return std::nullopt;
  unsigned tiedOperandsOffset = tiedOp.getTiedOperandsIndexAndLength().first;
  return tiedOperandsOffset + static_cast<unsigned>(value);
}

void detail::setTiedResultOperandIndex(Operation *op, unsigned resultIndex,
                                       llvm::Optional<unsigned> operandIndex) {
  auto tiedOp = cast<TiedOpInterface>(op);
  auto resultRange = tiedOp.getTiedResultsIndexAndLength();
  resultIndex -= resultRange.first;

  auto indices = getTiedResultOperandIndices(op);
  if (indices.empty()) {
    indices.resize(resultRange.second, TiedOpInterface::kUntiedIndex);
  } else {
    // Well, getTiedResultOperandIndices() returns indices into the full range
    // of the op, but in the attribute, we expect to store ranges into the range
    // returned by `getTiedOperandsIndexAndLength`.
    unsigned tiedOperandsOffset = tiedOp.getTiedOperandsIndexAndLength().first;
    for (auto &index : indices) {
      if (index != TiedOpInterface::kUntiedIndex) index -= tiedOperandsOffset;
    }
  }

  indices[resultIndex] = operandIndex.value_or(TiedOpInterface::kUntiedIndex);
  op->setAttr(TiedOpInterface::getStorageAttrName(),
              Builder(op).getIndexArrayAttr(indices));
}

SmallVector<int64_t, 4> detail::getTiedResultOperandIndices(Operation *op) {
  SmallVector<int64_t, 4> indices;
  auto storageAttr =
      op->getAttrOfType<ArrayAttr>(TiedOpInterface::getStorageAttrName());
  if (!storageAttr) return indices;
  auto valueAttrs = storageAttr.getValue();
  if (valueAttrs.empty()) return indices;
  auto tiedOp = cast<TiedOpInterface>(op);
  auto resultRange = tiedOp.getTiedResultsIndexAndLength();
  unsigned tiedOperandsOffset = tiedOp.getTiedOperandsIndexAndLength().first;
  indices.resize(resultRange.second);
  for (unsigned i = 0; i < valueAttrs.size(); ++i) {
    int64_t index = valueAttrs[i].cast<IntegerAttr>().getInt();
    indices[i] = index != TiedOpInterface::kUntiedIndex
                     ? tiedOperandsOffset + index
                     : TiedOpInterface::kUntiedIndex;
  }
  return indices;
}

// static
Value TiedOpInterface::findTiedBaseValue(Value derivedValue) {
  Value baseValue = derivedValue;
  while (auto definingOp =
             dyn_cast_or_null<TiedOpInterface>(baseValue.getDefiningOp())) {
    auto tiedValue = definingOp.getTiedResultOperand(baseValue);
    if (!tiedValue) break;
    baseValue = tiedValue;
  }
  return baseValue;
}

// static
bool TiedOpInterface::hasAnyTiedUses(Value value) {
  for (auto &use : value.getUses()) {
    auto tiedOp = dyn_cast<IREE::Util::TiedOpInterface>(use.getOwner());
    if (!tiedOp) continue;
    if (tiedOp.isOperandTied(use.getOperandNumber())) return true;
  }
  return false;
}

bool detail::isOperandTied(Operation *op, unsigned operandIndex) {
  auto tiedOp = dyn_cast<TiedOpInterface>(op);
  if (!tiedOp) return false;
  auto tiedIndices = tiedOp.getTiedResultOperandIndices();
  for (unsigned i = 0; i < tiedIndices.size(); ++i) {
    if (tiedIndices[i] == operandIndex) {
      return true;
    }
  }
  return false;
}

SmallVector<Value> detail::getOperandTiedResults(Operation *op,
                                                 unsigned operandIndex) {
  auto tiedOp = dyn_cast<TiedOpInterface>(op);
  if (!tiedOp) return {};
  auto resultRange = tiedOp.getTiedResultsIndexAndLength();
  SmallVector<Value> results;
  auto tiedIndices = tiedOp.getTiedResultOperandIndices();
  for (unsigned i = 0; i < tiedIndices.size(); ++i) {
    if (tiedIndices[i] == operandIndex) {
      results.push_back(op->getResult(resultRange.first + i));
    }
  }
  return results;
}

LogicalResult detail::verifyTiedOp(TiedOpInterface tiedOp) {
  auto tiedOperandIndices = tiedOp.getTiedResultOperandIndices();
  if (tiedOperandIndices.empty()) return success();
  auto resultRange = tiedOp.getTiedResultsIndexAndLength();
  if (tiedOperandIndices.size() != resultRange.second) {
    return tiedOp.emitError("op results/tied operand indices mismatch");
  }
  return success();
}

void excludeTiedOperandAndResultIndices(
    ArrayRef<unsigned> excludedOperandIndices,
    ArrayRef<unsigned> excludedResultIndices,
    SmallVector<int64_t, 4> &tiedOperandIndices) {
  SmallVector<int64_t, 4> oldTiedOperandIndices = tiedOperandIndices;
  tiedOperandIndices.clear();

  // To adjust operand indices we need to know the how many operands to offset
  // the indices by - if 2 operands before operand N were removed then we know
  // it needs to be -2. This is nasty but that's why we have this helper
  // function.
  unsigned numBits = 1;
  if (!excludedOperandIndices.empty()) {
    numBits += *std::max_element(excludedOperandIndices.begin(),
                                 excludedOperandIndices.end());
  }
  llvm::BitVector excludedOperands(numBits, false);
  for (unsigned i = 0; i < excludedOperandIndices.size(); ++i) {
    excludedOperands[excludedOperandIndices[i]] = true;
  }

  for (auto it : llvm::enumerate(oldTiedOperandIndices)) {
    unsigned resultIndex = it.index();
    if (llvm::is_contained(excludedResultIndices, resultIndex)) {
      continue;  // result removed
    }

    int64_t tiedOperandIndex = it.value();
    if (tiedOperandIndex != TiedOpInterface::kUntiedIndex) {
      // Check whether this operand is removed. If so, untie. We need to do this
      // before calculating the new operand index given `excludedOperandIndices`
      // contains the old indices.
      if (llvm::is_contained(excludedOperandIndices, tiedOperandIndex)) {
        tiedOperandIndex = TiedOpInterface::kUntiedIndex;
      }

      // Count up the number of removed operands prior to this one.
      unsigned offset = 0;
      for (unsigned i = 0; i < tiedOperandIndex; ++i) {
        if (i < excludedOperands.size() && excludedOperands[i]) ++offset;
      }

      tiedOperandIndex -= offset;
    }
    tiedOperandIndices.push_back(tiedOperandIndex);
  }
}

//===----------------------------------------------------------------------===//
// IREE::Util::SizeAwareTypeInterface
//===----------------------------------------------------------------------===//

// static
Value SizeAwareTypeInterface::findSizeValue(Value resourceValue, Block *block,
                                            Block::iterator insertionPoint) {
  // See if the value is produced by a size-aware op; we can just ask for the
  // size it has tied. Walking upward is always good as we know any size we find
  // dominates {|block|, |insertionPoint|}.
  SmallVector<Value> worklist;
  worklist.push_back(resourceValue);
  while (!worklist.empty()) {
    auto value = worklist.pop_back_val();
    auto *definingOp = value.getDefiningOp();
    if (!definingOp) continue;
    if (auto sizeAwareOp =
            llvm::dyn_cast<IREE::Util::SizeAwareOpInterface>(definingOp)) {
      return sizeAwareOp.getResultSizeFromValue(value);
    }
    if (auto tiedOp = llvm::dyn_cast<IREE::Util::TiedOpInterface>(definingOp)) {
      auto tiedOperand = tiedOp.getTiedResultOperand(value);
      if (tiedOperand) worklist.push_back(tiedOperand);
    }
  }

  // Walk the users to see if any can report the size.
  worklist.push_back(resourceValue);
  while (!worklist.empty()) {
    auto value = worklist.pop_back_val();
    for (auto &use : value.getUses()) {
      if (auto sizeAwareOp = llvm::dyn_cast<IREE::Util::SizeAwareOpInterface>(
              use.getOwner())) {
        auto sizeValue = sizeAwareOp.getOperandSize(use.getOperandNumber());
        if (sizeValue) {
          if (isValueUsableForOp(sizeValue, block, insertionPoint)) {
            return sizeValue;
          }
        }
      }
      if (auto tiedOp =
              llvm::dyn_cast<IREE::Util::TiedOpInterface>(use.getOwner())) {
        worklist.append(tiedOp.getOperandTiedResults(use.getOperandNumber()));
      }
    }
  }

  return {};
}

// static
Value SizeAwareTypeInterface::queryValueSize(Location loc, Value resourceValue,
                                             OpBuilder &builder) {
  auto sizeAwareType =
      resourceValue.getType().dyn_cast<IREE::Util::SizeAwareTypeInterface>();
  if (!sizeAwareType) {
    return {};  // Not a sized type.
  }
  if (!builder.getInsertionPoint().getNodePtr()->isKnownSentinel()) {
    auto sizeValue = sizeAwareType.findSizeValue(
        resourceValue, builder.getBlock(), builder.getInsertionPoint());
    if (sizeValue) {
      return sizeValue;  // Found in IR.
    }
  }
  // TODO(benvanik): make this cleaner.
  auto *definingOp = resourceValue.getDefiningOp();
  if (auto sizeAwareOp =
          llvm::dyn_cast_or_null<IREE::Util::SizeAwareOpInterface>(
              definingOp)) {
    return sizeAwareOp.getResultSizeFromValue(resourceValue);
  } else if (auto inferSizeType =
                 resourceValue.getType()
                     .dyn_cast<IREE::Util::InferTypeSizeInterface>()) {
    return inferSizeType.inferSizeFromValue(loc, resourceValue, builder);
  }
  return {};
}

//===----------------------------------------------------------------------===//
// IREE::Util::ShapeAware*
//===----------------------------------------------------------------------===//

Optional<ValueRange> findDynamicDims(Value shapedValue) {
  // Look up the use-def chain: always safe, as any value we reach dominates
  // {|block|, |insertionPoint|} implicitly.
  SmallVector<Value> worklist;
  worklist.push_back(shapedValue);
  while (!worklist.empty()) {
    auto workValue = worklist.pop_back_val();
    auto workOp = workValue.getDefiningOp();
    if (!workOp) continue;
    if (auto shapeAwareOp = dyn_cast<ShapeAwareOpInterface>(workOp)) {
      return shapeAwareOp.getResultDynamicDimsFromValue(workValue);
    } else if (auto tiedOp = dyn_cast<TiedOpInterface>(workOp)) {
      auto tiedValue = tiedOp.getTiedResultOperand(workValue);
      if (tiedValue) worklist.push_back(tiedValue);
    }
  }
  return std::nullopt;
}

Optional<ValueRange> findDynamicDims(Value shapedValue, Block *block,
                                     Block::iterator insertionPoint) {
  // Look up the use-def chain: always safe, as any value we reach dominates
  // {|block|, |insertionPoint|} implicitly.
  auto upwardRange = findDynamicDims(shapedValue);
  if (upwardRange.has_value()) return upwardRange.value();

  // Look down the use-def chain: not safe at some point because we'll move past
  // where {|block|, |insertionPoint|} is dominated. This is often fine for a
  // bit, though, as {|block|, |insertionPoint|} may be a user of |shapedValue|
  // and be able to provide the shape itself.
  for (auto &use : shapedValue.getUses()) {
    if (auto shapeAwareOp = dyn_cast<ShapeAwareOpInterface>(use.getOwner())) {
      auto dynamicDims =
          shapeAwareOp.getOperandDynamicDims(use.getOperandNumber());
      if (llvm::all_of(dynamicDims, [&](Value dim) {
            return isValueUsableForOp(dim, block, insertionPoint);
          })) {
        return dynamicDims;
      }
    }
  }

  return std::nullopt;
}

ValueRange findVariadicDynamicDims(unsigned idx, ValueRange values,
                                   ValueRange dynamicDims) {
  auto value = values[idx];
  auto shapedType = value.getType().dyn_cast<ShapedType>();
  if (!shapedType) return ValueRange{};

  // Bail immediately if the shape is static.
  if (shapedType.hasStaticShape()) return ValueRange{};

  // Find where the dynamic dims start in the flattened list.
  unsigned offset = 0;
  for (unsigned i = 0; i < idx; ++i) {
    if (auto type = values[i].getType().dyn_cast<ShapedType>()) {
      offset += type.getNumDynamicDims();
    }
  }

  // Return the subrange of dynamic dims for the value being queried.
  return dynamicDims.slice(offset, shapedType.getNumDynamicDims());
}

SmallVector<Value> buildDynamicDimsForValue(Location loc, Value value,
                                            OpBuilder &builder) {
  auto valueType = value.getType().dyn_cast<ShapedType>();
  if (!valueType) {
    mlir::emitError(loc) << "cannot construct shape for non shaped value: "
                         << value.getType();
    return {};
  }

  // Early-exit if all dimensions are static.
  if (valueType.hasStaticShape()) {
    return {};
  }

  // Try the fast-path of scanning for the dynamic dims that exist in the IR
  // already. For shape-aware ops this is free as the dynamic dim SSA values are
  // always available.
  auto foundDynamicDims = IREE::Util::findDynamicDims(
      value, builder.getBlock(), builder.getInsertionPoint());
  if (foundDynamicDims.has_value()) {
    return llvm::to_vector<4>(foundDynamicDims.value());
  }

  // Slower path that materializes the entire shape for a result. Some
  // implementations may only support this (vs the fast find above).
  if (auto shapeAwareOp = dyn_cast_or_null<IREE::Util::ShapeAwareOpInterface>(
          value.getDefiningOp())) {
    return shapeAwareOp.buildResultValueShape(value, builder);
  }

  // TODO(benvanik): add support for ReifyRankedShapedTypeOpInterface;
  // unfortunately it is for all results and all dimensions so a lot of unneeded
  // IR will be inserted.

  // Fallback to inserting dim ops that can be resolved via normal upstream
  // mechanisms. Depending on where this is called from within the parent
  // pipeline these ops may not be desirable, but that's what the
  // ShapeAwareOpInterface is for.
  SmallVector<Value> dynamicDims;
  for (unsigned i = 0; i < valueType.getRank(); ++i) {
    if (valueType.isDynamicDim(i)) {
      dynamicDims.push_back(builder.createOrFold<tensor::DimOp>(loc, value, i));
    }
  }
  return dynamicDims;
}

static SmallVector<Value> buildShape(Location loc, ShapedType type,
                                     ValueRange dynamicDims,
                                     OpBuilder &builder) {
  SmallVector<Value> dims;
  dims.reserve(type.getRank());
  unsigned dynamicIdx = 0;
  for (unsigned i = 0; i < type.getRank(); ++i) {
    int64_t dim = type.getDimSize(i);
    if (dim == ShapedType::kDynamic) {
      dims.push_back(dynamicDims[dynamicIdx++]);
    } else {
      dims.push_back(builder.create<arith::ConstantIndexOp>(loc, dim));
    }
  }
  return dims;
}

SmallVector<Value> buildOperandShape(ShapeAwareOpInterface op,
                                     unsigned operandIdx, OpBuilder &builder) {
  auto operand = op->getOperand(operandIdx);
  auto type = operand.getType().cast<ShapedType>();
  auto dynamicDims = op.getOperandDynamicDims(operandIdx);
  return buildShape(op.getLoc(), type, dynamicDims, builder);
}

SmallVector<Value> buildResultShape(ShapeAwareOpInterface op,
                                    unsigned resultIdx, OpBuilder &builder) {
  auto result = op->getResult(resultIdx);
  auto type = result.getType().cast<ShapedType>();
  auto dynamicDims = op.getResultDynamicDims(resultIdx);
  return buildShape(op.getLoc(), type, dynamicDims, builder);
}

}  // namespace Util
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir

//===----------------------------------------------------------------------===//
// IREE::Util::UtilDialect
//===----------------------------------------------------------------------===//

// clang-format off: must be included after all LLVM/MLIR headers.
#define GET_TYPEDEF_CLASSES
#include "iree/compiler/Dialect/Util/IR/UtilTypes.cpp.inc"  // IWYU pragma: keep
// clang-format on

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace Util {

// At the end so it can use functions above:
#include "iree/compiler/Dialect/Util/IR/UtilOpInterfaces.cpp.inc"
#include "iree/compiler/Dialect/Util/IR/UtilTypeInterfaces.cpp.inc"

void UtilDialect::registerTypes() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "iree/compiler/Dialect/Util/IR/UtilTypes.cpp.inc"  // IWYU pragma: keep
      >();
}

}  // namespace Util
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir
