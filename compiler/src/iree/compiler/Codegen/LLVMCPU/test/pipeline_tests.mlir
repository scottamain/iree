// RUN: iree-opt --pass-pipeline='builtin.module(hal.executable(hal.executable.variant(iree-llvmcpu-lower-executable-target)))' --split-input-file %s | FileCheck %s
// RUN: iree-opt --pass-pipeline='builtin.module(hal.executable(hal.executable.variant(iree-llvmcpu-lower-executable-target)))' --iree-llvmcpu-enable-hoist-padding --split-input-file %s | FileCheck %s --check-prefix=HOIST-PAD

// Check that this dispatch compiles to vectors and that there are no allocas.
// By proxy checks that destination passing style kicked in correctly
// and no CSE was run between first level tile + fuse + distribute
// and the conversion to destination passing style. Running CSE
// before hoists the fill and the init_tensor out of the loop causing
// issues with the conversion.
#map3 = affine_map<(d0) -> (d0)>
#map4 = affine_map<(d0, d1) -> (d0)>
#map5 = affine_map<(d0, d1) -> (d0, d1)>
#executable_target_embedded_elf_x86_64_ = #hal.executable.target<"llvm-cpu", "embedded-elf-x86_64", {
  cpu_features = "",
  data_layout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128",
  native_vector_size = 16 : index,
  target_triple = "x86_64-unknown-unknown-eabi-elf"}>
#pipeline_layout5 = #hal.pipeline.layout<push_constants = 2, sets = [
  #hal.descriptor_set.layout<0, bindings = [
    #hal.descriptor_set.binding<0, storage_buffer>,
    #hal.descriptor_set.binding<1, storage_buffer>]
  >]>
