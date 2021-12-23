# Copyright 2021 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
This module contains codegen-related build rules for XLS.
"""

load("@bazel_skylib//lib:dicts.bzl", "dicts")
load("//xls/build_rules:xls_common_rules.bzl", "get_args")
load("//xls/build_rules:xls_config_rules.bzl", "CONFIG")
load("//xls/build_rules:xls_providers.bzl", "CodegenInfo")
load("//xls/build_rules:xls_ir_rules.bzl", "xls_ir_common_attrs")
load(
    "//xls/build_rules:xls_toolchains.bzl",
    "get_xls_toolchain_info",
    "xls_toolchain_attr",
)

_DEFAULT_CODEGEN_ARGS = {
    "delay_model": "unit",
}

_VERILOG_FILE_EXTENSION = ".v"
_SIGNATURE_TEXTPROTO_FILE_EXTENSION = ".sig.textproto"
_SCHEDULE_TEXTPROTO_FILE_EXTENSION = ".schedule.textproto"

xls_ir_verilog_attrs = {
    "codegen_args": attr.string_dict(
        doc = "Arguments of the codegen tool. For details on the arguments, " +
              "refer to the codegen_main application at " +
              "//xls/tools/codegen_main.cc.",
    ),
    "verilog_file": attr.output(
        doc = "The Verilog file generated.",
    ),
    "module_sig_file": attr.output(
        doc = "The module signature of the generated Verilog file.",
    ),
    "schedule_file": attr.output(
        doc = "The schedule of the generated Verilog file.",
    ),
}

def _is_combinational_generator(arguments):
    """Returns True, if "generator" is "combinational". Otherwise, returns False.

    Args:
      arguments: The list of arguments.
    Returns:
      Returns True, if "generator" is "combinational". Otherwise, returns False.
    """
    return arguments.get("generator", "") == "combinational"

def get_xls_ir_verilog_generated_files(basename, arguments):
    """Returns a list of filenames generated by the 'xls_ir_verilog' rule.

    Args:
      basename: The file basename.
      arguments: The codegen arguments.

    Returns:
      Returns a list of filenames generated by the 'xls_ir_verilog' rule.
    """
    generated_files = [
        basename + _VERILOG_FILE_EXTENSION,
        basename + _SIGNATURE_TEXTPROTO_FILE_EXTENSION,
    ]
    if not _is_combinational_generator(arguments):
        generated_files.append(basename + _SCHEDULE_TEXTPROTO_FILE_EXTENSION)
    return generated_files

def xls_ir_verilog_impl(ctx, src):
    """The core implementation of the 'xls_ir_verilog' rule.

    Generates a Verilog file, module signature file and schedule file.

    Args:
      ctx: The current rule's context object.
      src: The source file.
    Returns:
      CodegenInfo provider
      DefaultInfo provider
    """
    codegen_tool = get_xls_toolchain_info(ctx).codegen_tool
    my_generated_files = []

    # default arguments
    codegen_default_args = _DEFAULT_CODEGEN_ARGS
    codegen_args = ctx.attr.codegen_args

    # parse arguments
    CODEGEN_FLAGS = (
        "clock_period_ps",
        "pipeline_stages",
        "delay_model",
        "entry",
        "top_level_proc",
        "generator",
        "input_valid_signal",
        "output_valid_signal",
        "manual_load_enable_signal",
        "flop_inputs",
        "flop_outputs",
        "module_name",
        "clock_margin_percent",
        "reset",
        "reset_active_low",
        "reset_asynchronous",
        "use_system_verilog",
        "streaming_channel_data_suffix",
        "streaming_channel_ready_suffix",
        "streaming_channel_valid_suffix",
    )

    my_args = get_args(codegen_args, CODEGEN_FLAGS, codegen_default_args)
    uses_combinational_generator = _is_combinational_generator(codegen_args)

    schedule_file = None
    if not uses_combinational_generator:
        # Pipeline generator produces a schedule artifact.
        schedule_file = ctx.actions.declare_file(
            ctx.attr.name + _SCHEDULE_TEXTPROTO_FILE_EXTENSION,
        )
        my_generated_files.append(schedule_file)
        my_args += " --output_schedule_path={}".format(schedule_file.path)
    verilog_file = ctx.actions.declare_file(
        ctx.attr.name + _VERILOG_FILE_EXTENSION,
    )
    module_sig_file = ctx.actions.declare_file(
        ctx.attr.name + _SIGNATURE_TEXTPROTO_FILE_EXTENSION,
    )
    my_generated_files += [verilog_file, module_sig_file]
    my_args += " --output_verilog_path={}".format(verilog_file.path)
    my_args += " --output_signature_path={}".format(module_sig_file.path)

    ctx.actions.run_shell(
        outputs = my_generated_files,
        tools = [codegen_tool],
        inputs = [src, codegen_tool],
        command = "{} {} {}".format(
            codegen_tool.path,
            src.path,
            my_args,
        ),
        mnemonic = "Codegen",
        progress_message = "Building Verilog file: %s" % (verilog_file.path),
    )
    return [
        CodegenInfo(
            verilog_file = verilog_file,
            module_sig_file = module_sig_file,
            schedule_file = schedule_file,
        ),
        DefaultInfo(
            files = depset(my_generated_files),
        ),
    ]

def _xls_ir_verilog_impl_wrapper(ctx):
    """The implementation of the 'xls_ir_verilog' rule.

    Wrapper for xls_ir_verilog_impl. See: xls_ir_verilog_impl.

    Args:
      ctx: The current rule's context object.
    Returns:
      See: codegen_impl.
    """
    return xls_ir_verilog_impl(ctx, ctx.file.src)

xls_ir_verilog = rule(
    doc = """A build rule that generates a Verilog file.

        Examples:

        1) A file as the source.

        ```
            xls_ir_verilog(
                name = "a_verilog",
                src = "a.ir",
                codegen_args = {
                    "pipeline_stages": "1",
                    ...
                },
            )
        ```

        2) A target as the source.

        ```
            xls_ir_opt_ir(
                name = "a_opt_ir",
                src = "a.ir",
            )

            xls_ir_verilog(
                name = "a_verilog",
                src = ":a_opt_ir",
                codegen_args = {
                    "generator": "combinational",
                    ...
                },
            )
        ```
    """,
    implementation = _xls_ir_verilog_impl_wrapper,
    attrs = dicts.add(
        xls_ir_common_attrs,
        xls_ir_verilog_attrs,
        CONFIG["xls_outs_attrs"],
        xls_toolchain_attr,
    ),
)
