// RUN: iree-opt -iree-convert-to-llvm %s | FileCheck %s

builtin.module {
  func.func private @extern_public()
  func.func @entry_point() {
    return
  }
}
//      CHECK: llvm.func @extern_public()
//      CHECK: llvm.func @entry_point(
// CHECK-SAME:     %[[ARG0:[a-zA-Z0-9]+]]: !llvm.ptr<struct<"iree_hal_executable_environment_v0_t", {{[^\{]+}}>> {llvm.align = 16 : i64, llvm.noalias},
// CHECK-SAME:     %[[ARG1:[a-zA-Z0-9]+]]: !llvm.ptr<struct<"iree_hal_executable_dispatch_state_v0_t", {{[^\{]+}}>> {llvm.align = 16 : i64, llvm.noalias},
// CHECK-SAME:     %[[ARG0:[a-zA-Z0-9]+]]: !llvm.ptr<struct<"iree_hal_executable_workgroup_state_v0_t", {{[^\{]+}}>> {llvm.align = 16 : i64, llvm.noalias}) -> i32
//      CHECK:     llvm.return %{{.+}} : i32

