// RUN: iree-opt %s -iree-transform-dialect-interpreter -transform-dialect-drop-schedule | FileCheck %s

hal.executable private @pad_matmul_static_dispatch_0  {
  builtin.module {
    func.func @pad_matmul_static_dispatch_0(%arg0: tensor<250x500xf32>, %arg1: tensor<500x1020xf32>) -> tensor<250x1020xf32> {
      %c0 = arith.constant 0 : index
      %0 = hal.interface.binding.subspan set(0) binding(0) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<readonly:tensor<250x500xf32>>
      %1 = hal.interface.binding.subspan set(0) binding(1) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<readonly:tensor<500x1020xf32>>
      %2 = hal.interface.binding.subspan set(0) binding(2) type(storage_buffer) alignment(64) offset(%c0) : !flow.dispatch.tensor<readwrite:tensor<250x1020xf32>>
      %3 = flow.dispatch.tensor.load %0, offsets = [0, 0], sizes = [250, 500], strides = [1, 1] : !flow.dispatch.tensor<readonly:tensor<250x500xf32>> -> tensor<250x500xf32>
      %4 = flow.dispatch.tensor.load %1, offsets = [0, 0], sizes = [500, 1020], strides = [1, 1] : !flow.dispatch.tensor<readonly:tensor<500x1020xf32>> -> tensor<500x1020xf32>

      %50 = tensor.empty() : tensor<250x1020xf32>
      %cst = arith.constant 0.000000e+00 : f32
      %5 = linalg.fill ins(%cst : f32) outs(%50 : tensor<250x1020xf32>) -> tensor<250x1020xf32>
      // CHECK:      %[[CST:.+]] = arith.constant 0.000000e+00 : f32
      // CHECK:      %[[D0:.+]] = hal.interface.binding.subspan set(0) binding(0) type(storage_buffer) alignment(64)
      // CHECK:      %[[D1:.+]] = hal.interface.binding.subspan set(0) binding(1) type(storage_buffer) alignment(64)
      // CHECK:      %[[D2:.+]] = hal.interface.binding.subspan set(0) binding(2) type(storage_buffer) alignment(64)
      // CHECK:      %[[D3:.+]] = flow.dispatch.tensor.load %[[D0]], offsets = [0, 0], sizes = [250, 500]
      // CHECK:      %[[D4:.+]] = flow.dispatch.tensor.load %[[D1]], offsets = [0, 0], sizes = [500, 1020]
      // CHECK:      %[[D5:.+]] = tensor.empty() : tensor<250x1020xf32>
      // CHECK-NEXT: %[[D6:.+]] = linalg.fill ins(%[[CST]] : f32) outs(%[[D5]] : tensor<250x1020xf32>) -> tensor<250x1020xf32>
      // CHECK-NEXT: %[[D7:.+]] = bufferization.alloc_tensor() copy(%[[D3]]) {bufferization.escape = [false]} : tensor<250x500xf32>
      // CHECK-NEXT: %[[D8:.+]] = bufferization.alloc_tensor() copy(%[[D4]]) {bufferization.escape = [false]} : tensor<500x1020xf32>
      // CHECK-NEXT: %[[D9:.+]] = linalg.matmul ins(%[[D7]], %[[D8]] : tensor<250x500xf32>, tensor<500x1020xf32>)
      // CHECK-SAME:                 outs(%[[D6]] : tensor<250x1020xf32>) -> tensor<250x1020xf32>
      %6 = linalg.matmul ins(%3, %4 : tensor<250x500xf32>, tensor<500x1020xf32>) outs(%5 : tensor<250x1020xf32>) -> tensor<250x1020xf32>
      return %6: tensor<250x1020xf32>
    }
  }

  transform.structured.canonicalized_sequence failures(propagate) {
  ^bb1(%variant_op: !pdl.operation):
    %matmul = transform.structured.match ops{["linalg.matmul"]} in %variant_op : (!pdl.operation) -> !pdl.operation
    transform.iree.promote_operands %matmul [0, 1] : (!pdl.operation) -> (!pdl.operation, !pdl.operation)
  }
}