hal.executable private @check_no_cse {
  hal.executable.variant public @embedded_elf_x86_64, target = #executable_target_embedded_elf_x86_64_ {
    hal.executable.export public @check_no_cse ordinal(0) layout(#pipeline_layout5) {
    ^bb0(%arg0: !hal.device, %arg1: index):
      %x, %y, %z = flow.dispatch.workgroup_count_from_dag_root %arg1
      hal.return %x, %y, %z : index, index, index
    }
    builtin.module {
      func.func @check_no_cse() {
        %cst = arith.constant 3.840000e+02 : f32
        %cst_0 = arith.constant 0.000000e+00 : f32
        %0 = hal.interface.constant.load[0] : i32
        %1 = hal.interface.constant.load[1] : i32
        %2 = arith.index_cast %0 {stream.alignment = 512 : index, stream.values = [0 : index, 10752 : index]} : i32 to index
        %3 = arith.index_cast %1 {stream.alignment = 512 : index, stream.values = [10752 : index, 21504 : index]} : i32 to index
        %4 = hal.interface.binding.subspan set(0) binding(0) type(storage_buffer) alignment(64) offset(%2) : !flow.dispatch.tensor<readonly:tensor<7x384xf32>>
        %5 = hal.interface.binding.subspan set(0) binding(1) type(storage_buffer) alignment(64) offset(%3) : !flow.dispatch.tensor<writeonly:tensor<7xf32>>
        %6 = flow.dispatch.tensor.load %4, offsets = [0, 0], sizes = [7, 384], strides = [1, 1] : !flow.dispatch.tensor<readonly:tensor<7x384xf32>> -> tensor<7x384xf32>
        %7 = tensor.empty() : tensor<7xf32>
        %8 = linalg.fill ins(%cst_0 : f32) outs(%7 : tensor<7xf32>) -> tensor<7xf32>
        %9 = linalg.generic {indexing_maps = [#map5, #map4], iterator_types = ["parallel", "reduction"]} ins(%6 : tensor<7x384xf32>) outs(%8 : tensor<7xf32>) {
        ^bb0(%arg0: f32, %arg1: f32):
          %11 = arith.addf %arg1, %arg0 : f32
          linalg.yield %11 : f32
        } -> tensor<7xf32>
        %10 = linalg.generic {indexing_maps = [#map3, #map3], iterator_types = ["parallel"]} ins(%9 : tensor<7xf32>) outs(%7 : tensor<7xf32>) {
        ^bb0(%arg0: f32, %arg1: f32):
          %11 = arith.divf %arg0, %cst : f32
          linalg.yield %11 : f32
        } -> tensor<7xf32>
        flow.dispatch.tensor.store %10, %5, offsets = [0], sizes = [7], strides = [1] : tensor<7xf32> -> !flow.dispatch.tensor<writeonly:tensor<7xf32>>
        return
      }
    }
  }
}
//      CHECK: func.func @check_no_cse()
//  CHECK-NOT:    memref.alloc
//      CHECK:    %[[FOR:.+]] = scf.for
//      CHECK:    %[[DIVF:.+]] = arith.divf %[[FOR]]
//      CHECK:    %[[RES:.+]] = vector.extract %[[DIVF]]
//      CHECK:    memref.store %[[RES]]

// -----

// Checks that the ops are padded and vectorized. The test sets tiling sizes to
// be non-divisible by problem sizes. If padding and vectorizing are kicked in,
// vector ops will be generated.
#compilation = #iree_codegen.compilation_info<
    lowering_config = <tile_sizes = [[65, 65], [8, 32, 0], [0, 0, 16]]>,
    translation_info  = <CPUDoubleTilingPadExpert>,
    workgroup_size = []>
#pipeline_layout = #hal.pipeline.layout<push_constants = 0, sets = [
  #hal.descriptor_set.layout<0, bindings = [
    #hal.descriptor_set.binding<0, storage_buffer>,
    #hal.descriptor_set.binding<1, storage_buffer>,
    #hal.descriptor_set.binding<2, storage_buffer>
  ]>
]>
hal.executable private @preset_config_matmul  {
  hal.executable.variant @system_elf_x86_64, target = <"llvm-cpu", "system-elf-x86_64"> {
    hal.executable.export @preset_config_matmul layout(#pipeline_layout) {
    ^bb0(%arg0: !hal.device, %arg1: index, %arg2 : index, %arg3 : index):
      %x, %y, %z = flow.dispatch.workgroup_count_from_dag_root %arg1, %arg2, %arg3
      hal.return %x, %y, %z : index, index, index
    }
    builtin.module {
      func.func @preset_config_matmul() {
        %cst = arith.constant 0.000000e+00 : f32
        %c0 = arith.constant 0 : index
        %0 = hal.interface.binding.subspan set(0) binding(0) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<readonly:tensor<128x49xf32>>
        %1 = hal.interface.binding.subspan set(0) binding(1) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<readonly:tensor<49x512xf32>>
        %2 = hal.interface.binding.subspan set(0) binding(2) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<writeonly:tensor<128x512xf32>>
        %3 = flow.dispatch.tensor.load %0, offsets = [0, 0], sizes = [128, 49], strides = [1, 1] : !flow.dispatch.tensor<readonly:tensor<128x49xf32>> -> tensor<128x49xf32>
        %4 = flow.dispatch.tensor.load %1, offsets = [0, 0], sizes = [49, 512], strides = [1, 1] : !flow.dispatch.tensor<readonly:tensor<49x512xf32>> -> tensor<49x512xf32>
        %5 = tensor.empty() : tensor<128x512xf32>
        %6 = linalg.fill ins(%cst : f32) outs(%5 : tensor<128x512xf32>) -> tensor<128x512xf32>
        %7 = linalg.matmul {compilation_info = #compilation}
          ins(%3, %4 : tensor<128x49xf32>, tensor<49x512xf32>)
          outs(%6 : tensor<128x512xf32>) -> tensor<128x512xf32>
        flow.dispatch.tensor.store %7, %2, offsets = [0, 0], sizes = [128, 512], strides = [1, 1] : tensor<128x512xf32> -> !flow.dispatch.tensor<writeonly:tensor<128x512xf32>>
        return
      }
    }
  }
}
// CHECK: func.func @preset_config_matmul
// CHECK:   vector.outerproduct
// HOIST-PAD:         func.func @preset_config_matmul
// HOIST-PAD-DAG:       %[[BUF1:.+]] = memref.alloca() {{.+}} : memref<3x4x16x32xf32>
// HOIST-PAD-DAG:       %[[BUF2:.+]] = memref.alloca() {{.+}} : memref<4x8x16xf32>
// HOIST-PAD-16-DAG:      vector.store {{.+}}, %[[BUF1]]
// HOIST-PAD-8-DAG:       vector.store {{.+}}, %[[BUF2]]
// HOIST-PAD-16-DAG:      vector.load %[[BUF1]]
// HOIST-PAD-8-DAG:       vector.load %[[BUF2]]
// HOIST-PAD:             vector.outerproduct

// -----

#executable_target_embedded_elf_x86_64_ = #hal.executable.target<"llvm-cpu", "embedded-elf-x86_64", {
  cpu_features = "",
  data_layout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128",
  native_vector_size = 16 : index,
  target_triple = "x86_64-unknown-unknown-eabi-elf"}>
