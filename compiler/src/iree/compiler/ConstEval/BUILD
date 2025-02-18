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
    ],
    deps = [
        ":PassesIncGen",
        "@llvm-project//mlir:Pass",
        "@llvm-project//mlir:Transforms",
    ],
)

iree_compiler_cc_library(
    name = "ConstEval",
    srcs = [
        "JitGlobals.cpp",
        "Passes.cpp",
    ],
    hdrs = [
        "Passes.h",
    ],
    deps = [
        ":PassHeaders",
        ":PassesIncGen",
        ":Runtime",
        "//compiler/src/iree/compiler/Pipelines",
        "//compiler/src/iree/compiler/Utils",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:FuncDialect",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Pass",
    ],
)

iree_compiler_cc_library(
    name = "Runtime",
    srcs = [
        "Runtime.cpp",
    ],
    hdrs = [
        "Runtime.h",
    ],
    deps = [
        "//compiler/src/iree/compiler/Dialect/VM/Target/Bytecode",
        "//runtime/src/iree/hal",
        "//runtime/src/iree/hal/drivers/local_task/registration",
        "//runtime/src/iree/modules/hal",
        "//runtime/src/iree/tooling:vm_util",
        "//runtime/src/iree/vm",
        "//runtime/src/iree/vm:bytecode_module",
        "@llvm-project//mlir:IR",
    ],
)
