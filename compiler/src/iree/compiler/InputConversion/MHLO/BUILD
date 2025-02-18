# Copyright 2021 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

load("//build_tools/bazel:build_defs.oss.bzl", "iree_compiler_cc_library")
load("//build_tools/bazel:iree_tablegen.bzl", "iree_gentbl_cc_library")

package(
    default_visibility = ["//visibility:public"],
    features = ["layering_check"],
    licenses = ["notice"],  # Apache 2.0
)

iree_gentbl_cc_library(
    name = "PassesIncGen",
    tbl_outs = [
        (
            ["--gen-pass-decls"],
            "Passes.h.inc",
        ),
    ],
    tblgen = "@llvm-project//mlir:mlir-tblgen",
    td_file = "Passes.td",
    deps = [
        "@llvm-project//mlir:PassBaseTdFiles",
    ],
)

iree_compiler_cc_library(
    name = "PassHeaders",
    hdrs = [
        "PassDetail.h",
        "Passes.h",
        "Passes.h.inc",
        "Rewriters.h",
    ],
    deps = [
        ":PassesIncGen",
        "@llvm-project//mlir:Pass",
        "@llvm-project//mlir:Transforms",
    ],
)

iree_compiler_cc_library(
    name = "MHLO",
    srcs = [
        "BroadcastingToLinalgPatterns.cpp",
        "ConvertComplexToReal.cpp",
        "ConvertMHLOToFlow.cpp",
        "ConvertMHLOToFlow.h",
        "ConvertMHLOToLinalgExt.cpp",
        "FlattenTuplesInCFG.cpp",
        "MHLOToLinalgOnTensors.cpp",
        "MHLOToMHLOPreprocessing.cpp",
        "Passes.cpp",
        "VerifyCompilerMHLOInputLegality.cpp",
    ],
    hdrs = [
        "Passes.h",
    ],
    defines = [
        "IREE_HAVE_MHLO_INPUT",
    ],
    deps = [
        ":PassHeaders",
        ":PassesIncGen",
        "//compiler/src/iree/compiler/Dialect/Flow/IR",
        "//compiler/src/iree/compiler/Dialect/Util/IR",
        "//compiler/src/iree/compiler/Dialect/Util/Transforms",
        "//compiler/src/iree/compiler/InputConversion/Common",
        "//compiler/src/iree/compiler/Utils",
        "//llvm-external-projects/iree-dialects:IREELinalgExtDialect",
        "//llvm-external-projects/iree-dialects:IREELinalgExtPasses",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:AffineDialect",
        "@llvm-project//mlir:AffineUtils",
        "@llvm-project//mlir:ArithDialect",
        "@llvm-project//mlir:ComplexDialect",
        "@llvm-project//mlir:ControlFlowDialect",
        "@llvm-project//mlir:DialectUtils",
        "@llvm-project//mlir:FuncDialect",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:LinalgDialect",
        "@llvm-project//mlir:LinalgTransforms",
        "@llvm-project//mlir:MLProgramDialect",
        "@llvm-project//mlir:MathDialect",
        "@llvm-project//mlir:MemRefDialect",
        "@llvm-project//mlir:Pass",
        "@llvm-project//mlir:ReconcileUnrealizedCasts",
        "@llvm-project//mlir:SCFToControlFlow",
        "@llvm-project//mlir:SCFTransforms",
        "@llvm-project//mlir:ShapeDialect",
        "@llvm-project//mlir:ShapeToStandard",
        "@llvm-project//mlir:ShapeTransforms",
        "@llvm-project//mlir:Support",
        "@llvm-project//mlir:TensorDialect",
        "@llvm-project//mlir:Transforms",
        "@mlir-hlo//:chlo_legalize_to_hlo",
        "@mlir-hlo//:map_chlo_to_hlo_op",
        "@mlir-hlo//:map_lmhlo_to_scalar_op",
        "@mlir-hlo//:map_mhlo_to_scalar_op",
        "@mlir-hlo//:mhlo_passes",
        "@mlir-hlo//:mlir_hlo",
        "@mlir-hlo//:unfuse_batch_norm",
        "@mlir-hlo//stablehlo:broadcast_utils",
        "@mlir-hlo//stablehlo:chlo_ops",
    ],
)
