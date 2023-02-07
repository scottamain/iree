// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/builtins/ukernel/pack.h"

#include "iree/builtins/ukernel/pack_tile.h"

enum { iree_uk_pack_tmp_buf_size = 4096 };

// Holds some information and a temporary buffer for performing padding.
typedef struct iree_uk_pack_tmpbuf_helper_t {
  // Temporary buffer to pad the source data into, to pass to the tile_func.
  // Cache line alignment helps in pack_benchmark on ARM Cortex-A510/A710.
  IREE_UK_ATTRIBUTE_ALIGNED(64) char tmp_buf[iree_uk_pack_tmp_buf_size];
  // How many tiles fit in `tmp_buf`.
  int max_tiles_in_tmp_buf;
  // Is the padding value as single-byte pattern (so we can use memset).
  bool is_padding_single_byte;
} iree_uk_pack_tmpbuf_helper_t;

// Return x/y for x>=0 and y>0, with a fast path for when y is a power of two.
static iree_uk_ssize_t iree_uk_div_nonneg_by_pos_and_likely_po2_i32(
    iree_uk_ssize_t x, iree_uk_int32_t y) {
  IREE_UK_ASSERT(x >= 0);
  IREE_UK_ASSERT(y > 0);
  return IREE_UK_LIKELY(iree_uk_is_po2_u32(y)) ? (x >> iree_uk_po2_log2_u32(y))
                                               : (x / y);
}

// Returns true if the `num_bytes` bytes at `buf` are all equal.
static bool iree_uk_is_single_byte_pattern(const char* buf,
                                           iree_uk_ssize_t num_bytes) {
  for (iree_uk_ssize_t i = 1; i < num_bytes; ++i) {
    if (buf[i] != buf[0]) return false;
  }
  return true;
}

// Initializes a `iree_uk_pack_tmpbuf_helper_t`. Asserts if the temporary buffer
// is smaller than one tile.
static void iree_uk_pack_tmpbuf_helper_t_init(
    iree_uk_ssize_t tile_size0, iree_uk_ssize_t tile_size1,
    iree_uk_ssize_t elem_size, const void* padding_value,
    iree_uk_pack_tmpbuf_helper_t* helper) {
  helper->max_tiles_in_tmp_buf = iree_uk_div_nonneg_by_pos_and_likely_po2_i32(
      iree_uk_pack_tmp_buf_size, tile_size0 * tile_size1 * elem_size);
  IREE_UK_ASSERT(helper->max_tiles_in_tmp_buf > 0);
  helper->is_padding_single_byte =
      iree_uk_is_single_byte_pattern(padding_value, elem_size);
}

static void iree_uk_pack_validate(const iree_uk_pack_params_t* params) {
#ifdef IREE_UK_ENABLE_ASSERTS
  const iree_uk_uint32_t allflags =
      IREE_UK_FLAG_PACK_TRANSPOSE_INNER | IREE_UK_FLAG_PACK_TRANSPOSE_OUTER;
  IREE_UK_ASSERT(!(params->flags & ~allflags));
  IREE_UK_ASSERT(params->type == iree_uk_pack_type_f32f32 ||
                 params->type == iree_uk_pack_type_i8i8 ||
                 params->type == iree_uk_pack_type_i32i32);
  IREE_UK_ASSERT(params->in_stride0 >= 0);
  IREE_UK_ASSERT(params->out_stride0 >= 0);
  IREE_UK_ASSERT(params->in_size0 >= 0);
  IREE_UK_ASSERT(params->in_size1 >= 0);
  IREE_UK_ASSERT(params->out_size0 >= 0);
  IREE_UK_ASSERT(params->out_size1 >= 0);
  IREE_UK_ASSERT(params->out_size2 >= 0);
  IREE_UK_ASSERT(params->out_size3 >= 0);
  // Check that the input and output shapes match, give or take padding that
  // must not exceed the inner tile size.s
  iree_uk_ssize_t outer_size0 = params->out_size0;
  iree_uk_ssize_t outer_size1 = params->out_size1;
  iree_uk_ssize_t tile_size0 = params->out_size2;
  iree_uk_ssize_t tile_size1 = params->out_size3;
  if (params->flags & IREE_UK_FLAG_PACK_TRANSPOSE_OUTER) {
    iree_uk_ssize_swap(&outer_size0, &outer_size1);
  }
  if (params->flags & IREE_UK_FLAG_PACK_TRANSPOSE_INNER) {
    iree_uk_ssize_swap(&tile_size0, &tile_size1);
  }
  IREE_UK_ASSERT(outer_size0 * tile_size0 >= params->in_size0);
  IREE_UK_ASSERT(outer_size1 * tile_size1 >= params->in_size1);
  // TODO(#11632): reenable these conditions.
  // IREE_UK_ASSERT((outer_size0 - 1) * tile_size0 < params->in_size0);
  // IREE_UK_ASSERT((outer_size1 - 1) * tile_size1 < params->in_size1);

  // Initialize a padding helper, just to get the assertion that the tile size
  // does not exceed the internal temporary buffer size, without having to
  // duplicate this arithmetic. Generally, we want to hit all failure modes
  // in the validation function so that the subsequent ukernel code can be
  // treated as infallible.
  iree_uk_pack_tmpbuf_helper_t padding_helper;
  iree_uk_type_t elem_type = iree_uk_pack_in_type(params->type);
  iree_uk_ssize_t elem_size = iree_uk_type_size(elem_type);
  iree_uk_pack_tmpbuf_helper_t_init(tile_size0, tile_size1, elem_size,
                                    params->padding_value, &padding_helper);
#endif  // IREE_UK_ENABLE_ASSERTS
}

