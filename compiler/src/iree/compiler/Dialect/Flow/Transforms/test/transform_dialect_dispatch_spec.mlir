transform.structured.canonicalized_sequence failures(propagate) {
^bb1(%arg1: !pdl.operation):
  %0 = transform.structured.match ops{["linalg.matmul"]} in %arg1 : (!pdl.operation) -> !pdl.operation
  %foreach_op, %tiled_op = transform.structured.tile_to_foreach_thread_op %0 num_threads [42, 67]
  %dispatch_op = transform.iree.foreach_thread_to_flow %foreach_op
}