#pipeline_layout = #hal.pipeline.layout<push_constants = 6, sets = [
  #hal.descriptor_set.layout<0, bindings = [
    #hal.descriptor_set.binding<0, storage_buffer>,
    #hal.descriptor_set.binding<1, storage_buffer>,
    #hal.descriptor_set.binding<2, storage_buffer>]
  >]>
hal.executable private @batch_matmul_dynamic {
  hal.executable.variant public @embedded_elf_x86_64, target = #executable_target_embedded_elf_x86_64_ {
    hal.executable.export public @batch_matmul_dynamic ordinal(0) layout(#pipeline_layout) {
    ^bb0(%arg0: !hal.device, %arg1: index, %arg2 : index, %arg3 : index, %arg4 : index):
      %x, %y, %z = flow.dispatch.workgroup_count_from_dag_root %arg1, %arg2, %arg3, %arg4
      hal.return %x, %y, %z : index, index, index
    }
    builtin.module {
      func.func @batch_matmul_dynamic() {
        %cst = arith.constant 0.000000e+00 : f32
        %c0 = arith.constant 0 : index
        %0 = hal.interface.constant.load[0] : i32
        %1 = hal.interface.constant.load[1] : i32
        %2 = hal.interface.constant.load[2] : i32
        %3 = hal.interface.constant.load[3] : i32
        %4 = hal.interface.constant.load[4] : i32
        %5 = hal.interface.constant.load[5] : i32
        %6 = arith.index_cast %0 : i32 to index
        %7 = arith.index_cast %1 : i32 to index
        %8 = arith.index_cast %2 : i32 to index
        %9 = arith.index_cast %3 : i32 to index
        %10 = arith.index_cast %4 : i32 to index
        %11 = arith.index_cast %5 : i32 to index
        %12 = hal.interface.binding.subspan set(0) binding(0) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<readonly:tensor<?x?x?xf32>>{%6, %7, %9}
        %13 = hal.interface.binding.subspan set(0) binding(1) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<readonly:tensor<?x?x?xf32>>{%10, %11, %8}
        %14 = hal.interface.binding.subspan set(0) binding(2) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<writeonly:tensor<?x?x?xf32>>{%6, %7, %8}
        %15 = flow.dispatch.tensor.load %12, offsets = [0, 0, 0], sizes = [%6, %7, %9], strides = [1, 1, 1] : !flow.dispatch.tensor<readonly:tensor<?x?x?xf32>>{%6, %7, %9} -> tensor<?x?x?xf32>
        %16 = flow.dispatch.tensor.load %13, offsets = [0, 0, 0], sizes = [%10, %11, %8], strides = [1, 1, 1] : !flow.dispatch.tensor<readonly:tensor<?x?x?xf32>>{%10, %11, %8} -> tensor<?x?x?xf32>
        %17 = tensor.empty(%6, %7, %8) : tensor<?x?x?xf32>
        %18 = linalg.fill ins(%cst : f32) outs(%17 : tensor<?x?x?xf32>) -> tensor<?x?x?xf32>
        %19 = linalg.batch_matmul ins(%15, %16 : tensor<?x?x?xf32>, tensor<?x?x?xf32>) outs(%18 : tensor<?x?x?xf32>) -> tensor<?x?x?xf32>
        flow.dispatch.tensor.store %19, %14, offsets = [0, 0, 0], sizes = [%6, %7, %8], strides = [1, 1, 1] : tensor<?x?x?xf32> -> !flow.dispatch.tensor<writeonly:tensor<?x?x?xf32>>{%6, %7, %8}
        return
      }
    }
  }
}
// CHECK: func.func @batch_matmul_dynamic
// CHECK:   vector.outerproduct

