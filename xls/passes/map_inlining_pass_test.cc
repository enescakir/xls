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

#include "xls/passes/map_inlining_pass.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_parser.h"

namespace xls {
namespace {

namespace m = ::xls::op_matchers;

// "Smoke" test for a basic map transform.
TEST(MapInliningPassTest, BasicOperation) {
  const char kPackage[] = R"(
package p

fn map_fn(x: bits[32]) -> bits[16] {
  ret bit_slice.1: bits[16] = bit_slice(x, start=0, width=16)
}

fn main() -> bits[16][4] {
  literal_1: bits[32] = literal(value=0x123)
  literal_2: bits[32] = literal(value=0x456)
  literal_3: bits[32] = literal(value=0x789)
  literal_4: bits[32] = literal(value=0xabc)
  array_1: bits[32][4] = array(literal_1, literal_2, literal_3, literal_4)
  ret result: bits[16][4] = map(array_1, to_apply=map_fn)
}

)";

  XLS_ASSERT_OK_AND_ASSIGN(auto package, Parser::ParsePackage(kPackage));
  XLS_ASSERT_OK_AND_ASSIGN(auto func, package->GetFunction("main"));
  MapInliningPass pass;
  PassOptions options;
  XLS_ASSERT_OK_AND_ASSIGN(bool changed,
                           pass.RunOnFunctionBase(func, options, nullptr));
  ASSERT_TRUE(changed);
  EXPECT_THAT(func->return_value(),
              m::Array(m::Invoke(m::ArrayIndex(m::Array(), m::Literal(0))),
                       m::Invoke(m::ArrayIndex(m::Array(), m::Literal(1))),
                       m::Invoke(m::ArrayIndex(m::Array(), m::Literal(2))),
                       m::Invoke(m::ArrayIndex(m::Array(), m::Literal(3)))));

  XLS_VLOG(1) << package->DumpIr();
}

TEST(MapInliningPass, InputArrayOrLiteral) {
  const char kPackage[] = R"(
package p

fn map_fn(x: bits[32]) -> bits[16] {
  ret bit_slice.1: bits[16] = bit_slice(x, start=0, width=16)
}

fn main(a: bits[32][4]) -> bits[16][4] {
  ret result: bits[16][4] = map(a, to_apply=map_fn)
}
)";

  XLS_ASSERT_OK_AND_ASSIGN(auto package, Parser::ParsePackage(kPackage));
  XLS_ASSERT_OK_AND_ASSIGN(auto func, package->GetFunction("main"));
  MapInliningPass pass;
  PassOptions options;
  XLS_ASSERT_OK_AND_ASSIGN(bool changed,
                           pass.RunOnFunctionBase(func, options, nullptr));
  ASSERT_TRUE(changed);

  EXPECT_THAT(func->return_value(),
              m::Array(m::Invoke(m::ArrayIndex(m::Param(), m::Literal(0))),
                       m::Invoke(m::ArrayIndex(m::Param(), m::Literal(1))),
                       m::Invoke(m::ArrayIndex(m::Param(), m::Literal(2))),
                       m::Invoke(m::ArrayIndex(m::Param(), m::Literal(3)))));
}

}  // namespace
}  // namespace xls
