// RUN: iree-opt %s -iree-transform-dialect-interpreter -transform-dialect-drop-schedule | FileCheck %s

hal.executable private @matmul  {
builtin.module {
// CHECK-LABEL: func.func @matmul
func.func @matmul() {
  %c8 = arith.constant 8 : index
  %c0 = arith.constant 0 : index
  %cst = arith.constant dense<0.000000e+00> : vector<16x16xf32>
  %c16 = arith.constant 16 : index
  %c32 = arith.constant 32 : index
  %cst_0 = arith.constant 0.000000e+00 : f32
  %0 = hal.interface.binding.subspan set(0) binding(0) type(storage_buffer) alignment(64) offset(%c0) : memref<32x32xf32>
  memref.assume_alignment %0, 64 : memref<32x32xf32>
  %1 = hal.interface.binding.subspan set(0) binding(1) type(storage_buffer) alignment(64) offset(%c0) : memref<32x32xf32>
  memref.assume_alignment %1, 64 : memref<32x32xf32>
  %2 = hal.interface.binding.subspan set(0) binding(2) type(storage_buffer) alignment(64) offset(%c0) : memref<32x32xf32>
  memref.assume_alignment %2, 64 : memref<32x32xf32>
  %3 = gpu.thread_id  x
  %4 = gpu.thread_id  y
  %5 = affine.apply affine_map<()[s0] -> (s0 * 16)>()[%4]
  %6 = affine.apply affine_map<()[s0] -> ((s0 floordiv 32) * 16)>()[%3]
// CHECK: gpu.subgroup_mma_constant_matrix %{{.*}} : !gpu.mma_matrix<16x16xf32, "COp">
// CHECK: scf.for {{.*}} -> (!gpu.mma_matrix<16x16xf32, "COp">) {
// CHECK:   gpu.subgroup_mma_load_matrix {{.*}} {leadDimension = 32 : index} : memref<32x32xf32> -> !gpu.mma_matrix<16x8xf32, "AOp">
// CHECK:   gpu.subgroup_mma_load_matrix {{.*}} {leadDimension = 32 : index} : memref<32x32xf32> -> !gpu.mma_matrix<16x8xf32, "AOp">
// CHECK:   gpu.subgroup_mma_load_matrix {{.*}} {leadDimension = 32 : index} : memref<32x32xf32> -> !gpu.mma_matrix<8x16xf32, "BOp">
// CHECK:   gpu.subgroup_mma_load_matrix {{.*}} {leadDimension = 32 : index} : memref<32x32xf32> -> !gpu.mma_matrix<8x16xf32, "BOp">
// CHECK:   gpu.subgroup_mma_compute {{.*}} : !gpu.mma_matrix<16x8xf32, "AOp">, !gpu.mma_matrix<8x16xf32, "BOp"> -> !gpu.mma_matrix<16x16xf32, "COp">
// CHECK:   gpu.subgroup_mma_compute {{.*}} : !gpu.mma_matrix<16x8xf32, "AOp">, !gpu.mma_matrix<8x16xf32, "BOp"> -> !gpu.mma_matrix<16x16xf32, "COp">
// CHECK:   scf.yield {{.*}} : !gpu.mma_matrix<16x16xf32, "COp">
// CHECK: }
// CHECK: gpu.subgroup_mma_store_matrix {{.*}} {leadDimension = 32 : index} : !gpu.mma_matrix<16x16xf32, "COp">, memref<32x32xf32>
  %7 = scf.for %arg0 = %c0 to %c32 step %c16 iter_args(%arg1 = %cst) -> (vector<16x16xf32>) {
    %10 = affine.apply affine_map<(d0)[s0] -> (d0 + s0)>(%c0)[%5]
    %11 = affine.apply affine_map<(d0)[s0] -> (d0 + s0)>(%c0)[%arg0]
    %12 = vector.transfer_read %0[%10, %11], %cst_0 {in_bounds = [true, true]} : memref<32x32xf32>, vector<16x16xf32>
    %16 = affine.apply affine_map<(d0)[s0] -> (d0 + s0)>(%c0)[%6]
    %17 = affine.apply affine_map<(d0)[s0] -> (d0 + s0)>(%c0)[%arg0]
    %18 = vector.transfer_read %1[%17, %16], %cst_0 {in_bounds = [true, true]} : memref<32x32xf32>, vector<16x16xf32>
    %22 = vector.contract {indexing_maps = [affine_map<(d0, d1, d2) -> (d0, d2)>, affine_map<(d0, d1, d2) -> (d2, d1)>, affine_map<(d0, d1, d2) -> (d0, d1)>], iterator_types = ["parallel", "parallel", "reduction"], kind = #vector.kind<add>} %12, %18, %arg1 : vector<16x16xf32>, vector<16x16xf32> into vector<16x16xf32>
    scf.yield %22 : vector<16x16xf32>
  }
  %8 = affine.apply affine_map<(d0)[s0] -> (d0 + s0)>(%c0)[%5]
  %9 = affine.apply affine_map<(d0)[s0] -> (d0 + s0)>(%c0)[%6]
  vector.transfer_write %7, %2[%8, %9] {in_bounds = [true, true]} : vector<16x16xf32>, memref<32x32xf32>
  return
}
}
transform.structured.canonicalized_sequence failures(propagate) {
^bb1(%variant_op: !pdl.operation):
  %func = transform.structured.match ops{["func.func"]} in %variant_op : (!pdl.operation) -> !pdl.operation
  transform.iree.vector.vector_to_mma_conversion %func
}
}
