// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_DIALECTS_DIALECT_LINALGEXT_TRANSFORMS_TRANSFORMS_H_
#define IREE_DIALECTS_DIALECT_LINALGEXT_TRANSFORMS_TRANSFORMS_H_

#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtOps.h"
#include "iree-dialects/Dialect/LinalgExt/Passes/Passes.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/SCF/Transforms/TileUsingInterface.h"
#include "mlir/IR/PatternMatch.h"

namespace mlir {
namespace scf {
class ForOp;
class ForeachThreadOp;
} // namespace scf
namespace linalg {
class LinalgOp;
}

namespace iree_compiler {
namespace IREE {
namespace LinalgExt {

/// Pattern to swap a `TilingInterface` op -> `tensor::ExtractSliceOp`.
struct SwapTilingInterfaceOp : public OpRewritePattern<tensor::ExtractSliceOp> {
  using OpRewritePattern<tensor::ExtractSliceOp>::OpRewritePattern;

  FailureOr<Operation *>
  returningMatchAndRewrite(tensor::ExtractSliceOp sliceOp,
                           PatternRewriter &rewriter) const;

  LogicalResult matchAndRewrite(tensor::ExtractSliceOp sliceOp,
                                PatternRewriter &rewriter) const override {
    return returningMatchAndRewrite(sliceOp, rewriter);
  }
};

/// Pattern to rewrite a scf::ForEachThreadOp to the async dialect.
struct ForeachThreadOpToAsyncRewriter
    : public OpRewritePattern<scf::ForeachThreadOp> {
  using OpRewritePattern::OpRewritePattern;

  FailureOr<Operation *>
  returningMatchAndRewrite(scf::ForeachThreadOp foreachThreadOp,
                           PatternRewriter &rewriter) const;

  LogicalResult matchAndRewrite(scf::ForeachThreadOp foreachThreadOp,
                                PatternRewriter &rewriter) const override {
    return returningMatchAndRewrite(foreachThreadOp, rewriter);
  }
};

/// Pattern to rewrite a ForeachThreadOp to an scf::ForOp.
struct ForeachThreadOpToScfForRewriter
    : public OpRewritePattern<scf::ForeachThreadOp> {
  using OpRewritePattern::OpRewritePattern;

  FailureOr<scf::ForOp>
  returningMatchAndRewrite(scf::ForeachThreadOp foreachThreadOp,
                           PatternRewriter &rewriter) const;

  LogicalResult matchAndRewrite(scf::ForeachThreadOp foreachThreadOp,
                                PatternRewriter &rewriter) const override {
    return returningMatchAndRewrite(foreachThreadOp, rewriter);
  }
};

struct FusionResult {
  TilingInterface consumerOp;
  SmallVector<TilingInterface> fusedOps;
};

/// Pattern to fuse the producers of a tileable op.
struct LinalgExtFusionPattern
    : public OpInterfaceRewritePattern<TilingInterface> {
  LinalgExtFusionPattern(MLIRContext *context, ArrayRef<int64_t> operandsToFuse)
      : OpInterfaceRewritePattern<TilingInterface>(context),
        operandsToFuse(operandsToFuse.begin(), operandsToFuse.end()) {}

  FailureOr<FusionResult>
  returningMatchAndRewrite(TilingInterface consumerOp,
                           PatternRewriter &rewriter) const;

  LogicalResult matchAndRewrite(TilingInterface consumerOp,
                                PatternRewriter &rewriter) const override {
    return returningMatchAndRewrite(consumerOp, rewriter);
  }

private:
  SmallVector<int64_t> operandsToFuse;
};

//===----------------------------------------------------------------------===//
// Transformations exposed as patterns, moved from upstream MLIR as IREE still
// heavily relies on patterns that compose through filters.
// TODO: Deprecate all the code below.
//===----------------------------------------------------------------------===//
/// Wrap upstream linalg::splitReduction with a filter.
inline FailureOr<linalg::LinalgOp>
splitReduction(PatternRewriter &b, linalg::LinalgOp op,
               const linalg::ControlSplitReductionFn &controlSplitReductionFn,
               const LinalgTransformationFilter &filter,
               bool useAlloc = false) {
  if (failed(filter.checkAndNotify(b, op)) || !op.hasTensorSemantics() ||
      op.getNumReductionLoops() != 1 || op.getNumDpsInits() != 1 ||
      !op.hasOnlyProjectedPermutations())
    return b.notifyMatchFailure(op, "precondition not met");

  FailureOr<linalg::SplitReductionResult> res =
      linalg::splitReduction(b, op, controlSplitReductionFn, useAlloc);
  if (failed(res))
    return failure();

  filter.replaceLinalgTransformationFilter(b, res->splitLinalgOp);
  filter.replaceLinalgTransformationFilter(b, res->resultCombiningLinalgOp);

  return res->splitLinalgOp;
}

///
/// Linalg tiling pattern.
///
/// Apply the `tiling` transformation as a pattern.
/// `filter` controls LinalgTransformMarker matching and update when specified.
/// See `tiling` for more details.
// TODO: TiledOpInterface
struct LinalgTilingPattern
    : public OpInterfaceRewritePattern<linalg::LinalgOp> {
  /// Construct a generic pattern applied to all LinalgOp that verify `filter`.
  LinalgTilingPattern(
      MLIRContext *context, linalg::LinalgTilingOptions options,
      LinalgTransformationFilter f = LinalgTransformationFilter(),
      PatternBenefit benefit = 1);

  /// Construct a pattern specifically applied to `opName`.
  LinalgTilingPattern(
      StringRef opName, MLIRContext *context,
      linalg::LinalgTilingOptions options,
      LinalgTransformationFilter f = LinalgTransformationFilter(),
      PatternBenefit benefit = 1);

  /// `matchAndRewrite` implementation that returns the significant transformed
  /// pieces of IR.
  FailureOr<linalg::TiledLinalgOp>
  returningMatchAndRewrite(linalg::LinalgOp op,
                           PatternRewriter &rewriter) const;

  LogicalResult matchAndRewrite(linalg::LinalgOp op,
                                PatternRewriter &rewriter) const override {
    return returningMatchAndRewrite(op, rewriter);
  }

private:
  /// LinalgTransformMarker handles special attribute manipulations.
  LinalgTransformationFilter filter;
  /// Options to control tiling;
  linalg::LinalgTilingOptions options;
};

///
/// Linalg SCF tiling pattern.
///
/// Apply the `tiling` transformation as a pattern.
/// `filter` controls LinalgTransformMarker matching and update when specified.
/// See `tiling` for more details.
struct SCFTilingPattern : public OpInterfaceRewritePattern<TilingInterface> {
  /// Construct a generic pattern applied to all LinalgOp that verify `filter`.
  SCFTilingPattern(MLIRContext *context, scf::SCFTilingOptions options,
                   LinalgTransformationFilter f = LinalgTransformationFilter(),
                   PatternBenefit benefit = 1);

