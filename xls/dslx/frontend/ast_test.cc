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

#include "xls/dslx/frontend/ast.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "xls/common/status/matchers.h"

namespace xls::dslx {
namespace {

using status_testing::IsOkAndHolds;
using status_testing::StatusIs;
using testing::HasSubstr;

TEST(AstTest, ModuleWithConstant) {
  Module m("test", /*fs_path=*/std::nullopt);
  const Span fake_span;
  Number* number = m.Make<Number>(fake_span, std::string("42"),
                                  NumberKind::kOther, /*type=*/nullptr);
  NameDef* name_def = m.Make<NameDef>(fake_span, std::string("MOL"), nullptr);
  ConstantDef* constant_def =
      m.Make<ConstantDef>(fake_span, name_def, /*type_annotation=*/nullptr,
                          number, /*is_public=*/false);
  name_def->set_definer(constant_def);
  XLS_ASSERT_OK(m.AddTop(constant_def, /*make_collision_error=*/nullptr));

  EXPECT_EQ(m.ToString(), "const MOL = 42;");
}

TEST(AstTest, GetNumberAsInt64) {
  struct Example {
    std::string text;
    uint64_t want;
  } kCases[] = {
      {"0b0", 0},
      {"0b1", 1},
      {"0b10", 2},
      {"0b11", 3},
      {"0b100", 4},
      {"0b1000", 8},
      {"0b1011", 11},
      {"0b1_1000", 24},
      {"0b1_1001", 25},
      {"0b1111_1111_1111_1111_1111_1111_1111_1111_1111_1111_1111_1111_1111_"
       "1111_1111_1111",
       static_cast<uint64_t>(-1)},
      {"-1", static_cast<uint64_t>(-1)},
  };
  Module m("test", /*fs_path=*/std::nullopt);
  auto make_num = [&m](std::string text) {
    const Span fake_span;
    return m.Make<Number>(fake_span, text, NumberKind::kOther,
                          /*type=*/nullptr);
  };
  for (const Example& example : kCases) {
    EXPECT_THAT(make_num(example.text)->GetAsUint64(),
                IsOkAndHolds(example.want));
  }

  EXPECT_THAT(make_num("0b")->GetAsUint64(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Could not convert 0b to a number")));
}

TEST(AstTest, CharacterNumberToStringTest) {
  struct Example {
    std::string text;
    std::string expected;
  } kCases[] = {
      {R"(4)", R"('4')"},  {R"(2)", R"('2')"},  {R"(X)", R"('X')"},
      {R"(l)", R"('l')"},  {R"(S)", R"('S')"},  {R"(")", R"('"')"},
      {R"(')", R"('\'')"}, {R"(\)", R"('\\')"},
  };
  Module m("test", /*fs_path=*/std::nullopt);
  auto make_char_num = [&m](std::string text) {
    const Span fake_span;
    return m.Make<Number>(fake_span, text, NumberKind::kCharacter,
                          /*type=*/nullptr);
  };
  for (const Example& example : kCases) {
    EXPECT_THAT(make_char_num(example.text)->ToString(), example.expected);
  }
}

TEST(AstTest, GetBuiltinTypeSignedness) {
  XLS_ASSERT_OK_AND_ASSIGN(bool is_signed,
                           GetBuiltinTypeSignedness(BuiltinType::kBool));
  EXPECT_FALSE(is_signed);
  XLS_ASSERT_OK_AND_ASSIGN(is_signed,
                           GetBuiltinTypeSignedness(BuiltinType::kS1));
  EXPECT_TRUE(is_signed);
  XLS_ASSERT_OK_AND_ASSIGN(is_signed,
                           GetBuiltinTypeSignedness(BuiltinType::kU1));
  EXPECT_FALSE(is_signed);
  XLS_ASSERT_OK_AND_ASSIGN(is_signed,
                           GetBuiltinTypeSignedness(BuiltinType::kSN));
  EXPECT_TRUE(is_signed);
  XLS_ASSERT_OK_AND_ASSIGN(is_signed,
                           GetBuiltinTypeSignedness(BuiltinType::kUN));
  EXPECT_FALSE(is_signed);
  XLS_ASSERT_OK_AND_ASSIGN(is_signed,
                           GetBuiltinTypeSignedness(BuiltinType::kBits));
  EXPECT_FALSE(is_signed);
  XLS_ASSERT_OK_AND_ASSIGN(is_signed,
                           GetBuiltinTypeSignedness(BuiltinType::kToken));
  EXPECT_FALSE(is_signed);
}

TEST(AstTest, GetBuiltinTypeBitCount) {
  XLS_ASSERT_OK_AND_ASSIGN(int64_t bit_count,
                           GetBuiltinTypeBitCount(BuiltinType::kBool));
  EXPECT_EQ(bit_count, 1);
  XLS_ASSERT_OK_AND_ASSIGN(bit_count, GetBuiltinTypeBitCount(BuiltinType::kS1));
  EXPECT_EQ(bit_count, 1);
  XLS_ASSERT_OK_AND_ASSIGN(bit_count,
                           GetBuiltinTypeBitCount(BuiltinType::kS64));
  EXPECT_EQ(bit_count, 64);
  XLS_ASSERT_OK_AND_ASSIGN(bit_count, GetBuiltinTypeBitCount(BuiltinType::kU1));
  EXPECT_EQ(bit_count, 1);
  XLS_ASSERT_OK_AND_ASSIGN(bit_count,
                           GetBuiltinTypeBitCount(BuiltinType::kU64));
  EXPECT_EQ(bit_count, 64);
  XLS_ASSERT_OK_AND_ASSIGN(bit_count, GetBuiltinTypeBitCount(BuiltinType::kSN));
  EXPECT_EQ(bit_count, 0);
  XLS_ASSERT_OK_AND_ASSIGN(bit_count, GetBuiltinTypeBitCount(BuiltinType::kUN));
  EXPECT_EQ(bit_count, 0);
  XLS_ASSERT_OK_AND_ASSIGN(bit_count,
                           GetBuiltinTypeBitCount(BuiltinType::kBits));
  EXPECT_EQ(bit_count, 0);
  XLS_ASSERT_OK_AND_ASSIGN(bit_count,
                           GetBuiltinTypeBitCount(BuiltinType::kToken));
  EXPECT_EQ(bit_count, 0);
}

// We have to parenthesize the LHS to avoid ambiguity that the RHS of the cast
// might be a parametric type we're instantiating.
TEST(AstTest, ToStringCastWithinLtComparison) {
  Module m("test", /*fs_path=*/std::nullopt);
  const Span fake_span;
  BuiltinNameDef* x_def = m.GetOrCreateBuiltinNameDef("x");
  NameRef* x_ref = m.Make<NameRef>(fake_span, "x", x_def);

  BuiltinTypeAnnotation* builtin_u32 = m.Make<BuiltinTypeAnnotation>(
      fake_span, BuiltinType::kU32, m.GetOrCreateBuiltinNameDef("u32"));

  // type t = u32;
  NameDef* t_def = m.Make<NameDef>(fake_span, "t", /*definer=*/nullptr);
  TypeAlias* type_alias =
      m.Make<TypeAlias>(fake_span, t_def, builtin_u32, /*is_public=*/false);
  t_def->set_definer(type_alias);

  TypeRef* type_ref = m.Make<TypeRef>(fake_span, type_alias);

  auto* type_ref_type_annotation = m.Make<TypeRefTypeAnnotation>(
      fake_span, type_ref, /*parametrics=*/std::vector<ExprOrType>{});

  // x as t < x
  Cast* cast = m.Make<Cast>(fake_span, x_ref, type_ref_type_annotation);
  Binop* lt = m.Make<Binop>(fake_span, BinopKind::kLt, cast, x_ref);

  EXPECT_EQ(lt->ToString(), "(x as t) < x");
}

}  // namespace
}  // namespace xls::dslx