// Early-return implementation for this ukernel. Returns true if already done.
static bool iree_uk_pack_early(const iree_uk_pack_params_t* params) {
  return (params->out_size0 == 0 || params->out_size1 == 0 ||
          params->out_size2 == 0 || params->out_size3 == 0);
}

// Fills `buf` with `num_elems` times the `pattern` of size `elem_size`.
// If this pattern's `elem_size` bytes are all equal, then it is legal to pass
// `is_single_byte_pattern=true`, which allows the impl to use memset.
static void iree_uk_fill(char* buf, iree_uk_ssize_t num_elems,
                         iree_uk_ssize_t elem_size, bool is_single_byte_pattern,
                         const char* pattern) {
  if (is_single_byte_pattern) {
    iree_uk_memset(buf, pattern[0], num_elems * elem_size);
  } else {
    for (iree_uk_ssize_t i = 0; i < num_elems; ++i) {
      iree_uk_memcpy(buf + i * elem_size, pattern, elem_size);
    }
  }
}

// Copy from a source 2D buffer to a destination 2D buffer, padding to the
// destination size.
static void iree_uk_copy_and_pad(iree_uk_ssize_t src_size0,
                                 iree_uk_ssize_t src_size1,
                                 iree_uk_ssize_t src_stride0,
                                 const char* src_buf, iree_uk_ssize_t dst_size0,
                                 iree_uk_ssize_t dst_size1,
                                 iree_uk_ssize_t dst_stride0, char* dst_buf,
                                 iree_uk_ssize_t elem_size, const char* padding,
                                 bool padding_is_single_byte) {
  iree_uk_fill(dst_buf, dst_size1 + (dst_size0 - 1) * dst_stride0, elem_size,
               padding_is_single_byte, padding);
  for (iree_uk_ssize_t in_i0 = 0; in_i0 < src_size0; in_i0++) {
    iree_uk_memcpy(dst_buf, src_buf, src_size1 * elem_size);
    dst_buf += dst_stride0 * elem_size;
    src_buf += src_stride0 * elem_size;
  }
}

// Pads and packs an entire row. In cases that are known not to require padding,
// it is more efficient to call tile_func directly.
static void iree_uk_pad_and_pack_row_using_tile_func(
    iree_uk_pack_tile_func_t tile_func, iree_uk_ssize_t dim1_tile_start,
    iree_uk_ssize_t dim1_tile_end, iree_uk_ssize_t dim0_src_read_size,
    iree_uk_ssize_t tile_size0, iree_uk_ssize_t tile_size1,
    iree_uk_ssize_t elem_size, iree_uk_ssize_t in_size1,
    iree_uk_ssize_t in_stride0, iree_uk_ssize_t out_stride1,
    const void* padding_value, iree_uk_pack_tmpbuf_helper_t* helper,
    const char* in_buf, char* out_buf) {
  iree_uk_ssize_t dim1_tile = dim1_tile_start;
  while (dim1_tile < dim1_tile_end) {
    iree_uk_ssize_t dim1_chunk_tiles = iree_uk_ssize_clamp(
        dim1_tile_end - dim1_tile, 0, helper->max_tiles_in_tmp_buf);
    iree_uk_ssize_t dim1_chunk_src_width = dim1_chunk_tiles * tile_size1;
    iree_uk_ssize_t dim1_chunk_src_pos = dim1_tile * tile_size1;
    iree_uk_ssize_t i1_read_size = iree_uk_ssize_clamp(
        in_size1 - dim1_chunk_src_pos, 0, dim1_chunk_src_width);
    iree_uk_copy_and_pad(dim0_src_read_size, i1_read_size, in_stride0,
                         in_buf + dim1_chunk_src_pos * elem_size, tile_size0,
                         dim1_chunk_src_width, dim1_chunk_src_width,
                         helper->tmp_buf, elem_size, padding_value,
                         helper->is_padding_single_byte);
    tile_func(out_buf + (dim1_tile * out_stride1 * elem_size), helper->tmp_buf,
              dim1_chunk_tiles, out_stride1, dim1_chunk_src_width, elem_size,
              tile_size0, tile_size1);
    dim1_tile += dim1_chunk_tiles;
  }
}