  /// Construct a pattern specifically applied to `opName`.
  SCFTilingPattern(StringRef opName, MLIRContext *context,
                   scf::SCFTilingOptions options,
                   LinalgTransformationFilter f = LinalgTransformationFilter(),
                   PatternBenefit benefit = 1);

  /// `matchAndRewrite` implementation that returns the significant transformed
  /// pieces of IR.
  LogicalResult returningMatchAndRewrite(TilingInterface op,
                                         PatternRewriter &rewriter) const;

  LogicalResult matchAndRewrite(TilingInterface op,
                                PatternRewriter &rewriter) const override {
    return returningMatchAndRewrite(op, rewriter);
  }

private:
  /// LinalgTransformMarker handles special attribute manipulations.
  LinalgTransformationFilter filter;
  /// Options to control tiling;
  scf::SCFTilingOptions options;
};

template <typename... OpTypes>
class TilingPatterns;

template <>
class TilingPatterns<> {
public:
  static void insert(RewritePatternSet &patterns,
                     const linalg::LinalgTilingOptions &options,
                     const LinalgTransformationFilter &f) {}
};

template <typename OpTy, typename... OpTypes>
class TilingPatterns<OpTy, OpTypes...> {
public:
  static void insert(RewritePatternSet &patterns,
                     const linalg::LinalgTilingOptions &options,
                     const LinalgTransformationFilter &f) {
    patterns.add<LinalgTilingPattern>(OpTy::getOperationName(),
                                      patterns.getContext(), options, f);
    TilingPatterns<OpTypes...>::insert(patterns, options, f);
  }
};

///
/// Linalg SCF tile and fuse patterns.
///
/// `filter` controls LinalgTransformMarker matching and update when specified.
struct SCFTileAndFusePattern
    : public OpInterfaceRewritePattern<TilingInterface> {
  /// Construct a generic pattern applied to all LinalgOp that verify `filter`.
  SCFTileAndFusePattern(
      MLIRContext *context,
      scf::SCFTilingOptions options = scf::SCFTilingOptions(),
      LinalgTransformationFilter filter = LinalgTransformationFilter(),
      PatternBenefit benefit = 1)
      : OpInterfaceRewritePattern<TilingInterface>(context, benefit),
        options(std::move(options)), filter(std::move(filter)) {}

  /// Construct a pattern specifically applied to `opName`.
  SCFTileAndFusePattern(
      StringRef opName, MLIRContext *context,
      scf::SCFTilingOptions options = scf::SCFTilingOptions(),
      LinalgTransformationFilter filter = LinalgTransformationFilter(),
      PatternBenefit benefit = 1)
      : OpInterfaceRewritePattern<TilingInterface>(context, benefit),
        options(std::move(options)), filter(filter.addOpNameFilter(opName)) {}

  LogicalResult matchAndRewrite(TilingInterface op,
                                PatternRewriter &rewriter) const override;

private:
  scf::SCFTilingOptions options;

  /// LinalgTransformMarker handles special attribute manipulations.
  LinalgTransformationFilter filter;
};

///
/// Linalg vectorization patterns.
///
/// `filter` controls LinalgTransformMarker matching and update when specified.
/// See `vectorizeLinalgOp` for more details.
struct LinalgVectorizationPattern
    : public OpInterfaceRewritePattern<linalg::LinalgOp> {
  /// Construct a generic pattern applied to all LinalgOp that verify `filter`.
  LinalgVectorizationPattern(
      MLIRContext *context,
      LinalgVectorizationOptions opts = LinalgVectorizationOptions(),
      LinalgTransformationFilter f = LinalgTransformationFilter(),
      PatternBenefit benefit = 1);

  /// Construct a pattern specifically applied to `opName`.
  LinalgVectorizationPattern(
      StringRef opName, MLIRContext *context,
      LinalgVectorizationOptions opts = LinalgVectorizationOptions(),
      LinalgTransformationFilter f = LinalgTransformationFilter(),
      PatternBenefit benefit = 1);

  LogicalResult matchAndRewrite(linalg::LinalgOp linalgOp,
                                PatternRewriter &rewriter) const override;

private:
  /// LinalgTransformMarker handles special attribute manipulations.
  LinalgVectorizationOptions options;
  LinalgTransformationFilter filter;
};

template <typename... OpTypes>
class VectorizationPatterns;

template <>
class VectorizationPatterns<> {
public:
  static void insert(RewritePatternSet &patterns,
                     const LinalgVectorizationOptions &opts,
                     const LinalgTransformationFilter &f) {}
};

template <typename OpTy, typename... OpTypes>
class VectorizationPatterns<OpTy, OpTypes...> {
public:
  static void insert(RewritePatternSet &patterns,
                     const LinalgVectorizationOptions &opts,
                     const LinalgTransformationFilter &f) {
    patterns.add<LinalgVectorizationPattern>(OpTy::getOperationName(),
                                             patterns.getContext(), opts, f);
    VectorizationPatterns<OpTypes...>::insert(patterns, opts, f);
  }
};

///
/// Linalg promotion patterns.
///
/// Apply the `promoteSubViews` transformation as a pattern.
/// `filter` controls LinalgTransformMarker matching and update when specified.
/// See `promoteSubViews` for more details.
struct LinalgBasePromotionPattern : public RewritePattern {
  /// Entry point to match any LinalgOp
  /// OpInterface. MatchAnyOpTag-based constructor
  /// with a mandatory `filter`.
  LinalgBasePromotionPattern(
      MLIRContext *context, LinalgTransformationFilter f,
      linalg::LinalgPromotionOptions options = linalg::LinalgPromotionOptions(),
      PatternBenefit benefit = 1)
      : RewritePattern(MatchAnyOpTypeTag(), benefit, context),
        filter(std::move(f)), options(std::move(options)) {}
  /// Entry point to match a specific Linalg op.
  LinalgBasePromotionPattern(
      StringRef opName, MLIRContext *context,
      linalg::LinalgPromotionOptions options,
      LinalgTransformationFilter f = LinalgTransformationFilter(),
      PatternBenefit benefit = 1)
      : RewritePattern(opName, benefit, context, {}), filter(std::move(f)),
        options(std::move(options)) {}

  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    if (failed(filter.checkAndNotify(rewriter, op)))
      return failure();
    if (failed(promoteSubviewsPrecondition(op, options)))
      return failure();

