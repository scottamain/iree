## Copyright 2022 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Generates CMake rules to build IREE artifacts."""

from dataclasses import dataclass
from typing import Dict, List, Sequence
import pathlib

from benchmark_suites.iree import benchmark_collections
from e2e_test_artifacts import iree_artifacts
from e2e_test_artifacts.cmake_generator import model_rule_generator
from e2e_test_framework.definitions import common_definitions, iree_definitions
import cmake_builder.rules

BENCHMARK_IMPORT_MODELS_CMAKE_TARGET = "iree-benchmark-import-models"
BENCHMARK_SUITES_CMAKE_TARGET = "iree-benchmark-suites"
E2E_COMPILE_STATS_SUITES = "iree-e2e-compile-stats-suites"


@dataclass(frozen=True)
class IreeModelImportRule(object):
  target_name: str
  output_file_path: pathlib.PurePath
  cmake_rules: List[str]


@dataclass(frozen=True)
class IreeModuleCompileRule(object):
  target_name: str
  output_module_path: pathlib.PurePath
  cmake_rules: List[str]


class IreeRuleBuilder(object):
  """Builder to generate IREE CMake rules."""

  _package_name: str

  def __init__(self, package_name: str):
    self._package_name = package_name

  def build_model_import_rule(
      self, source_model_rule: model_rule_generator.ModelRule,
      imported_model: iree_definitions.ImportedModel,
      output_file_path: pathlib.PurePath) -> IreeModelImportRule:

    model = imported_model.model
    if model.source_type == common_definitions.ModelSourceType.EXPORTED_LINALG_MLIR:
      if source_model_rule.file_path != output_file_path:
        raise ValueError(
            f"Separate path for Linalg model isn't supported "
            f"('{source_model_rule.file_path }' != '{output_file_path}')")
      return IreeModelImportRule(target_name=source_model_rule.target_name,
                                 output_file_path=output_file_path,
                                 cmake_rules=[])

    # Import target name: iree-imported-model-<model_id>
    target_name = f"iree-imported-model-{model.id}"

    if model.source_type == common_definitions.ModelSourceType.EXPORTED_TFLITE:
      cmake_rules = [
          cmake_builder.rules.build_iree_import_tflite_model(
              target_path=self.build_target_path(target_name),
              source=str(source_model_rule.file_path),
              output_mlir_file=str(output_file_path))
      ]
    elif model.source_type == common_definitions.ModelSourceType.EXPORTED_TF:
      cmake_rules = [
          cmake_builder.rules.build_iree_import_tf_model(
              target_path=self.build_target_path(target_name),
              source=str(source_model_rule.file_path),
              entry_function=model.entry_function,
              output_mlir_file=str(output_file_path))
      ]
    else:
      raise ValueError(
          f"Unsupported source type '{model.source_type}' of the model '{model.id}'."
      )

    return IreeModelImportRule(target_name=target_name,
                               output_file_path=output_file_path,
                               cmake_rules=cmake_rules)

  def build_module_compile_rule(
      self, model_import_rule: IreeModelImportRule,
      imported_model: iree_definitions.ImportedModel,
      compile_config: iree_definitions.CompileConfig,
      output_file_path: pathlib.PurePath) -> IreeModuleCompileRule:

    compile_flags = self._generate_compile_flags(
        compile_config=compile_config,
        mlir_dialect_type=imported_model.dialect_type.value
    ) + compile_config.extra_flags

    # Module target name: iree-module-<model_id>-<compile_config_id>
    target_name = f"iree-module-{imported_model.model.id}-{compile_config.id}"

    cmake_rules = [
        cmake_builder.rules.build_iree_bytecode_module(
            target_name=target_name,
            src=str(model_import_rule.output_file_path),
            module_name=str(output_file_path),
            flags=compile_flags)
    ]

    # TODO(#10155): Dump the compile flags from iree_bytecode_module into a flagfile.

    return IreeModuleCompileRule(target_name=target_name,
                                 output_module_path=output_file_path,
                                 cmake_rules=cmake_rules)

  def build_target_path(self, target_name: str):
    """Returns the full target path by combining the package name and the target
    name.
    """
    return f"{self._package_name}_{target_name}"

  def _generate_compile_flags(self,
                              compile_config: iree_definitions.CompileConfig,
                              mlir_dialect_type: str) -> List[str]:
    if len(compile_config.compile_targets) != 1:
      raise ValueError(f"Only one compile target is supported. Got:"
                       f" {compile_config.compile_targets}")

    compile_target = compile_config.compile_targets[0]
    flags = [
        f"--iree-hal-target-backends={compile_target.target_backend.value}",
        f"--iree-input-type={mlir_dialect_type}"
    ]
    flags.extend(self._generate_compile_target_flags(compile_target))
    return flags

  def _generate_compile_target_flags(
      self, target: iree_definitions.CompileTarget) -> List[str]:
    arch_info = target.target_architecture
    if arch_info.architecture == "x86_64":
      flags = [
          f"--iree-llvm-target-triple=x86_64-unknown-{target.target_abi.value}",
          f"--iree-llvm-target-cpu={arch_info.microarchitecture.lower()}"
      ]
    elif arch_info.architecture == "riscv_64":
      flags = [
          f"--iree-llvm-target-triple=riscv64-pc-{target.target_abi.value}",
          "--iree-llvm-target-cpu=generic-rv64", "--iree-llvm-target-abi=lp64d",
          "--iree-llvm-target-cpu-features=+m,+a,+f,+d,+zvl512b,+v",
          "--riscv-v-fixed-length-vector-lmul-max=8"
      ]
    elif arch_info.architecture == "riscv_32":
      flags = [
          f"--iree-llvm-target-triple=riscv32-pc-{target.target_abi.value}",
          "--iree-llvm-target-cpu=generic-rv32", "--iree-llvm-target-abi=ilp32",
          "--iree-llvm-target-cpu-features=+m,+a,+f,+zvl512b,+zve32x",
          "--riscv-v-fixed-length-vector-lmul-max=8"
      ]
    elif arch_info.architecture == "adreno":
      flags = [
          f"--iree-vulkan-target-triple=adreno-unknown-{target.target_abi.value}",
      ]
    elif arch_info.architecture == "mali":
      flags = [
          f"--iree-vulkan-target-triple=valhall-unknown-{target.target_abi.value}",
      ]
    elif arch_info.architecture == "armv8.2-a":
      flags = [
          f"--iree-llvm-target-triple=aarch64-none-{target.target_abi.value}",
      ]
    elif arch_info.architecture == "cuda":
      if target.target_abi != iree_definitions.TargetABI.LINUX_GNU:
        raise ValueError(
            f"Unsupported target ABI for CUDA backend: `{target.target_abi}`")
      flags = [
          f"--iree-hal-cuda-llvm-target-arch=sm_80",
      ]
    elif arch_info.architecture == "vmvx":
      flags = []
    else:
      raise ValueError(f"Unsupported architecture: '{arch_info.architecture}'")
    return flags