// -----

#executable_target_embedded_elf_x86_64_ = #hal.executable.target<"llvm-cpu", "embedded-elf-x86_64", {
  cpu_features = "",
  data_layout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128",
  native_vector_size = 16 : index,
  target_triple = "x86_64-unknown-unknown-eabi-elf"}>
#pipeline_layout = #hal.pipeline.layout<push_constants = 2, sets = [
  #hal.descriptor_set.layout<0, bindings = [
    #hal.descriptor_set.binding<0, storage_buffer>,
    #hal.descriptor_set.binding<1, storage_buffer>]
  >]>
hal.executable private @check_buffer_ops_vectorization {
  hal.executable.variant public @embedded_elf_x86_64, target = #executable_target_embedded_elf_x86_64_ {
    hal.executable.export public @check_buffer_ops_vectorization ordinal(0) layout(#pipeline_layout) {
    ^bb0(%arg0: !hal.device, %arg1: index, %arg2 : index):
      %x, %y, %z = flow.dispatch.workgroup_count_from_dag_root %arg1, %arg2
      hal.return %x, %y, %z : index, index, index
    }
    builtin.module {
      func.func @check_buffer_ops_vectorization() {
        %c0 = arith.constant 0 : index
        %0 = hal.interface.binding.subspan set(0) binding(0) type(storage_buffer) alignment(64) offset(%c0) : memref<128x1024xi32>
        memref.assume_alignment %0, 64 : memref<128x1024xi32>
        %1 = hal.interface.binding.subspan set(0) binding(1) type(storage_buffer) alignment(64) offset(%c0) : memref<128x1536xi32>
        memref.assume_alignment %1, 64 : memref<128x1536xi32>
        %2 = memref.subview %1[0, 0] [128, 1024] [1, 1] : memref<128x1536xi32> to memref<128x1024xi32, affine_map<(d0, d1) -> (d0 * 1536 + d1)>>
        linalg.generic {indexing_maps = [affine_map<(d0, d1) -> (d0, d1)>, affine_map<(d0, d1) -> (d0, d1)>], iterator_types = ["parallel", "parallel"]}
          ins(%0 : memref<128x1024xi32>)
          outs(%2 : memref<128x1024xi32, affine_map<(d0, d1) -> (d0 * 1536 + d1)>>) {
        ^bb0(%arg0: i32, %arg1: i32):
          linalg.yield %arg0 : i32
        }
        return
      }
    }
  }
}
// CHECK:      #{{.+}} = #iree_codegen.translation_info<CPUBufferOpsTileAndVectorize
// CHECK:      func.func @check_buffer_ops_vectorization
// CHECK:        vector.load
// CHECK-NEXT:   vector.store

// -----

