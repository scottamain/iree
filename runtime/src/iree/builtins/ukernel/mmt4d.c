// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/builtins/ukernel/mmt4d.h"

#include "iree/builtins/ukernel/mmt4d_tile.h"

#define OUTSIDE_UINT_RANGE(value, bits) (((value) < 0) || ((value) >> (bits)))

static void iree_uk_mmt4d_validate(const iree_uk_mmt4d_params_t* params) {
#ifdef IREE_UK_ENABLE_ASSERTS
  IREE_UK_ASSERT(!(params->flags & ~IREE_UK_FLAG_ACCUMULATE));
  IREE_UK_ASSERT(params->type == iree_uk_mmt4d_type_f32f32f32 ||
                 params->type == iree_uk_mmt4d_type_i8i8i32);
  // Some implementations may wish to avoid supporting absurdly wide types. For
  // instance, K is the innermost (i.e. hottest) loop bound, so some 32bit
  // targets may benefit from K being int32, not int64. We still let K be of
  // type int64 to be future-proof, as types are hard to change later. But we
  // enforce a narrower range here, as we can always relax that later as needed.
  IREE_UK_ASSERT(IREE_UK_VALUE_IN_UNSIGNED_INT_RANGE(params->M, 31));
  IREE_UK_ASSERT(IREE_UK_VALUE_IN_UNSIGNED_INT_RANGE(params->N, 31));
  IREE_UK_ASSERT(IREE_UK_VALUE_IN_UNSIGNED_INT_RANGE(params->K, 31));
  // int32 is overkill for the inner tile sizes. Enforce int16 range for now.
  IREE_UK_ASSERT(IREE_UK_VALUE_IN_UNSIGNED_INT_RANGE(params->M0, 15));
  IREE_UK_ASSERT(IREE_UK_VALUE_IN_UNSIGNED_INT_RANGE(params->N0, 15));
  IREE_UK_ASSERT(IREE_UK_VALUE_IN_UNSIGNED_INT_RANGE(params->K0, 15));
  // Ensure iree_uk_mmt4d_tile_generic_max_bytes large enough for this tile.
  IREE_UK_ASSERT(params->M0 * params->N0 *
                     iree_uk_type_size(iree_uk_mmt4d_out_type(params->type)) <=
                 iree_uk_mmt4d_tile_generic_max_bytes);
#endif  // IREE_UK_ENABLE_ASSERTS
}

// General mmt4d implementation, shared among all cases. The idea is that the
// only really performance-critical part is the inner-most loop, and that's
// handled by the tile_func passed as argument here. Sharing the outer loops
// across all cases is a roughly 2x code shrink compared to if we were
// emitting the whole loop nest for each case.
static void iree_uk_mmt4d_using_tile_func(const iree_uk_mmt4d_params_t* params,
                                          iree_uk_mmt4d_tile_func_t tile_func) {
  const iree_uk_int32_t M = params->M;
  const iree_uk_int32_t N = params->N;
  const iree_uk_int32_t K = params->K;
  const iree_uk_int16_t M0 = params->M0;
  const iree_uk_int16_t N0 = params->N0;
  const iree_uk_type_t lhs_type = iree_uk_mmt4d_lhs_type(params->type);
  const iree_uk_type_t rhs_type = iree_uk_mmt4d_rhs_type(params->type);
  const iree_uk_type_t out_type = iree_uk_mmt4d_out_type(params->type);
  const iree_uk_int16_t lhs_elem_size_log2 = iree_uk_type_size_log2(lhs_type);
  const iree_uk_int16_t rhs_elem_size_log2 = iree_uk_type_size_log2(rhs_type);
  const iree_uk_int16_t out_elem_size_log2 = iree_uk_type_size_log2(out_type);
  char* out_tile_row = params->out_buffer;
  const char* lhs_panel = params->lhs_buffer;
  iree_uk_int32_t out_tile_size = (M0 * N0) << out_elem_size_log2;
  iree_uk_ssize_t lhs_panel_stride = params->lhs_stride << lhs_elem_size_log2;
  iree_uk_ssize_t rhs_panel_stride = params->rhs_stride << rhs_elem_size_log2;
  iree_uk_ssize_t out_stride = params->out_stride << out_elem_size_log2;
  for (iree_uk_int32_t i = 0; i < M; ++i) {
    char* out_tile = out_tile_row;
    const char* rhs_panel = params->rhs_buffer;
    for (iree_uk_int32_t j = 0; j < N; ++j) {
      tile_func(out_tile, lhs_panel, rhs_panel, K, params->flags, params);
      out_tile += out_tile_size;
      rhs_panel += rhs_panel_stride;
    }
    out_tile_row += out_stride;
    lhs_panel += lhs_panel_stride;
  }
}

// Helper for early-return path when K==0 and we just need to clear the output.
static void iree_uk_mmt4d_zero_out(const iree_uk_mmt4d_params_t* params) {
  iree_uk_type_t out_type = iree_uk_mmt4d_out_type(params->type);
  int out_type_size_log2 = iree_uk_type_size_log2(out_type);
  iree_uk_ssize_t contiguous_size = params->N * params->M0 * params->N0
                                    << out_type_size_log2;
  iree_uk_ssize_t stride = params->out_stride << out_type_size_log2;
  char* out_ptr = params->out_buffer;
  for (iree_uk_ssize_t i = 0; i < params->M; ++i) {
    iree_uk_memset(out_ptr, 0, contiguous_size);
    out_ptr += stride;
  }
}

// Early-return code paths, including trivial or near-trivial cases (when one
// of the dimensions is 0) and in the future, hardware ports that specialize
// the entire loop nest.
// Returns true if already done.
static bool iree_uk_mmt4d_early(const iree_uk_mmt4d_params_t* params) {
  // Trivial cases
  if (params->M == 0 || params->N == 0) {
    return true;
  }
  if (params->K == 0) {
    if (params->flags & IREE_UK_FLAG_ACCUMULATE) {
      // Nothing to do!
    } else {
      iree_uk_mmt4d_zero_out(params);
    }
    return true;
  }

  // Targets that want to specialize the entire loop nest can do so here.

  return false;
}

IREE_UK_EXPORT void iree_uk_mmt4d(const iree_uk_mmt4d_params_t* params) {
  iree_uk_mmt4d_validate(params);

  // Maybe handle this mmt4d "early", without needing to select a tile_func.
  // Typical cases include trivial cases (e.g. when params->K == 0) and hardware
  // targets that want to handle the entire loop nest in target-specific code.
  if (iree_uk_mmt4d_early(params)) return;

  // Select a target-specific tile_func (inner loop on K, computing one M0xN0
  // tile) and use that with generic outer loops.
  iree_uk_mmt4d_tile_func_t tile_func = iree_uk_mmt4d_select_tile_func(params);
  iree_uk_mmt4d_using_tile_func(params, tile_func);
}