def generate_rules(
    package_name: str, root_path: pathlib.PurePath,
    module_generation_configs: Sequence[
        iree_definitions.ModuleGenerationConfig],
    model_rule_map: Dict[str, model_rule_generator.ModelRule]) -> List[str]:
  """Generates all rules to build IREE artifacts.

  Args:
    package_name: CMake package name for rules.
    root_path: path of the root artifact directory.
    module_generation_configs: list of IREE module generation configs.
    model_rule_map: map of generated model rules keyed by model id, it must
      cover all model referenced in module_generation_configs.
  Returns:
    List of cmake rules.
  """

  rule_builder = IreeRuleBuilder(package_name=package_name)

  all_imported_models = dict(
      (config.imported_model.model.id, config.imported_model)
      for config in module_generation_configs)

  cmake_rules = []
  model_import_rule_map = {}
  for model_id, imported_model in all_imported_models.items():
    model_rule = model_rule_map.get(imported_model.model.id)
    if model_rule is None:
      raise ValueError(f"Model rule not found for {imported_model.model.id}.")

    imported_model_path = iree_artifacts.get_imported_model_path(
        imported_model=imported_model, root_path=root_path)
    model_import_rule = rule_builder.build_model_import_rule(
        source_model_rule=model_rule,
        imported_model=imported_model,
        output_file_path=imported_model_path)
    model_import_rule_map[model_id] = model_import_rule
    cmake_rules.extend(model_import_rule.cmake_rules)

  module_target_names = []
  compile_stats_module_target_names = []
  for gen_config in module_generation_configs:
    model_import_rule = model_import_rule_map[
        gen_config.imported_model.model.id]
    module_dir_path = iree_artifacts.get_module_dir_path(
        module_generation_config=gen_config, root_path=root_path)
    module_compile_rule = rule_builder.build_module_compile_rule(
        model_import_rule=model_import_rule,
        imported_model=gen_config.imported_model,
        compile_config=gen_config.compile_config,
        output_file_path=module_dir_path / iree_artifacts.MODULE_FILENAME)
    if benchmark_collections.COMPILE_STATS_TAG in gen_config.compile_config.tags:
      compile_stats_module_target_names.append(module_compile_rule.target_name)
    else:
      module_target_names.append(module_compile_rule.target_name)
    cmake_rules.extend(module_compile_rule.cmake_rules)

  if len(model_import_rule_map) > 0:
    cmake_rules.append(
        cmake_builder.rules.build_add_dependencies(
            target=BENCHMARK_IMPORT_MODELS_CMAKE_TARGET,
            deps=[
                rule_builder.build_target_path(rule.target_name)
                for rule in model_import_rule_map.values()
            ]))
  if len(module_target_names) > 0:
    cmake_rules.append(
        cmake_builder.rules.build_add_dependencies(
            target=BENCHMARK_SUITES_CMAKE_TARGET,
            deps=[
                rule_builder.build_target_path(target_name)
                for target_name in module_target_names
            ]))
  if len(compile_stats_module_target_names) > 0:
    cmake_rules.append(
        cmake_builder.rules.build_add_dependencies(
            target=E2E_COMPILE_STATS_SUITES,
            deps=[
                rule_builder.build_target_path(target_name)
                for target_name in compile_stats_module_target_names
            ]))

  return cmake_rules
