# Copyright 2022 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Provides utilities for benchmarking IREE modules.

Provides convenient methods for invoking IREE's benchmarking tooling from
python. This allows easy benchmarking results from within python.
"""

# pylint: disable=protected-access
# pylint: disable=unused-argument
# pylint: disable=g-explicit-length-test

# TODO(#4131) python>=3.7: Use postponed type annotations.

from collections import namedtuple

import iree.runtime
import numpy
import os
import subprocess

__all__ = [
    "benchmark_exe",
    "benchmark_module",
]

BenchmarkResult = namedtuple("BenchmarkResult",
                             "entry_function process_time real_time")

DTYPE_TO_ABI_TYPE = {
    numpy.dtype(numpy.float32): "f32",
    numpy.dtype(numpy.int32): "i32",
    numpy.dtype(numpy.int64): "i64",
    numpy.dtype(numpy.float64): "f64",
    numpy.dtype(numpy.int16): "i16",
    numpy.dtype(numpy.int8): "i8",
    numpy.dtype(numpy.bool_): "i1",
}


def benchmark_exe():
  return os.path.join(os.path.dirname(__file__), "..", "..",
                      "iree-benchmark-module")


def benchmark_module(module, entry_functiong=None, inputs=[], **kwargs):
  funcs = [a for a in module.function_names if a != "__init"]
  if entry_functiong is None:
    if len(funcs) > 1:
      raise ValueError(f"No function specified with multiple options {funcs}")
    entry_functiong = funcs[0]

  # Throw an error
  if entry_functiong not in funcs:
    raise ValueError(
        f"Attempted to benchmark unknown function {entry_functiong} of options {funcs}"
    )

  flatbuffer = module.stashed_flatbuffer_blob
  function = module.lookup_function(entry_functiong)
  args = [iree.runtime.benchmark_exe()]
  args.append(f"--function={funcs[0]}")

  for k in kwargs:
    v = kwargs[k]
    args.append(f"--{k}={v}")

  for inp in inputs:
    if isinstance(inp, str):
      args.append(f"--input={inp}")
      continue
    shape = "x".join([str(d) for d in inp.shape])
    abitype = DTYPE_TO_ABI_TYPE[inp.dtype]
    values = inp.flatten()
    if numpy.all(values[0] == values):
      values = str(values[0])
    else:
      values = ",".join([str(v) for v in values])

    args.append(f"--input={shape}x{abitype}={values}")

  call = subprocess.Popen(args=args,
                          stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
  out, err = call.communicate(input=flatbuffer)

  err = err.decode()
  if "INVALID_ARGUMENT;" in err:
    raise ValueError("Invalid inputs specified for benchmarking")

  out = out.decode().split("\n")[4]
  splt = out.split()
  process_time = splt[1]
  real_time = splt[3]
  return BenchmarkResult(entry_function=entry_functiong,
                         process_time=process_time,
                         real_time=real_time)
