// RUN: iree-opt %s

transform.structured.canonicalized_sequence failures(propagate) {
^bb1(%variant_op: !pdl.operation):
  %fill = transform.structured.match ops{["linalg.fill"]} in %variant_op : (!pdl.operation) -> !pdl.operation

  // Step 1. Split the reduction to get meatier (size(red) / 2)-way parallelism.
  // ===========================================================================
  %0 = transform.structured.match ops{["linalg.generic"]} in %variant_op : (!pdl.operation) -> !pdl.operation
  %eltwise, %reduction = transform.split_handles %0 in [2] : (!pdl.operation) -> (!pdl.operation, !pdl.operation)
  %init_or_alloc_op, %more_parallel_fill_op, %more_parallel_op, %combiner_op =
    transform.structured.split_reduction %reduction
      { split_factor = 2, insert_split_dimension = 1 }

  // Step 2. First level of tiling + fusion parallelizes to blocks.
  // ===========================================================================
  %foreach_thread_grid, %grid_combiner_op =
    transform.structured.tile_to_foreach_thread_op %combiner_op tile_sizes [1]
      ( mapping = [#gpu.block<x>] )

  // Step 2.1: Cannot fuse across the "expand_shape" produced by reduction
  // splitting above, so we need to bubble that up via patterns and rematch
  // the entire structure.
  // TODO: bubbling should be a proper transform op, at which point we will be
  // able to preserve the handles.
  // ===========================================================================
  %func = transform.structured.match ops{["func.func"]} in %variant_op : (!pdl.operation) -> !pdl.operation
  transform.iree.apply_patterns %func { bubble_collapse_expand }
  %fills = transform.structured.match ops{["linalg.fill"]} in %variant_op : (!pdl.operation) -> !pdl.operation
  %fill_2, %more_parallel_fill_2 = transform.split_handles %fills in [2]
    : (!pdl.operation) -> (!pdl.operation, !pdl.operation)
  %generics = transform.structured.match ops{["linalg.generic"]} in %variant_op : (!pdl.operation) -> !pdl.operation
  %expanded_eltwise, %more_parallel_2, %combiner_2 =
    transform.split_handles %generics in [3] : (!pdl.operation) -> (!pdl.operation, !pdl.operation, !pdl.operation)
  %foreach_thread_grid_2 = transform.structured.match ops{["scf.foreach_thread"]} in %variant_op : (!pdl.operation) -> !pdl.operation
  %not_combiner = transform.merge_handles %fill_2, %more_parallel_fill_2, %more_parallel_2, %expanded_eltwise : !pdl.operation
  transform.structured.fuse_into_containing_op %not_combiner into %foreach_thread_grid_2

  // Step 3. Second level of tiling + fusion parallelizes to threads. Also
  // fuse in the leading elementwise.
  // ===========================================================================
  %fill_1d = transform.structured.match ops{["linalg.fill"]} filter_result_type = tensor<1xf32> in %variant_op : (!pdl.operation) -> !pdl.operation
  %foreach_thread_block_combiner_op, %block_combiner_op =
    transform.structured.tile_to_foreach_thread_op %combiner_2 tile_sizes [1] 
    ( mapping = [#gpu.thread<z>] )
  transform.structured.fuse_into_containing_op %fill_1d into %foreach_thread_block_combiner_op

  %fill_2d = transform.structured.match ops{["linalg.fill"]} filter_result_type = tensor<1x2xf32> in %variant_op : (!pdl.operation) -> !pdl.operation
  %grid_more_parallel_op = transform.structured.match ops{["linalg.generic"]} : (!pdl.operation) -> !pdl.operation
    attributes{iterator_types = [#linalg.iterator_type<parallel>, #linalg.iterator_type<parallel>, #linalg.iterator_type<reduction>]} in %variant_op : (!pdl.operation) -> !pdl.operation
  %grid_eltwise_op = transform.structured.match ops{["linalg.generic"]} : (!pdl.operation) -> !pdl.operation
    attributes{iterator_types = [#linalg.iterator_type<parallel>, #linalg.iterator_type<parallel>, #linalg.iterator_type<parallel>]} in %variant_op : (!pdl.operation) -> !pdl.operation
  %foreach_thread_block_more_parallel_op, %block_more_parallel_op =
    transform.structured.tile_to_foreach_thread_op %grid_more_parallel_op tile_sizes [1, 1] 
    ( mapping = [#gpu.thread<z>, #gpu.thread<y>] )
  transform.structured.fuse_into_containing_op %fill_2d into %foreach_thread_block_more_parallel_op
  transform.structured.fuse_into_containing_op %grid_eltwise_op into %foreach_thread_block_more_parallel_op

  // Step 4. Rank-reduce and vectorize.
  // ===========================================================================
  %func_1 = transform.structured.match ops{["func.func"]} in %variant_op : (!pdl.operation) -> !pdl.operation
  %func_2 = transform.iree.apply_patterns %func_1 {  rank_reducing_linalg, rank_reducing_vector }
  %func_3 = transform.structured.vectorize %func_2

  // Step 5. Bufferize and drop HAL decriptor from memref ops.
  // ===========================================================================
  %variant_op_2 = transform.iree.eliminate_empty_tensors %variant_op
  %variant_op_3 = transform.iree.bufferize { target_gpu } %variant_op_2
  %memref_func = transform.structured.match ops{["func.func"]} in %variant_op_3 : (!pdl.operation) -> !pdl.operation
  transform.iree.erase_hal_descriptor_type_from_memref %memref_func

  // Step 6. Post-bufferization mapping to blocks and threads.
  // ===========================================================================
  %func_4 = transform.structured.match ops{["func.func"]} in %variant_op_3 : (!pdl.operation) -> !pdl.operation
  %func_5 = transform.iree.foreach_thread_to_workgroup %func_4
  %func_6 = transform.iree.map_nested_foreach_thread_to_gpu_threads %func_5
      { workgroup_size = [32, 2, 1] }

  // Step 7. Post-bufferization vector distribution with rank-reduction.
  // ===========================================================================
  %func_7 = transform.iree.apply_patterns %func_6 {  rank_reducing_linalg, rank_reducing_vector }
  %if_op = transform.structured.match ops{["scf.if"]} in %variant_op_3 : (!pdl.operation) -> !pdl.operation
  // Don't complain about unsupported if (threadIdx.x == 0 && threadIdx.y == 0)
  // at this point.
  transform.sequence %variant_op_3 : !pdl.operation failures(suppress) {
  ^bb0(%arg0: !pdl.operation):
    transform.iree.vector.to_warp_execute_on_lane_0 %if_op { warp_size = 32 }
  }
  transform.iree.vector.warp_distribute %func_7
}
