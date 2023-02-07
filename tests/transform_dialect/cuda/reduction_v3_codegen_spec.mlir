// RUN: iree-opt %s

transform.structured.canonicalized_sequence failures(propagate) {
^bb1(%variant_op: !pdl.operation):
  %fill = transform.structured.match ops{["linalg.fill"]} in %variant_op : (!pdl.operation) -> !pdl.operation
  %reduction = transform.structured.match ops{["linalg.generic"]} in %variant_op : (!pdl.operation) -> !pdl.operation

  // Step 1. First level of tiling + fusion parallelizes to blocks.
  // ===========================================================================
  %foreach_thread_grid, %grid_reduction =
    transform.iree.tile_to_foreach_thread_and_workgroup_count_region %reduction tile_sizes [1]
      ( mapping = [#gpu.block<x>] )
  transform.structured.fuse_into_containing_op %fill into %foreach_thread_grid

  // Step 2. Split the reduction to get meatier parallelism.
  // This also parallelizes to threads.
  // ===========================================================================
  %foreach_thread, %block_more_parallel_fill_op_2, %block_more_parallel_op_2, %block_combiner_op_2 = 
     transform.structured.tile_reduction_using_foreach_thread %grid_reduction 
        by num_threads = [0, 1024], tile_sizes = [0, 1], mapping = [#gpu.thread<x>]

  // Fuse the fill and pointwise to privatize them.
  transform.structured.fuse_into_containing_op %block_more_parallel_fill_op_2
    into %foreach_thread

  // block_combiner_op_2 op is [parallel, reduction] of 1x384 that cannot fuse.
  // map the 1-dim to threadIdx.y to trigger mapping of the reduction to 
  // threadIdx.x via predication via `if (x==0)`.
  transform.structured.tile_to_foreach_thread_op %block_combiner_op_2 num_threads [1] 
    ( mapping = [#gpu.thread<y>] )

  // Step 3. Rank-reduce and vectorize.
  // ===========================================================================
  %func = transform.structured.match ops{["func.func"]} in %variant_op : (!pdl.operation) -> !pdl.operation
  // TODO: masked vectorization on block_more_parallel_op_2 if we want 
  // vector<4> to work as intended.
  %func_2 = transform.iree.apply_patterns %func {  rank_reducing_linalg, rank_reducing_vector }
  %func_3 = transform.structured.vectorize %func_2

  // Step 4. Bufferize and drop HAL descriptor from memref ops.
  // ===========================================================================
  %func_4 = transform.iree.apply_patterns %func_3 { fold_reassociative_reshapes }
  %variant_op_2 = transform.iree.eliminate_empty_tensors %variant_op
  %func_5 = transform.structured.match ops{["func.func"]} in %variant_op_2 : (!pdl.operation) -> !pdl.operation
  %func_6 = transform.iree.apply_patterns %func_5 { erase_unnecessary_tensor_operands }
  %variant_op_3 = transform.iree.bufferize { target_gpu } %variant_op_2
  %memref_func = transform.structured.match ops{["func.func"]} in %variant_op_3 : (!pdl.operation) -> !pdl.operation
  transform.iree.erase_hal_descriptor_type_from_memref %memref_func

  // Step 5. Post-bufferization mapping to blocks and threads.
  // ===========================================================================
  %func_7 = transform.structured.match ops{["func.func"]} in %variant_op_3 : (!pdl.operation) -> !pdl.operation
  %func_8 = transform.iree.foreach_thread_to_workgroup %func_7
  %func_9 = transform.iree.map_nested_foreach_thread_to_gpu_threads %func_8
      { workgroup_size = [1024, 1, 1] }

  // Step 6. Post-bufferization vector distribution with rank-reduction.
  // ===========================================================================
  %func_10 = transform.iree.apply_patterns %func_9 { rank_reducing_linalg, rank_reducing_vector, fold_memref_aliases }
  %if_op = transform.structured.match ops{["scf.if"]} in %variant_op_3 : (!pdl.operation) -> !pdl.operation
  %warp = transform.iree.vector.to_warp_execute_on_lane_0 %if_op { warp_size = 32 }
  transform.iree.vector.warp_distribute %func_10
}