    // TODO: We cannot use root update here. This
    // pattern is creating other ops, so if the
    // promotion fails, those need to be cleaned
    // up, which doesnt seem to be happening here.
    // So to fail properly, we should be cloning
    // the op and deleting the previous op. This
    // needs more investigation.
    rewriter.startRootUpdate(op);
    Optional<linalg::LinalgOp> promotedOp =
        promoteSubViews(rewriter, op, options);
    if (!promotedOp) {
      rewriter.cancelRootUpdate(op);
      return op->emitError("subview promotion failed");
    }
    rewriter.finalizeRootUpdate(op);
    filter.replaceLinalgTransformationFilter(rewriter, op);
    return success();
  }

private:
  /// LinalgTransformMarker handles special
  /// attribute manipulations.
  LinalgTransformationFilter filter;
  /// Promotion options.
  linalg::LinalgPromotionOptions options;
};

template <typename OpTy>
struct LinalgPromotionPattern : public LinalgBasePromotionPattern {
  /// SFINAE: This constructor can only trigger for
  /// concrete ops that have a static
  /// `getOperationName` method.
  template <typename ConcreateOpTy = OpTy>
  LinalgPromotionPattern(
      MLIRContext *context, linalg::LinalgPromotionOptions options,
      LinalgTransformationFilter f = LinalgTransformationFilter(),
      PatternBenefit benefit = 1)
      : LinalgBasePromotionPattern(OpTy::getOperationName(), context, options,
                                   f, benefit) {}
  /// This constructor is available to anyone.
  LinalgPromotionPattern(
      StringRef opName, MLIRContext *context,
      linalg::LinalgPromotionOptions options,
      LinalgTransformationFilter f = LinalgTransformationFilter(),
      PatternBenefit benefit = 1)
      : LinalgBasePromotionPattern(opName, context, options, f, benefit) {}
};

/// Wraps upstream Linalg pattern in a filter check + update.
template <typename Conv2DOp, typename Conv1DOp>
struct DownscaleSizeOneWindowed2DConvolution final
    : public OpRewritePattern<Conv2DOp> {
  DownscaleSizeOneWindowed2DConvolution(MLIRContext *context,
                                        LinalgTransformationFilter f)
      : OpRewritePattern<Conv2DOp>(context, /*benefit=*/1),
        filter(std::move(f)) {}

  LogicalResult matchAndRewrite(Conv2DOp convOp,
                                PatternRewriter &rewriter) const override {
    if (failed(filter.checkAndNotify(rewriter, convOp)))
      return failure();
    linalg::DownscaleSizeOneWindowed2DConvolution<Conv2DOp, Conv1DOp> p(
        convOp.getContext());
    auto maybeConv1DOp = p.returningMatchAndRewrite(convOp, rewriter);
    if (failed(maybeConv1DOp))
      return failure();
    filter.replaceLinalgTransformationFilter(rewriter, *maybeConv1DOp);
    return success();
  }

private:
  /// LinalgTransformMarker handles special attribute manipulations.
  LinalgTransformationFilter filter;
};

/// Wraps upstream Linalg pattern in a filter check + update.
struct DownscaleDepthwiseConv2DNhwcHwcOp final
    : public OpRewritePattern<linalg::DepthwiseConv2DNhwcHwcOp> {
  DownscaleDepthwiseConv2DNhwcHwcOp(MLIRContext *context,
                                    LinalgTransformationFilter f)
      : OpRewritePattern<linalg::DepthwiseConv2DNhwcHwcOp>(context,
                                                           /*benefit=*/1),
        filter(std::move(f)) {}

  LogicalResult matchAndRewrite(linalg::DepthwiseConv2DNhwcHwcOp convOp,
                                PatternRewriter &rewriter) const override {
    if (failed(filter.checkAndNotify(rewriter, convOp)))
      return failure();
    linalg::DownscaleDepthwiseConv2DNhwcHwcOp p(convOp.getContext());
    auto maybeConv1DOp = p.returningMatchAndRewrite(convOp, rewriter);
    if (failed(maybeConv1DOp))
      return failure();
    filter.replaceLinalgTransformationFilter(rewriter, *maybeConv1DOp);
    return success();
  }

private:
  /// LinalgTransformMarker handles special attribute manipulations.
  LinalgTransformationFilter filter;
};

/// Wraps upstream Linalg pattern in a filter check + update.
struct LinalgPaddingPattern
    : public OpInterfaceRewritePattern<linalg::LinalgOp> {
  /// Construct a generic pattern applied to all LinalgOp that verify `filter`.
  LinalgPaddingPattern(
      MLIRContext *context,
      linalg::LinalgPaddingOptions options = linalg::LinalgPaddingOptions(),
      LinalgTransformationFilter f = LinalgTransformationFilter())
      : OpInterfaceRewritePattern<linalg::LinalgOp>(context,
                                                    /*benefit=*/1),
        filter(std::move(f)), options(options) {}

  /// Construct a pattern specifically applied to `opName`.
  LinalgPaddingPattern(
      StringRef opName, MLIRContext *context,
      linalg::LinalgPaddingOptions options = linalg::LinalgPaddingOptions(),
      LinalgTransformationFilter f = LinalgTransformationFilter())
      : OpInterfaceRewritePattern<linalg::LinalgOp>(context, /*benefit=*/1),
        filter(f.addOpNameFilter(opName)), options(std::move(options)) {}

  LogicalResult matchAndRewrite(linalg::LinalgOp op,
                                PatternRewriter &rewriter) const override {
    if (failed(filter.checkAndNotify(rewriter, op)))
      return failure();
    linalg::LinalgPaddingPattern p(op.getContext(), options);
    auto maybeRes = p.returningMatchAndRewrite(op, rewriter);
    if (failed(maybeRes))
      return failure();
    filter.replaceLinalgTransformationFilter(rewriter, *maybeRes);
    return success();
  }

private:
  /// LinalgTransformMarker handles special attribute manipulations.
  LinalgTransformationFilter filter;
  /// Options to control padding and hoisting.
  linalg::LinalgPaddingOptions options;
};

FailureOr<linalg::TileLoopNest> tileConsumerAndFuseProducers(
    OpBuilder &b, linalg::LinalgOp consumerOp, ArrayRef<int64_t> tileSizes,
    ArrayRef<int64_t> tileInterchange,
    const Optional<linalg::LinalgLoopDistributionOptions> &tileDistribution);

} // namespace LinalgExt
} // namespace IREE
} // namespace iree_compiler
} // namespace mlir

#endif // IREE_DIALECTS_DIALECT_LINALGEXT_TRANSFORMS_TRANSFORMS_H_