hal.executable private @vectorize_fill_conv2d_generic {
  hal.executable.variant public @embedded_elf_x86_64, target = #hal.executable.target<
    "llvm-cpu", "embedded-elf-x86_64", {
      cpu_features = "",
      data_layout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128",
      native_vector_size = 16 : index,
      target_triple = "x86_64-unknown-unknown-eabi-elf"
    }> {
    hal.executable.export public @vectorize_fill_conv2d_generic ordinal(0) layout(
      #hal.pipeline.layout<push_constants = 0, sets = [
        #hal.descriptor_set.layout<0, bindings = [
          #hal.descriptor_set.binding<0, storage_buffer>,
          #hal.descriptor_set.binding<1, storage_buffer>,
          #hal.descriptor_set.binding<2, storage_buffer>]
        >]>
      ) {
    ^bb0(%arg0: !hal.device, %arg1: index, %arg2 : index, %arg3 : index, %arg4 : index):
      %x, %y, %z = flow.dispatch.workgroup_count_from_dag_root %arg1, %arg2, %arg3, %arg4
      hal.return %x, %y, %z : index, index, index
    }
    builtin.module {
      func.func @vectorize_fill_conv2d_generic() {
        %cst = arith.constant 0.000000e+00 : f32
        %cst_0 = arith.constant 3.000000e+00 : f32
        %cst_1 = arith.constant 6.000000e+00 : f32
        %cst_2 = arith.constant 0.166666672 : f32
        %cst_3 = arith.constant dense<0.0> : tensor<16xf32>
        %c0 = arith.constant 0 : index
        %0 = hal.interface.binding.subspan set(0) binding(0) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<readonly:tensor<1x225x225x3xf32>>
        %1 = hal.interface.binding.subspan set(0) binding(1) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<readonly:tensor<3x3x3x16xf32>>
        %2 = hal.interface.binding.subspan set(0) binding(2) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<writeonly:tensor<1x112x112x16xf32>>
        %3 = flow.dispatch.tensor.load %0, offsets = [0, 0, 0, 0], sizes = [1, 225, 225, 3], strides = [1, 1, 1, 1] : !flow.dispatch.tensor<readonly:tensor<1x225x225x3xf32>> -> tensor<1x225x225x3xf32>
        %4 = flow.dispatch.tensor.load %1, offsets = [0, 0, 0, 0], sizes = [3, 3, 3, 16], strides = [1, 1, 1, 1] : !flow.dispatch.tensor<readonly:tensor<3x3x3x16xf32>> -> tensor<3x3x3x16xf32>
        %5 = tensor.empty() : tensor<1x112x112x16xf32>
        %6 = linalg.fill ins(%cst : f32) outs(%5 : tensor<1x112x112x16xf32>) -> tensor<1x112x112x16xf32>
        %7 = linalg.conv_2d_nhwc_hwcf {dilations = dense<1> : tensor<2xi64>, strides = dense<2> : tensor<2xi64>} ins(%3, %4 : tensor<1x225x225x3xf32>, tensor<3x3x3x16xf32>) outs(%6 : tensor<1x112x112x16xf32>) -> tensor<1x112x112x16xf32>
        %8 = linalg.generic {
          indexing_maps = [
            affine_map<(d0, d1, d2, d3) -> (d3)>,
            affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>,
            affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>],
            iterator_types = ["parallel", "parallel", "parallel", "parallel"
          ]} ins(%cst_3, %7 : tensor<16xf32>, tensor<1x112x112x16xf32>) outs(%5 : tensor<1x112x112x16xf32>) {
        ^bb0(%arg0: f32, %arg1: f32, %arg2: f32):
          %9 = arith.addf %arg0, %arg1 : f32
          %10 = arith.addf %9, %cst_0 : f32
          %11 = arith.cmpf olt, %10, %cst : f32
          %12 = arith.select %11, %cst, %10 : f32
          %13 = arith.cmpf olt, %cst_1, %10 : f32
          %14 = arith.select %13, %cst_1, %12 : f32
          %15 = arith.mulf %9, %14 : f32
          %16 = arith.mulf %15, %cst_2 : f32
          linalg.yield %16 : f32
        } -> tensor<1x112x112x16xf32>
        flow.dispatch.tensor.store %8, %2, offsets = [0, 0, 0, 0], sizes = [1, 112, 112, 16], strides = [1, 1, 1, 1] : tensor<1x112x112x16xf32> -> !flow.dispatch.tensor<writeonly:tensor<1x112x112x16xf32>>
        return
      }
    }
  }
}

// CHECK:      func.func @vectorize_fill_conv2d_generic
// CHECK-NOT:    memref.alloca
// CHECK-NOT:    linalg.fill
// CHECK:        vector.outerproduct %{{.+}}, %{{.+}}, %{{.+}} {kind = #vector.kind<add>}
// CHECK-NOT:    linalg.generic
// CHECK:        arith.cmpf olt, %{{.+}}, %{{.+}} : vector<4x8xf32>

// -----

