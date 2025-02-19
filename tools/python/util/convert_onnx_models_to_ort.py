#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

import argparse
import contextlib
import enum
import os
import pathlib
import tempfile
import typing

import onnxruntime as ort
from .file_utils import files_from_file_or_dir, path_match_suffix_ignore_case
from .onnx_model_utils import get_optimization_level
from .ort_format_model import create_config_from_models


class OptimizationStyle(enum.Enum):
    Fixed = 0
    Runtime = 1


def _optimization_suffix(optimization_level_str: str, optimization_style: OptimizationStyle, suffix: str):
    return "{}{}{}".format(f".{optimization_level_str}" if optimization_level_str != "all" else "",
                           ".with_runtime_opt" if optimization_style == OptimizationStyle.Runtime else "",
                           suffix)


def _create_config_file_path(model_path_or_dir: pathlib.Path,
                             optimization_level_str: str,
                             optimization_style: OptimizationStyle,
                             enable_type_reduction: bool):
    config_name = "{}{}".format('required_operators_and_types' if enable_type_reduction else 'required_operators',
                                _optimization_suffix(optimization_level_str, optimization_style, ".config"))
    if model_path_or_dir.is_dir():
        return model_path_or_dir / config_name
    return model_path_or_dir.with_suffix(f".{config_name}")


def _create_session_options(optimization_level: ort.GraphOptimizationLevel,
                            output_model_path: pathlib.Path,
                            custom_op_library: pathlib.Path,
                            session_options_config_entries: typing.Dict[str, str]):
    so = ort.SessionOptions()
    so.optimized_model_filepath = str(output_model_path)
    so.graph_optimization_level = optimization_level

    if custom_op_library:
        so.register_custom_ops_library(str(custom_op_library))

    for key, value in session_options_config_entries.items():
        so.add_session_config_entry(key, value)

    return so


def _convert(model_path_or_dir: pathlib.Path, output_dir: typing.Optional[pathlib.Path],
             optimization_level_str: str, optimization_style: OptimizationStyle,
             custom_op_library: pathlib.Path, create_optimized_onnx_model: bool, allow_conversion_failures: bool,
             target_platform: str, session_options_config_entries: typing.Dict[str, str]) \
        -> typing.List[pathlib.Path]:

    model_dir = model_path_or_dir if model_path_or_dir.is_dir() else model_path_or_dir.parent
    output_dir = output_dir or model_dir

    optimization_level = get_optimization_level(optimization_level_str)

    def is_model_file_to_convert(file_path: pathlib.Path):
        if not path_match_suffix_ignore_case(file_path, ".onnx"):
            return False
        # ignore any files with an extension of .optimized.onnx which are presumably from previous executions
        # of this script
        if path_match_suffix_ignore_case(file_path, ".optimized.onnx"):
            print(f"Ignoring '{file_path}'")
            return False
        return True

    models = files_from_file_or_dir(model_path_or_dir, is_model_file_to_convert)

    if len(models) == 0:
        raise ValueError("No model files were found in '{}'".format(model_path_or_dir))

    providers = ['CPUExecutionProvider']

    # if the optimization level is 'all' we manually exclude the NCHWc transformer. It's not applicable to ARM
    # devices, and creates a device specific model which won't run on all hardware.
    # If someone really really really wants to run it they could manually create an optimized onnx model first,
    # or they could comment out this code.
    optimizer_filter = None
    if optimization_level == ort.GraphOptimizationLevel.ORT_ENABLE_ALL and target_platform != 'amd64':
        optimizer_filter = ['NchwcTransformer']

    converted_models = []

    for model in models:
        try:

            relative_model_path = model.relative_to(model_dir)

            (output_dir / relative_model_path).parent.mkdir(parents=True, exist_ok=True)

            ort_target_path = (output_dir / relative_model_path).with_suffix(
                _optimization_suffix(optimization_level_str, optimization_style, ".ort"))

            if create_optimized_onnx_model:
                # Create an ONNX file with the same optimization level that will be used for the ORT format file.
                # This allows the ONNX equivalent of the ORT format model to be easily viewed in Netron.
                # If runtime optimizations are saved in the ORT format model, there may be some difference in the
                # graphs at runtime between the ORT format model and this saved ONNX model.
                optimized_target_path = (output_dir / relative_model_path).with_suffix(
                    _optimization_suffix(optimization_level_str, optimization_style, ".optimized.onnx"))
                so = _create_session_options(optimization_level, optimized_target_path, custom_op_library,
                                             session_options_config_entries)
                if optimization_style == OptimizationStyle.Runtime:
                    # Limit the optimizations to those that can run in a model with runtime optimizations.
                    so.add_session_config_entry('optimization.minimal_build_optimizations', 'apply')

                print("Saving optimized ONNX model {} to {}".format(model, optimized_target_path))
                _ = ort.InferenceSession(str(model), sess_options=so, providers=providers,
                                         disabled_optimizers=optimizer_filter)

            # Load ONNX model, optimize, and save to ORT format
            so = _create_session_options(optimization_level, ort_target_path, custom_op_library,
                                         session_options_config_entries)
            so.add_session_config_entry('session.save_model_format', 'ORT')
            if optimization_style == OptimizationStyle.Runtime:
                so.add_session_config_entry('optimization.minimal_build_optimizations', 'save')

            print("Converting optimized ONNX model {} to ORT format model {}".format(model, ort_target_path))
            _ = ort.InferenceSession(str(model), sess_options=so, providers=providers,
                                     disabled_optimizers=optimizer_filter)

            converted_models.append(ort_target_path)

            # orig_size = os.path.getsize(onnx_target_path)
            # new_size = os.path.getsize(ort_target_path)
            # print("Serialized {} to {}. Sizes: orig={} new={} diff={} new:old={:.4f}:1.0".format(
            #     onnx_target_path, ort_target_path, orig_size, new_size, new_size - orig_size, new_size / orig_size))
        except Exception as e:
            print("Error converting {}: {}".format(model, e))
            if not allow_conversion_failures:
                raise

    print("Converted {}/{} models successfully.".format(len(converted_models), len(models)))

    return converted_models


