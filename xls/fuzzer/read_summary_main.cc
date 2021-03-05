// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/init_xls.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/status_macros.h"
#include "xls/fuzzer/sample_summary.pb.h"
#include "xls/ir/op.h"

const char kUsage[] = R"(
Reads Protobuf summary files emitted by the fuzzer and presents the data in
tabular form. The summary information includes information about each IR sample
generated by the fuzzer including op types, widths, etc. This information gives
an indication of what kind of IR operations are being covered by the
fuzzer. Usage:

  read_summary_main  [SUMMARY_FILE...]

Example invocations:

Show summary of a set of files emitted by the fuzzer:

  read_summary_main /tmp/summaries/summary_*.binarypb
)";

namespace xls {
namespace {

// Aggregate info about a particular IR op (e.g., 'array_updage').
struct OpInfo {
  // Count of the number of instances of this op.
  int64 samples = 0;

  // Count of operations by type ("bits", "array", or "tuple").
  absl::flat_hash_map<std::string, int64> by_type;

  // Count of operations wider than 64 bits.
  int64 wider_than_64bits = 0;

  // Count of operations for which the operands are different widths.
  int64 mixed_width = 0;

  // Count of operations with different arities.
  int64 nullary = 0;
  int64 unary = 0;
  int64 binary = 0;
  int64 manyary = 0;
};

// Aggregate information about a set of generated samples.
struct SampleInfo {
  int64 samples = 0;
  int64 node_count = 0;
  absl::flat_hash_map<std::string, OpInfo> per_op_info;
};

// Aggregate data about all the information in the summary file.
struct SummaryInfo {
  SampleInfo unoptimized_info;
  SampleInfo optimized_info;
  // The breakdown of total time spent in the fuzzer for the various operations
  // (e.g, generating the same, optimizing, JIT time, etc)..
  fuzzer::SampleTimingProto total_timing;
  // The maximum time spent on a single same for the various fuzzer operations.
  fuzzer::SampleTimingProto max_timing;
};

// Aggregates the summary data in 'summary' into 'info'.
void AggregateSummary(const fuzzer::SampleSummaryProto& summary,
                      SummaryInfo* info) {
  for (bool optimized : {false, true}) {
    SampleInfo& sample_info =
        optimized ? info->optimized_info : info->unoptimized_info;
    const auto& nodes =
        optimized ? summary.optimized_nodes() : summary.unoptimized_nodes();
    sample_info.samples++;
    for (const fuzzer::NodeProto& node_proto : nodes) {
      sample_info.node_count++;
      OpInfo& op_info = sample_info.per_op_info[node_proto.op()];
      op_info.samples++;
      op_info.by_type[node_proto.type()]++;
      if (node_proto.width() > 64) {
        op_info.wider_than_64bits++;
      }
      switch (node_proto.operands_size()) {
        case 0:
          op_info.nullary++;
          break;
        case 1:
          op_info.unary++;
          break;
        case 2:
          op_info.binary++;
          break;
        default:
          op_info.manyary++;
          break;
      }
      for (const fuzzer::NodeProto& operand : node_proto.operands()) {
        if (operand.width() != node_proto.operands(0).width()) {
          op_info.mixed_width++;
          break;
        }
      }
    }
  }

  // Aggregate timing info including total and maximum times.
#define AGGREGATE_FIELD(F)                                                     \
  {                                                                            \
    info->total_timing.set_##F(info->total_timing.F() + summary.timing().F()); \
    info->max_timing.set_##F(                                                  \
        std::max(info->max_timing.F(), summary.timing().F()));                 \
  }
  AGGREGATE_FIELD(total_ns);
  AGGREGATE_FIELD(generate_sample_ns);
  AGGREGATE_FIELD(interpret_dslx_ns);
  AGGREGATE_FIELD(convert_ir_ns);
  AGGREGATE_FIELD(unoptimized_interpret_ir_ns);
  AGGREGATE_FIELD(unoptimized_jit_ns);
  AGGREGATE_FIELD(optimize_ns);
  AGGREGATE_FIELD(optimized_interpret_ir_ns);
  AGGREGATE_FIELD(optimized_jit_ns);
  AGGREGATE_FIELD(codegen_ns);
  AGGREGATE_FIELD(simulate_ns);
#undef AGGREGATE_FIELD
}

// Print the timing info contained in 'info' to stdout.
void DumpTimingInfo(const SummaryInfo& info) {
  // Converts nanoseconds to seconds.
  auto us_to_sec = [](int64 nanoseconds) {
    return static_cast<float>(nanoseconds) / 1e9;
  };

  // Returns the percentage value of num/denom.
  auto percent = [&](int64 num, int64 denom) {
    return denom == 0 ? 0.0f : 100.0f * num / denom;
  };

  // Returns the mean value equal to total / count.
  auto mean = [&](int64 total, int64 count) {
    return count == 0 ? 0.0f : static_cast<float>(total) / count;
  };

  std::cout << absl::StreamFormat("Samples (unoptimized): %d\n",
                                  info.unoptimized_info.samples);
  std::cout << absl::StreamFormat(
      "Mean size (unoptimized): %.1f nodes\n",
      mean(info.unoptimized_info.node_count, info.unoptimized_info.samples));

  std::cout << absl::StreamFormat("Samples (optimized): %d\n",
                                  info.optimized_info.samples);
  std::cout << absl::StreamFormat(
      "Mean size (optimized): %.1f nodes\n",
      mean(info.optimized_info.node_count, info.optimized_info.samples));

  std::cout << absl::StreamFormat("Total time: %0.3fs\n",
                                  us_to_sec(info.total_timing.total_ns()));
  std::cout << absl::StreamFormat(
      "Mean time:   %0.3fs\n", us_to_sec(mean(info.total_timing.total_ns(),
                                              info.unoptimized_info.samples)));
  std::cout << absl::StreamFormat("Max time:   %0.3fs\n",
                                  us_to_sec(info.max_timing.total_ns()));
  std::cout << "\nBreakdown:\n";
#define PRINT_ROW(F)                                                         \
  std::cout << absl::StreamFormat(                                           \
      "%-30s %10.3fs (%4.1f%%), mean %5.3fs, max %6.3fs\n", #F,              \
      us_to_sec(info.total_timing.F()),                                      \
      percent(info.total_timing.F(), info.total_timing.total_ns()),          \
      us_to_sec(mean(info.total_timing.F(), info.unoptimized_info.samples)), \
      us_to_sec(info.max_timing.F()));
  PRINT_ROW(generate_sample_ns);
  PRINT_ROW(interpret_dslx_ns);
  PRINT_ROW(convert_ir_ns);
  PRINT_ROW(unoptimized_interpret_ir_ns);
  PRINT_ROW(unoptimized_jit_ns);
  PRINT_ROW(optimize_ns);
  PRINT_ROW(optimized_interpret_ir_ns);
  PRINT_ROW(optimized_jit_ns);
  PRINT_ROW(codegen_ns);
  PRINT_ROW(simulate_ns);
#undef PRINT_ROW
}

// Dumps aggregate information about the generated samples described in 'info'
// to stdout.
void DumpSampleInfo(const SampleInfo& info) {
  auto fmt = [&](const std::string& s, bool first_col = false) {
    if (first_col) {
      return absl::StrFormat("%-20s", s);
    } else {
      return absl::StrFormat("%13s", s);
    }
  };
  auto fmt_num = [&](int64 n) { return fmt(absl::StrCat(n), false); };

  std::vector<std::string> fields{"op",    "count",    "bits",        "tuple",
                                  "array", ">64-bits", "mixed width", "nullary",
                                  "unary", "binary",   ">=3ary"};
  for (int64 i = 0; i < fields.size(); ++i) {
    std::cout << fmt(fields[i], /*first_col=*/i == 0);
  }
  std::cout << "\n" << std::string(20 + 13 * (fields.size() - 1), '-') << "\n";
  for (Op op : AllOps()) {
    std::string op_str = OpToString(op);
    OpInfo op_info = info.per_op_info.contains(op_str)
                         ? info.per_op_info.at(op_str)
                         : OpInfo{0};
    std::cout << fmt(op_str, /*first_col=*/true);
    std::cout << fmt_num(op_info.samples);
    std::cout << fmt_num(op_info.by_type["bits"]);
    std::cout << fmt_num(op_info.by_type["tuple"]);
    std::cout << fmt_num(op_info.by_type["array"]);
    std::cout << fmt_num(op_info.wider_than_64bits);
    std::cout << fmt_num(op_info.mixed_width);
    std::cout << fmt_num(op_info.nullary);
    std::cout << fmt_num(op_info.unary);
    std::cout << fmt_num(op_info.binary);
    std::cout << fmt_num(op_info.manyary);
    std::cout << "\n";
  }
}

absl::Status RealMain(absl::Span<const absl::string_view> input_paths) {
  SummaryInfo summary_info;
  for (const absl::string_view input_path : input_paths) {
    XLS_ASSIGN_OR_RETURN(std::string summary_data, GetFileContents(input_path));
    fuzzer::SampleSummariesProto summaries;
    if (!summaries.ParseFromString(summary_data)) {
      return absl::InvalidArgumentError(
          "Failed to parse summary protobuf file.");
    }
    for (const fuzzer::SampleSummaryProto& summary : summaries.samples()) {
      AggregateSummary(summary, &summary_info);
    }
  }

  std::cout << "Before optimizations:\n";
  std::cout << "--------------------\n";
  DumpSampleInfo(summary_info.unoptimized_info);

  std::cout << "\nAfter optimizations\n";
  std::cout << "-------------------\n";
  DumpSampleInfo(summary_info.optimized_info);

  std::cout << "\nTiming\n";
  std::cout << "------\n";
  DumpTimingInfo(summary_info);
  return absl::OkStatus();
}

}  // namespace
}  // namespace xls

int main(int argc, char** argv) {
  std::vector<absl::string_view> positional_arguments =
      xls::InitXls(kUsage, argc, argv);

  if (positional_arguments.empty()) {
    XLS_LOG(QFATAL) << absl::StreamFormat(
        "Expected invocation: %s [SUMMARY_FILE...]", argv[0]);
  }

  XLS_QCHECK_OK(xls::RealMain(positional_arguments));
  return EXIT_SUCCESS;
}