hal.executable private @multi_result {
  hal.executable.variant public @embedded_elf_x86_64, target = <"llvm-cpu", "embedded-elf-x86_64", {cpu = "generic", cpu_features = "", data_layout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128", native_vector_size = 16 : index, target_triple = "x86_64-unknown-unknown-eabi-elf"}> {
    hal.executable.export public @multi_result ordinal(0) layout(#hal.pipeline.layout<push_constants = 0, sets = [<0, bindings = [<0, storage_buffer, ReadOnly>, <1, storage_buffer, ReadOnly>, <2, storage_buffer>]>]>) {
    ^bb0(%arg0: !hal.device, %arg1: index, %arg2: index):
      %x, %y, %z = flow.dispatch.workgroup_count_from_dag_root %arg1, %arg2
      hal.return %x, %y, %z : index, index, index
    }
    builtin.module {
      func.func @multi_result() {
        %c0 = arith.constant 0 : index
        %cst = arith.constant 0.000000e+00 : f32
        %cst_0 = arith.constant 1.000000e-03 : f32
        %0 = hal.interface.binding.subspan set(0) binding(0) type(storage_buffer) alignment(64) offset(%c0) flags(ReadOnly) : !flow.dispatch.tensor<readonly:tensor<64x128xf32>>
        %1 = hal.interface.binding.subspan set(0) binding(1) type(storage_buffer) alignment(64) offset(%c0) flags(ReadOnly) : !flow.dispatch.tensor<readonly:tensor<128x256xf32>>
        %2 = hal.interface.binding.subspan set(0) binding(2) type(storage_buffer) alignment(64) offset(%c0) flags(ReadOnly) : !flow.dispatch.tensor<readonly:tensor<256xf32>>
        %3 = hal.interface.binding.subspan set(0) binding(3) type(storage_buffer) alignment(64) offset(%c0) flags(ReadOnly) : !flow.dispatch.tensor<writeonly:tensor<128x256xf32>>
        %5 = hal.interface.binding.subspan set(0) binding(4) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<writeonly:tensor<64x256xf32>>
        %6 = hal.interface.binding.subspan set(0) binding(5) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<writeonly:tensor<64x256xf32>>
        %7 = flow.dispatch.tensor.load %0, offsets = [0, 0], sizes = [64, 128], strides = [1, 1] : !flow.dispatch.tensor<readonly:tensor<64x128xf32>> -> tensor<64x128xf32>
        %8 = flow.dispatch.tensor.load %1, offsets = [0, 0], sizes = [128, 256], strides = [1, 1] : !flow.dispatch.tensor<readonly:tensor<128x256xf32>> -> tensor<128x256xf32>
        %9 = flow.dispatch.tensor.load %2, offsets = [0], sizes = [256], strides = [1] : !flow.dispatch.tensor<readonly:tensor<256xf32>> -> tensor<256xf32>
        %13 = tensor.empty() : tensor<64x256xf32>
        %14 = linalg.fill ins(%cst : f32) outs(%13 : tensor<64x256xf32>) -> tensor<64x256xf32>
        %15 = linalg.matmul ins(%7, %8 : tensor<64x128xf32>, tensor<128x256xf32>) outs(%14 : tensor<64x256xf32>) -> tensor<64x256xf32>
        %16 = linalg.generic {
            indexing_maps = [affine_map<(d0, d1) -> (d0, d1)>, affine_map<(d0, d1) -> (d1)>, affine_map<(d0, d1) -> (d0, d1)>],
            iterator_types = ["parallel", "parallel"]}
          ins(%15, %9 : tensor<64x256xf32>, tensor<256xf32>) outs(%13 : tensor<64x256xf32>) {
        ^bb0(%in: f32, %in_1: f32, %out: f32):
          %17 = arith.addf %in, %in_1 : f32
          linalg.yield %17 : f32
        } -> tensor<64x256xf32>
        flow.dispatch.tensor.store %15, %5, offsets = [0, 0], sizes = [64, 256], strides = [1, 1] : tensor<64x256xf32> -> !flow.dispatch.tensor<writeonly:tensor<64x256xf32>>
        flow.dispatch.tensor.store %16, %6, offsets = [0, 0], sizes = [64, 256], strides = [1, 1] : tensor<64x256xf32> -> !flow.dispatch.tensor<writeonly:tensor<64x256xf32>>
        return
      }
    }
  }
}
//    CHECK-LABEL: func @multi_result
//          CHECK:   scf.for
//          CHECK:     scf.for
//          CHECK:       scf.for
// CHECK-COUNT-16:         vector.outerproduct
//          CHECK:       arith.addf %{{.+}}, %{{.+}} : vector<8x32xf32>
