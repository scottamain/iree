// RUN: iree-opt %s

transform.structured.canonicalized_sequence failures(propagate) {
^bb1(%variant_op: !pdl.operation):
  %0 = transform.structured.match ops{["linalg.matmul"]} in %variant_op : (!pdl.operation) -> !pdl.operation

  %foreach_thread, %tiled_generic =
    transform.structured.tile_to_foreach_thread_op %0 num_threads [2] 
    // TODO: IREE needs own workgroup mapping attribute.
    ( mapping = [#gpu.block<x>] )

  %1 = transform.iree.bufferize %variant_op
  %memref_func = transform.structured.match ops{["func.func"]} in %1 : (!pdl.operation) -> !pdl.operation
  transform.iree.erase_hal_descriptor_type_from_memref %memref_func

  %func = transform.structured.match ops{["func.func"]} in %1 : (!pdl.operation) -> !pdl.operation
  transform.iree.foreach_thread_to_workgroup %func
}