def parse_args():
    parser = argparse.ArgumentParser(
        os.path.basename(__file__),
        description='''Convert the ONNX format model/s in the provided directory to ORT format models.
        All files with a `.onnx` extension will be processed. For each one, an ORT format model will be created in the
        same directory. A configuration file will also be created containing the list of required operators for all
        converted models. This configuration file should be used as input to the minimal build via the
        `--include_ops_by_config` parameter.
        '''
    )

    parser.add_argument('--optimization_style',
                        nargs='+',
                        default=[OptimizationStyle.Fixed.name, OptimizationStyle.Runtime.name],
                        choices=[e.name for e in OptimizationStyle],
                        help="Style of optimization to perform on the ORT format model. "
                             "Multiple values may be provided. The conversion will run once for each value. "
                             "The general guidance is to use models optimized with "
                             f"'{OptimizationStyle.Runtime.name}' style when using NNAPI or CoreML and "
                             f"'{OptimizationStyle.Fixed.name}' style otherwise. "
                             f"'{OptimizationStyle.Fixed.name}': Run optimizations directly before saving the ORT "
                             "format model. This bakes in any platform-specific optimizations. "
                             f"'{OptimizationStyle.Runtime.name}': Run basic optimizations directly and save certain "
                             "other optimizations to be applied at runtime if possible. This is useful when using a "
                             "compiling EP like NNAPI or CoreML that may run an unknown (at model conversion time) "
                             "number of nodes. The saved optimizations can further optimize nodes not assigned to the "
                             "compiling EP at runtime.")

    parser.add_argument('--enable_type_reduction', action='store_true',
                        help='Add operator specific type information to the configuration file to potentially reduce '
                             'the types supported by individual operator implementations.')

    parser.add_argument('--custom_op_library', type=pathlib.Path, default=None,
                        help='Provide path to shared library containing custom operator kernels to register.')

    parser.add_argument('--save_optimized_onnx_model', action='store_true',
                        help='Save the optimized version of each ONNX model. '
                             'This will have the same level of optimizations applied as the ORT format model.')

    parser.add_argument('--allow_conversion_failures', action='store_true',
                        help='Whether to proceed after encountering model conversion failures.')

    parser.add_argument('--nnapi_partitioning_stop_ops',
                        help='Specify the list of NNAPI EP partitioning stop ops. '
                             'In particular, specify the value of the "ep.nnapi.partitioning_stop_ops" session '
                             'options config entry.')

    parser.add_argument('--target_platform', type=str, default=None, choices=['arm', 'amd64'],
                        help='Specify the target platform where the exported model will be used. '
                             'This parameter can be used to choose between platform-specific options, '
                             'such as QDQIsInt8Allowed(arm), NCHWc (amd64) and NHWC (arm/amd64) format, different '
                             'optimizer level options, etc.')

    parser.add_argument('model_path_or_dir', type=pathlib.Path,
                        help='Provide path to ONNX model or directory containing ONNX model/s to convert. '
                             'All files with a .onnx extension, including those in subdirectories, will be '
                             'processed.')

    return parser.parse_args()