static void iree_uk_pack_using_tile_func(const iree_uk_pack_params_t* params,
                                         iree_uk_pack_tile_func_t tile_func) {
  // For now, the input and output element types are always the same.
  iree_uk_type_t elem_type = iree_uk_pack_in_type(params->type);
  iree_uk_ssize_t elem_size = iree_uk_type_size(elem_type);
  iree_uk_ssize_t outer_size0 = params->out_size0;
  iree_uk_ssize_t outer_size1 = params->out_size1;
  iree_uk_ssize_t tile_size0 = params->out_size2;
  iree_uk_ssize_t tile_size1 = params->out_size3;
  iree_uk_ssize_t out_stride_l0 = params->out_stride0;
  iree_uk_ssize_t out_stride1 = params->out_size3 * params->out_size2;
  iree_uk_ssize_t out_stride_l2 = params->out_size3;
  iree_uk_ssize_t out_stride_l3 = 1;
  if (params->flags & IREE_UK_FLAG_PACK_TRANSPOSE_OUTER) {
    iree_uk_ssize_swap(&outer_size0, &outer_size1);
    iree_uk_ssize_swap(&out_stride_l0, &out_stride1);
  }
  if (params->flags & IREE_UK_FLAG_PACK_TRANSPOSE_INNER) {
    iree_uk_ssize_swap(&tile_size0, &tile_size1);
    iree_uk_ssize_swap(&out_stride_l2, &out_stride_l3);
  }
  const char* in_buf = params->in_buffer;
  char* out_buf = params->out_buffer;
  // Prepare for padding.
  iree_uk_pack_tmpbuf_helper_t padding_helper;
  if (params->in_size0 < outer_size0 * tile_size0 ||
      params->in_size1 < outer_size1 * tile_size1) {
    iree_uk_pack_tmpbuf_helper_t_init(tile_size0, tile_size1, elem_size,
                                      params->padding_value, &padding_helper);
  }
  // Compute number of tiles along both dimensions that fit entirely within the
  // source buffer's boundaries.
  int dim1_full_tiles = iree_uk_div_nonneg_by_pos_and_likely_po2_i32(
      params->in_size1, tile_size1);
  iree_uk_ssize_t i0 = 0;
  for (; i0 <= params->in_size0 - tile_size0; i0 += tile_size0) {
    // Pack whole tiles that do not require padding (entirely within the source
    // buffer's boundaries).
    tile_func(out_buf, in_buf, dim1_full_tiles, out_stride1, params->in_stride0,
              elem_size, tile_size0, tile_size1);
    // Right-padding.
    iree_uk_pad_and_pack_row_using_tile_func(
        tile_func, dim1_full_tiles, outer_size1, tile_size0, tile_size0,
        tile_size1, elem_size, params->in_size1, params->in_stride0,
        out_stride1, params->padding_value, &padding_helper, in_buf, out_buf);
    out_buf += out_stride_l0 * elem_size;
    in_buf += tile_size0 * params->in_stride0 * elem_size;
  }
  // Bottom-padding.
  for (; i0 < outer_size0 * tile_size0; i0 += tile_size0) {
    iree_uk_ssize_t dim0_src_read_size =
        iree_uk_ssize_clamp(params->in_size0 - i0, 0, tile_size0);
    iree_uk_pad_and_pack_row_using_tile_func(
        tile_func, 0, outer_size1, dim0_src_read_size, tile_size0, tile_size1,
        elem_size, params->in_size1, params->in_stride0, out_stride1,
        params->padding_value, &padding_helper, in_buf, out_buf);
    out_buf += out_stride_l0 * elem_size;
    in_buf += tile_size0 * params->in_stride0 * elem_size;
  }
}

IREE_UK_EXPORT void iree_uk_pack(const iree_uk_pack_params_t* params) {
  iree_uk_pack_validate(params);

  if (iree_uk_pack_early(params)) return;

  // Select a target-specific tile_func (inner loop on K, computing one M0xN0
  // tile) and use that with generic outer loops.
  iree_uk_pack_tile_func_t row_func = iree_uk_pack_select_tile_func(params);
  iree_uk_pack_using_tile_func(params, row_func);
}