def convert_onnx_models_to_ort():
    args = parse_args()

    optimization_styles = [OptimizationStyle[style_str] for style_str in args.optimization_style]
    # setting optimization level is not expected to be needed by typical users, but it can be set with this
    # environment variable
    optimization_level_str = os.getenv("ORT_CONVERT_ONNX_MODELS_TO_ORT_OPTIMIZATION_LEVEL", "all")
    model_path_or_dir = args.model_path_or_dir.resolve()
    custom_op_library = args.custom_op_library.resolve() if args.custom_op_library else None

    if not model_path_or_dir.is_dir() and not model_path_or_dir.is_file():
        raise FileNotFoundError("Model path '{}' is not a file or directory.".format(model_path_or_dir))

    if custom_op_library and not custom_op_library.is_file():
        raise FileNotFoundError("Unable to find custom operator library '{}'".format(custom_op_library))

    session_options_config_entries = {}

    if args.nnapi_partitioning_stop_ops is not None:
        session_options_config_entries["ep.nnapi.partitioning_stop_ops"] = args.nnapi_partitioning_stop_ops

    if args.target_platform == 'arm':
        session_options_config_entries["session.qdqisint8allowed"] = "1"
    else:
        session_options_config_entries["session.qdqisint8allowed"] = "0"

    for optimization_style in optimization_styles:
        print("Converting models with optimization style '{}' and level '{}'".format(
            optimization_style.name, optimization_level_str))

        converted_models = _convert(
            model_path_or_dir=model_path_or_dir, output_dir=None,
            optimization_level_str=optimization_level_str, optimization_style=optimization_style,
            custom_op_library=custom_op_library,
            create_optimized_onnx_model=args.save_optimized_onnx_model,
            allow_conversion_failures=args.allow_conversion_failures,
            target_platform=args.target_platform,
            session_options_config_entries=session_options_config_entries)

        with contextlib.ExitStack() as context_stack:
            if optimization_style == OptimizationStyle.Runtime:
                # Convert models again without runtime optimizations.
                # Runtime optimizations may not end up being applied, so we need to use both converted models with and
                # without runtime optimizations to get a complete set of ops that may be needed for the config file.
                model_dir = model_path_or_dir if model_path_or_dir.is_dir() else model_path_or_dir.parent
                temp_output_dir = context_stack.enter_context(
                    tempfile.TemporaryDirectory(dir=model_dir, suffix=".without_runtime_opt"))
                session_options_config_entries_for_second_conversion = session_options_config_entries.copy()
                # Limit the optimizations to those that can run in a model with runtime optimizations.
                session_options_config_entries_for_second_conversion[
                    "optimization.minimal_build_optimizations"] = "apply"

                print("Converting models again without runtime optimizations to generate a complete config file. "
                      "These converted models are temporary and will be deleted.")
                converted_models += _convert(
                    model_path_or_dir=model_path_or_dir, output_dir=temp_output_dir,
                    optimization_level_str=optimization_level_str, optimization_style=OptimizationStyle.Fixed,
                    custom_op_library=custom_op_library,
                    create_optimized_onnx_model=False,  # not useful as they would be created in a temp directory
                    allow_conversion_failures=args.allow_conversion_failures,
                    target_platform=args.target_platform,
                    session_options_config_entries=session_options_config_entries_for_second_conversion)

            print("Generating config file from ORT format models with optimization style '{}' and level '{}'".format(
                optimization_style.name, optimization_level_str))

            config_file = _create_config_file_path(model_path_or_dir, optimization_level_str, optimization_style,
                                                   args.enable_type_reduction)

            create_config_from_models(converted_models, config_file, args.enable_type_reduction)


if __name__ == '__main__':
    convert_onnx_models_to_ort()
