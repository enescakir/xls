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

#include <algorithm>
#include <climits>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/meta/type_traits.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "xls/common/indent.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/ir/bits.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/number_parser.h"

namespace xls::dslx {
namespace {

constexpr std::string_view kRustOneIndent = "    ";

class DfsIteratorNoTypes {
 public:
  explicit DfsIteratorNoTypes(const AstNode* start) : to_visit_({start}) {}

  bool HasNext() const { return !to_visit_.empty(); }

  const AstNode* Next() {
    const AstNode* result = to_visit_.front();
    to_visit_.pop_front();
    std::vector<AstNode*> children = result->GetChildren(/*want_types=*/false);
    std::reverse(children.begin(), children.end());
    for (AstNode* c : children) {
      to_visit_.push_front(c);
    }
    return result;
  }

 private:
  std::deque<const AstNode*> to_visit_;
};

static AnyNameDef GetSubjectNameDef(const ColonRef::Subject& subject) {
  return absl::visit(
      Visitor{
          [](NameRef* n) { return n->name_def(); },
          [](ColonRef* n) { return GetSubjectNameDef(n->subject()); },
      },
      subject);
}

void Parenthesize(std::string* s) { *s = absl::StrCat("(", *s, ")"); }

}  // namespace

std::string_view PrecedenceToString(Precedence p) {
  switch (p) {
    case Precedence::kStrongest:
      return "strongest";
    case Precedence::kPaths:
      return "paths";
    case Precedence::kMethodCall:
      return "method-call";

    case Precedence::kFieldExpression:
      return "field-expression";
    case Precedence::kFunctionCallOrArrayIndex:
      return "function-call-or-array-index";
    case Precedence::kQuestionMark:
      return "question-mark";
    case Precedence::kUnaryOp:
      return "unary";
    case Precedence::kAs:
      return "as";
    case Precedence::kStrongArithmetic:
      return "strong-arithmetic";
    case Precedence::kWeakArithmetic:
      return "weak-arithmetic";
    case Precedence::kShift:
      return "shift";

    case Precedence::kConcat:
      return "concat";

    case Precedence::kBitwiseAnd:
      return "bitwise-and";
    case Precedence::kBitwiseXor:
      return "bitwise-xor";
    case Precedence::kBitwiseOr:
      return "bitwise-or";
    case Precedence::kComparison:
      return "comparison";
    case Precedence::kLogicalAnd:
      return "logical-and";
    case Precedence::kLogicalOr:
      return "logical-or";
    case Precedence::kRange:
      return "range";
    case Precedence::kEquals:
      return "equals";
    case Precedence::kReturn:
      return "return";

    case Precedence::kWeakest:
      return "weakest";
  }

  XLS_LOG(FATAL) << "Invalid precedence value: " << static_cast<int>(p);
}

constexpr int64_t kTargetLineChars = 80;

ExprOrType ToExprOrType(AstNode* n) {
  if (Expr* e = down_cast<Expr*>(n)) {
    return e;
  }
  auto* type = down_cast<TypeAnnotation*>(n);
  XLS_CHECK_NE(type, nullptr);
  return type;
}

std::string_view AstNodeKindToString(AstNodeKind kind) {
  switch (kind) {
    case AstNodeKind::kConstAssert:
      return "const assert";
    case AstNodeKind::kStatement:
      return "statement";
    case AstNodeKind::kTypeAnnotation:
      return "type annotation";
    case AstNodeKind::kModule:
      return "module";
    case AstNodeKind::kNameDef:
      return "name definition";
    case AstNodeKind::kBuiltinNameDef:
      return "builtin name definition";
    case AstNodeKind::kConditional:
      return "conditional";
    case AstNodeKind::kTypeAlias:
      return "type alias";
    case AstNodeKind::kNumber:
      return "number";
    case AstNodeKind::kTypeRef:
      return "type reference";
    case AstNodeKind::kImport:
      return "import";
    case AstNodeKind::kUnop:
      return "unary op";
    case AstNodeKind::kBinop:
      return "binary op";
    case AstNodeKind::kColonRef:
      return "colon reference";
    case AstNodeKind::kParam:
      return "parameter";
    case AstNodeKind::kFunction:
      return "function";
    case AstNodeKind::kProc:
      return "proc";
    case AstNodeKind::kProcMember:
      return "proc member";
    case AstNodeKind::kNameRef:
      return "name reference";
    case AstNodeKind::kConstRef:
      return "const reference";
    case AstNodeKind::kArray:
      return "array";
    case AstNodeKind::kString:
      return "string";
    case AstNodeKind::kStructInstance:
      return "struct instance";
    case AstNodeKind::kSplatStructInstance:
      return "splat struct instance";
    case AstNodeKind::kNameDefTree:
      return "name definition tree";
    case AstNodeKind::kIndex:
      return "index";
    case AstNodeKind::kRange:
      return "range";
    case AstNodeKind::kRecv:
      return "receive";
    case AstNodeKind::kRecvNonBlocking:
      return "receive-non-blocking";
    case AstNodeKind::kRecvIf:
      return "receive-if";
    case AstNodeKind::kRecvIfNonBlocking:
      return "receive-if-non-blocking";
    case AstNodeKind::kSend:
      return "send";
    case AstNodeKind::kSendIf:
      return "send-if";
    case AstNodeKind::kJoin:
      return "join";
    case AstNodeKind::kTestFunction:
      return "test function";
    case AstNodeKind::kTestProc:
      return "test proc";
    case AstNodeKind::kWidthSlice:
      return "width slice";
    case AstNodeKind::kWildcardPattern:
      return "wildcard pattern";
    case AstNodeKind::kMatchArm:
      return "match arm";
    case AstNodeKind::kMatch:
      return "match";
    case AstNodeKind::kAttr:
      return "attribute";
    case AstNodeKind::kInstantiation:
      return "instantiation";
    case AstNodeKind::kInvocation:
      return "invocation";
    case AstNodeKind::kSpawn:
      return "spawn";
    case AstNodeKind::kFormatMacro:
      return "format macro";
    case AstNodeKind::kZeroMacro:
      return "zero macro";
    case AstNodeKind::kSlice:
      return "slice";
    case AstNodeKind::kEnumDef:
      return "enum definition";
    case AstNodeKind::kStructDef:
      return "struct definition";
    case AstNodeKind::kQuickCheck:
      return "quick-check";
    case AstNodeKind::kXlsTuple:
      return "tuple";
    case AstNodeKind::kFor:
      return "for";
    case AstNodeKind::kBlock:
      return "block";
    case AstNodeKind::kCast:
      return "cast";
    case AstNodeKind::kConstantDef:
      return "constant definition";
    case AstNodeKind::kLet:
      return "let";
    case AstNodeKind::kChannelDecl:
      return "channel declaration";
    case AstNodeKind::kParametricBinding:
      return "parametric binding";
    case AstNodeKind::kTupleIndex:
      return "tuple index";
    case AstNodeKind::kUnrollFor:
      return "unroll-for";
  }
  XLS_LOG(FATAL) << "Out-of-range AstNodeKind: " << static_cast<int>(kind);
}

AnyNameDef TypeDefinitionGetNameDef(const TypeDefinition& td) {
  return absl::visit(
      Visitor{
          [](TypeAlias* n) -> AnyNameDef { return n->name_def(); },
          [](StructDef* n) -> AnyNameDef { return n->name_def(); },
          [](EnumDef* n) -> AnyNameDef { return n->name_def(); },
          [](ColonRef* n) -> AnyNameDef {
            return GetSubjectNameDef(n->subject());
          },
      },
      td);
}

absl::StatusOr<TypeDefinition> ToTypeDefinition(AstNode* node) {
  if (auto* n = dynamic_cast<TypeAlias*>(node)) {
    return TypeDefinition(n);
  }
  if (auto* n = dynamic_cast<StructDef*>(node)) {
    return TypeDefinition(n);
  }
  if (auto* n = dynamic_cast<EnumDef*>(node)) {
    return TypeDefinition(n);
  }
  if (auto* n = dynamic_cast<ColonRef*>(node)) {
    return TypeDefinition(n);
  }
  return absl::InvalidArgumentError(
      absl::StrFormat("AST node is not a type definition: (%s) %s",
                      node->GetNodeTypeName(), node->ToString()));
}

FreeVariables FreeVariables::DropBuiltinDefs() const {
  FreeVariables result;
  for (const auto& [identifier, name_refs] : values_) {
    for (const NameRef* ref : name_refs) {
      auto def = ref->name_def();
      if (std::holds_alternative<BuiltinNameDef*>(def)) {
        continue;
      }
      result.Add(identifier, ref);
    }
  }
  return result;
}

std::vector<std::pair<std::string, AnyNameDef>>
FreeVariables::GetNameDefTuples() const {
  std::vector<std::pair<std::string, AnyNameDef>> result;
  for (const auto& item : values_) {
    const NameRef* ref = item.second[0];
    result.push_back({item.first, ref->name_def()});
  }
  std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.first < rhs.first;
  });
  return result;
}

std::vector<const ConstRef*> FreeVariables::GetConstRefs() {
  std::vector<const ConstRef*> const_refs;
  for (const auto& [name, refs] : values_) {
    for (const NameRef* name_ref : refs) {
      if (auto* const_ref = dynamic_cast<const ConstRef*>(name_ref)) {
        const_refs.push_back(const_ref);
      }
    }
  }
  return const_refs;
}

std::vector<AnyNameDef> FreeVariables::GetNameDefs() const {
  std::vector<AnyNameDef> result;
  for (auto& pair : GetNameDefTuples()) {
    result.push_back(pair.second);
  }
  return result;
}

void FreeVariables::Add(std::string identifier, const NameRef* name_ref) {
  auto it = values_.insert({identifier, {name_ref}});
  if (!it.second) {
    it.first->second.push_back(name_ref);
  }
}

absl::flat_hash_set<std::string> FreeVariables::Keys() const {
  absl::flat_hash_set<std::string> result;
  for (const auto& item : values_) {
    result.insert(item.first);
  }
  return result;
}

FreeVariables GetFreeVariables(const AstNode* node, const Pos* start_pos) {
  DfsIteratorNoTypes it(node);
  FreeVariables freevars;
  while (it.HasNext()) {
    const AstNode* n = it.Next();
    if (const auto* name_ref = dynamic_cast<const NameRef*>(n)) {
      // If a start position was given we test whether the name definition
      // occurs before that start position. (If none was given we accept all
      // name refs.)
      if (start_pos == nullptr) {
        freevars.Add(name_ref->identifier(), name_ref);
      } else {
        std::optional<Pos> name_def_start = name_ref->GetNameDefStart();
        if (!name_def_start.has_value() || *name_def_start < *start_pos) {
          freevars.Add(name_ref->identifier(), name_ref);
        }
      }
    }
  }
  return freevars;
}

std::string BuiltinTypeToString(BuiltinType t) {
  switch (t) {
#define CASE(__enum, B, __str, ...) \
  case BuiltinType::__enum:         \
    return __str;
    XLS_DSLX_BUILTIN_TYPE_EACH(CASE)
#undef CASE
  }
  return absl::StrFormat("<invalid BuiltinType(%d)>", static_cast<int>(t));
}

absl::StatusOr<BuiltinType> GetBuiltinType(bool is_signed, int64_t width) {
#define TEST(__enum, __name, __str, __signedness, __width) \
  if (__signedness == is_signed && __width == width) {     \
    return BuiltinType::__enum;                            \
  }
  XLS_DSLX_BUILTIN_TYPE_EACH(TEST)
#undef TEST
  return absl::NotFoundError(
      absl::StrFormat("Cannot find built in type with signedness: %d width: %d",
                      is_signed, width));
}

absl::StatusOr<bool> GetBuiltinTypeSignedness(BuiltinType type) {
  switch (type) {
#define CASE(__enum, _unused1, _unused2, __signedness, _unused3) \
  case BuiltinType::__enum:                                      \
    return __signedness;
    XLS_DSLX_BUILTIN_TYPE_EACH(CASE)
#undef CASE
  }
  return absl::InvalidArgumentError(
      absl::StrCat("Unknown builtin type: ", static_cast<int64_t>(type)));
}

absl::StatusOr<int64_t> GetBuiltinTypeBitCount(BuiltinType type) {
  switch (type) {
#define CASE(__enum, _unused1, _unused2, _unused3, __width) \
  case BuiltinType::__enum:                                 \
    return __width;
    XLS_DSLX_BUILTIN_TYPE_EACH(CASE)
#undef CASE
  }
  return absl::InvalidArgumentError(
      absl::StrCat("Unknown builtin type: ", static_cast<int64_t>(type)));
}

absl::StatusOr<BuiltinType> BuiltinTypeFromString(std::string_view s) {
#define CASE(__enum, __unused, __str, ...) \
  if (s == __str) {                        \
    return BuiltinType::__enum;            \
  }
  XLS_DSLX_BUILTIN_TYPE_EACH(CASE)
#undef CASE
  return absl::InvalidArgumentError(
      absl::StrFormat("String is not a BuiltinType: \"%s\"", s));
}

const absl::btree_set<BinopKind>& GetBinopSameTypeKinds() {
  static auto* singleton = new absl::btree_set<BinopKind>{
      BinopKind::kAdd, BinopKind::kSub, BinopKind::kMul, BinopKind::kAnd,
      BinopKind::kOr,  BinopKind::kXor, BinopKind::kDiv,
  };
  return *singleton;
}

const absl::btree_set<BinopKind>& GetBinopComparisonKinds() {
  static auto* singleton = new absl::btree_set<BinopKind>{
      BinopKind::kGe, BinopKind::kGt, BinopKind::kLe,
      BinopKind::kLt, BinopKind::kEq, BinopKind::kNe,
  };
  return *singleton;
}

const absl::btree_set<BinopKind>& GetBinopShifts() {
  static auto* singleton = new absl::btree_set<BinopKind>{
      BinopKind::kShl,
      BinopKind::kShr,
  };
  return *singleton;
}

std::string BinopKindFormat(BinopKind kind) {
  switch (kind) {
    // clang-format off
    // Shifts.
    case BinopKind::kShl:       return "<<";
    case BinopKind::kShr:       return ">>";
    // Comparisons.
    case BinopKind::kGe:         return ">=";
    case BinopKind::kGt:         return ">";
    case BinopKind::kLe:         return "<=";
    case BinopKind::kLt:         return "<";
    case BinopKind::kEq:         return "==";
    case BinopKind::kNe:         return "!=";

    case BinopKind::kAdd:        return "+";
    case BinopKind::kSub:        return "-";
    case BinopKind::kMul:        return "*";
    case BinopKind::kAnd:        return "&";
    case BinopKind::kOr:         return "|";
    case BinopKind::kXor:        return "^";
    case BinopKind::kDiv:        return "/";
    case BinopKind::kMod:        return "%";
    case BinopKind::kLogicalAnd: return "&&";
    case BinopKind::kLogicalOr:  return "||";
    case BinopKind::kConcat:     return "++";
      // clang-format on
  }
  return absl::StrFormat("<invalid BinopKind(%d)>", static_cast<int>(kind));
}

std::string BinopKindToString(BinopKind kind) {
  switch (kind) {
#define CASIFY(__enum, __str, ...) \
  case BinopKind::__enum:          \
    return __str;
    XLS_DSLX_BINOP_KIND_EACH(CASIFY)
#undef CASIFY
  }
  return absl::StrFormat("<invalid BinopKind(%d)>", static_cast<int>(kind));
}

// -- class NameDef

NameDef::NameDef(Module* owner, Span span, std::string identifier,
                 AstNode* definer)
    : AstNode(owner),
      span_(std::move(span)),
      identifier_(std::move(identifier)),
      definer_(definer) {}

NameDef::~NameDef() = default;

// -- class Conditional

Conditional::Conditional(Module* owner, Span span, Expr* test,
                         Block* consequent,
                         std::variant<Block*, Conditional*> alternate)
    : Expr(owner, std::move(span)),
      test_(test),
      consequent_(consequent),
      alternate_(alternate) {}

Conditional::~Conditional() = default;

std::string Conditional::ToStringInternal() const {
  std::string inline_str = absl::StrFormat(
      R"(if %s %s else %s)", test_->ToInlineString(),
      consequent_->ToInlineString(), ToAstNode(alternate_)->ToInlineString());
  if (inline_str.size() <= kTargetLineChars) {
    return inline_str;
  }
  return absl::StrFormat(R"(if %s %s else %s)", test_->ToString(),
                         consequent_->ToString(),
                         ToAstNode(alternate_)->ToString());
}

// -- class Attr

Attr::~Attr() = default;

// -- class ParametricBinding

ParametricBinding::ParametricBinding(Module* owner, NameDef* name_def,
                                     TypeAnnotation* type_annotation,
                                     Expr* expr)
    : AstNode(owner),
      name_def_(name_def),
      type_annotation_(type_annotation),
      expr_(expr) {
  XLS_CHECK_EQ(name_def_->owner(), owner);
  XLS_CHECK_EQ(type_annotation_->owner(), owner);
}

ParametricBinding::~ParametricBinding() = default;

std::string ParametricBinding::ToString() const {
  std::string suffix;
  if (expr_ != nullptr) {
    suffix = absl::StrFormat(" = {%s}", expr_->ToString());
  }
  return absl::StrFormat("%s: %s%s", name_def_->ToString(),
                         type_annotation_->ToString(), suffix);
}

std::vector<AstNode*> ParametricBinding::GetChildren(bool want_types) const {
  std::vector<AstNode*> results = {name_def_};
  if (want_types) {
    results.push_back(type_annotation_);
  }
  if (expr_ != nullptr) {
    results.push_back(expr_);
  }
  return results;
}

std::string MatchArm::ToString() const {
  std::string patterns_or = absl::StrJoin(
      patterns_, " | ", [](std::string* out, NameDefTree* name_def_tree) {
        absl::StrAppend(out, name_def_tree->ToString());
      });
  return absl::StrFormat("%s => %s", patterns_or, expr_->ToString());
}

std::vector<AstNode*> StructInstance::GetChildren(bool want_types) const {
  std::vector<AstNode*> results;
  results.reserve(members_.size());
  for (auto& item : members_) {
    results.push_back(item.second);
  }
  return results;
}

std::string StructInstance::ToStringInternal() const {
  std::string type_name;
  if (std::holds_alternative<StructDef*>(struct_ref_)) {
    type_name = std::get<StructDef*>(struct_ref_)->identifier();
  } else {
    type_name = ToAstNode(struct_ref_)->ToString();
  }

  std::string members_str = absl::StrJoin(
      members_, ", ",
      [](std::string* out, const std::pair<std::string, Expr*>& member) {
        absl::StrAppendFormat(out, "%s: %s", member.first,
                              member.second->ToString());
      });
  return absl::StrFormat("%s { %s }", type_name, members_str);
}

std::string For::ToStringInternal() const {
  std::string type_str;
  if (type_annotation_ != nullptr) {
    type_str = absl::StrCat(": ", type_annotation_->ToString());
  }
  return absl::StrFormat(R"(for %s%s in %s %s(%s))", names_->ToString(),
                         type_str, iterable_->ToString(), body_->ToString(),
                         init_->ToString());
}

UnrollFor::UnrollFor(Module* owner, Span span, NameDefTree* names,
                     TypeAnnotation* types, Expr* iterable, Block* body,
                     Expr* init)
    : Expr(owner, std::move(span)),
      names_(names),
      types_(types),
      iterable_(iterable),
      body_(body),
      init_(init) {}

UnrollFor::~UnrollFor() = default;

std::string UnrollFor::ToStringInternal() const {
  std::string type_str;
  if (types_ != nullptr) {
    type_str = absl::StrCat(": ", types_->ToString());
  }
  return absl::StrFormat("unroll_for! %s%s in %s %s(%s)", names_->ToString(),
                         type_str, iterable_->ToString(), body_->ToString(),
                         init_->ToString());
}

std::vector<AstNode*> UnrollFor::GetChildren(bool want_types) const {
  std::vector<AstNode*> children{names_, iterable_, body_, init_};
  if (want_types && types_ != nullptr) {
    children.push_back(types_);
  }
  return children;
}

ConstantDef::ConstantDef(Module* owner, Span span, NameDef* name_def,
                         TypeAnnotation* type_annotation, Expr* value,
                         bool is_public)
    : AstNode(owner),
      span_(std::move(span)),
      name_def_(name_def),
      type_annotation_(type_annotation),
      value_(value),
      is_public_(is_public) {}

ConstantDef::~ConstantDef() = default;

std::string ConstantDef::ToString() const {
  std::string privacy;
  if (is_public_) {
    privacy = "pub ";
  }
  std::string type_annotation_str;
  if (type_annotation_ != nullptr) {
    type_annotation_str = absl::StrCat(": ", type_annotation_->ToString());
  }
  return absl::StrFormat("%sconst %s%s = %s;", privacy, name_def_->ToString(),
                         type_annotation_str, value_->ToString());
}

Array::Array(Module* owner, Span span, std::vector<Expr*> members,
             bool has_ellipsis)
    : Expr(owner, std::move(span)),
      members_(std::move(members)),
      has_ellipsis_(has_ellipsis) {}

ConstantArray::ConstantArray(Module* owner, Span span,
                             std::vector<Expr*> members, bool has_ellipsis)
    : Array(owner, std::move(span), std::move(members), has_ellipsis) {
  for (Expr* expr : this->members()) {
    XLS_CHECK(IsConstant(expr))
        << "non-constant in constant array: " << expr->ToString();
  }
}

ConstantArray::~ConstantArray() = default;

// -- class TypeRef

TypeRef::TypeRef(Module* owner, Span span, TypeDefinition type_definition)
    : AstNode(owner),
      span_(std::move(span)),
      type_definition_(type_definition) {}

std::string TypeRef::ToString() const {
  return absl::visit(Visitor{[&](TypeAlias* n) { return n->identifier(); },
                             [&](StructDef* n) { return n->identifier(); },
                             [&](EnumDef* n) { return n->identifier(); },
                             [&](ColonRef* n) { return n->ToString(); }},
                     type_definition_);
}

TypeRef::~TypeRef() = default;

// -- class Import

Import::Import(Module* owner, Span span, std::vector<std::string> subject,
               NameDef* name_def, std::optional<std::string> alias)
    : AstNode(owner),
      span_(std::move(span)),
      subject_(std::move(subject)),
      name_def_(name_def),
      alias_(std::move(alias)) {
  XLS_CHECK(!subject_.empty());
  XLS_CHECK(name_def != nullptr);
}

Import::~Import() = default;

std::string Import::ToString() const {
  if (alias_.has_value()) {
    return absl::StrFormat("import %s as %s", absl::StrJoin(subject_, "."),
                           *alias_);
  }
  return absl::StrFormat("import %s", absl::StrJoin(subject_, "."));
}

// -- class ColonRef

ColonRef::ColonRef(Module* owner, Span span, Subject subject, std::string attr)
    : Expr(owner, std::move(span)), subject_(subject), attr_(std::move(attr)) {}

ColonRef::~ColonRef() = default;

std::optional<Import*> ColonRef::ResolveImportSubject() const {
  if (!std::holds_alternative<NameRef*>(subject_)) {
    return std::nullopt;
  }
  auto* name_ref = std::get<NameRef*>(subject_);
  AnyNameDef any_name_def = name_ref->name_def();
  if (!std::holds_alternative<const NameDef*>(any_name_def)) {
    return std::nullopt;
  }
  const auto* name_def = std::get<const NameDef*>(any_name_def);
  AstNode* definer = name_def->definer();
  Import* import = dynamic_cast<Import*>(definer);
  if (import == nullptr) {
    return std::nullopt;
  }
  return import;
}

// -- class ProcMember

ProcMember::ProcMember(Module* owner, NameDef* name_def,
                       TypeAnnotation* type_annotation)
    : AstNode(owner),
      name_def_(name_def),
      type_annotation_(type_annotation),
      span_(name_def_->span().start(), type_annotation_->span().limit()) {}

ProcMember::~ProcMember() = default;

// -- class Param

Param::Param(Module* owner, NameDef* name_def, TypeAnnotation* type_annotation)
    : AstNode(owner),
      name_def_(name_def),
      type_annotation_(type_annotation),
      span_(name_def_->span().start(), type_annotation_->span().limit()) {}

Param::~Param() = default;

// -- class ChannelDecl

ChannelDecl::~ChannelDecl() = default;

std::string ChannelDecl::ToStringInternal() const {
  std::vector<std::string> dims;
  if (dims_.has_value()) {
    for (const Expr* dim : dims_.value()) {
      dims.push_back(absl::StrCat("[", dim->ToString(), "]"));
    }
  }

  std::string fifo_depth_str;
  if (fifo_depth_.has_value()) {
    fifo_depth_str = absl::StrCat(", ", fifo_depth_.value()->ToString());
  }
  return absl::StrFormat("chan<%s%s>%s", type_->ToString(), fifo_depth_str,
                         absl::StrJoin(dims, ""));
}

// -- class Module

Module::~Module() {
  XLS_VLOG(3) << "Destroying module \"" << name_ << "\" @ " << this;
}

const AstNode* Module::FindNode(AstNodeKind kind, const Span& target) const {
  for (const auto& node : nodes_) {
    if (node->kind() == kind && node->GetSpan().has_value() &&
        node->GetSpan().value() == target) {
      return node.get();
    }
  }
  return nullptr;
}

std::vector<const AstNode*> Module::FindIntercepting(const Pos& target) const {
  std::vector<const AstNode*> found;
  for (const auto& node : nodes_) {
    if (node->GetSpan().has_value() && node->GetSpan()->Contains(target)) {
      found.push_back(node.get());
    }
  }
  return found;
}

std::optional<Function*> Module::GetFunction(std::string_view target_name) {
  for (ModuleMember& member : top_) {
    if (std::holds_alternative<Function*>(member)) {
      Function* f = std::get<Function*>(member);
      if (f->identifier() == target_name) {
        return f;
      }
    }
  }
  return std::nullopt;
}

std::optional<Proc*> Module::GetProc(std::string_view target_name) {
  for (ModuleMember& member : top_) {
    if (std::holds_alternative<Proc*>(member)) {
      Proc* p = std::get<Proc*>(member);
      if (p->identifier() == target_name) {
        return p;
      }
    }
  }
  return std::nullopt;
}

absl::StatusOr<TestFunction*> Module::GetTest(std::string_view target_name) {
  for (ModuleMember& member : top_) {
    if (std::holds_alternative<TestFunction*>(member)) {
      TestFunction* t = std::get<TestFunction*>(member);
      if (t->identifier() == target_name) {
        return t;
      }
    }
  }
  return absl::NotFoundError(absl::StrFormat(
      "No test in module %s with name \"%s\"", name_, target_name));
}

absl::StatusOr<TestProc*> Module::GetTestProc(std::string_view target_name) {
  for (ModuleMember& member : top_) {
    if (std::holds_alternative<TestProc*>(member)) {
      auto* t = std::get<TestProc*>(member);
      if (t->proc()->identifier() == target_name) {
        return t;
      }
    }
  }
  return absl::NotFoundError(absl::StrFormat(
      "No test proc in module %s with name \"%s\"", name_, target_name));
}

std::vector<std::string> Module::GetTestNames() const {
  std::vector<std::string> result;
  for (auto& member : top_) {
    if (std::holds_alternative<TestFunction*>(member)) {
      TestFunction* t = std::get<TestFunction*>(member);
      result.push_back(t->identifier());
    } else if (std::holds_alternative<TestProc*>(member)) {
      TestProc* tp = std::get<TestProc*>(member);
      result.push_back(tp->proc()->identifier());
    }
  }
  return result;
}

std::vector<std::string> Module::GetFunctionNames() const {
  std::vector<std::string> result;
  for (auto& member : top_) {
    if (std::holds_alternative<Function*>(member)) {
      result.push_back(std::get<Function*>(member)->identifier());
    }
  }
  return result;
}

const StructDef* Module::FindStructDef(const Span& span) const {
  return down_cast<const StructDef*>(FindNode(AstNodeKind::kStructDef, span));
}

const EnumDef* Module::FindEnumDef(const Span& span) const {
  return down_cast<const EnumDef*>(FindNode(AstNodeKind::kEnumDef, span));
}

std::optional<ModuleMember*> Module::FindMemberWithName(
    std::string_view target) {
  for (ModuleMember& member : top_) {
    if (std::holds_alternative<Function*>(member)) {
      if (std::get<Function*>(member)->identifier() == target) {
        return &member;
      }
    } else if (std::holds_alternative<Proc*>(member)) {
      if (std::get<Proc*>(member)->identifier() == target) {
        return &member;
      }
    } else if (std::holds_alternative<TestFunction*>(member)) {
      if (std::get<TestFunction*>(member)->identifier() == target) {
        return &member;
      }
    } else if (std::holds_alternative<TestProc*>(member)) {
      if (std::get<TestProc*>(member)->proc()->identifier() == target) {
        return &member;
      }
    } else if (std::holds_alternative<QuickCheck*>(member)) {
      if (std::get<QuickCheck*>(member)->identifier() == target) {
        return &member;
      }
    } else if (std::holds_alternative<TypeAlias*>(member)) {
      if (std::get<TypeAlias*>(member)->identifier() == target) {
        return &member;
      }
    } else if (std::holds_alternative<StructDef*>(member)) {
      if (std::get<StructDef*>(member)->identifier() == target) {
        return &member;
      }
    } else if (std::holds_alternative<ConstantDef*>(member)) {
      if (std::get<ConstantDef*>(member)->identifier() == target) {
        return &member;
      }
    } else if (std::holds_alternative<EnumDef*>(member)) {
      if (std::get<EnumDef*>(member)->identifier() == target) {
        return &member;
      }
    } else if (std::holds_alternative<Import*>(member)) {
      if (std::get<Import*>(member)->identifier() == target) {
        return &member;
      }
    } else {
      XLS_LOG(FATAL) << "Unhandled module member variant: "
                     << ToAstNode(member)->GetNodeTypeName();
    }
  }
  return std::nullopt;
}

absl::StatusOr<ConstantDef*> Module::GetConstantDef(std::string_view target) {
  std::optional<ModuleMember*> member = FindMemberWithName(target);
  if (!member.has_value()) {
    return absl::NotFoundError(
        absl::StrFormat("Could not find member named '%s' in module.", target));
  }
  if (!std::holds_alternative<ConstantDef*>(*member.value())) {
    return absl::NotFoundError(absl::StrFormat(
        "Member named '%s' in module was not a constant.", target));
  }
  return std::get<ConstantDef*>(*member.value());
}

absl::flat_hash_map<std::string, TypeDefinition>
Module::GetTypeDefinitionByName() const {
  absl::flat_hash_map<std::string, TypeDefinition> result;
  for (auto& member : top_) {
    if (std::holds_alternative<TypeAlias*>(member)) {
      TypeAlias* td = std::get<TypeAlias*>(member);
      result[td->identifier()] = td;
    } else if (std::holds_alternative<EnumDef*>(member)) {
      EnumDef* enum_ = std::get<EnumDef*>(member);
      result[enum_->identifier()] = enum_;
    } else if (std::holds_alternative<StructDef*>(member)) {
      StructDef* struct_ = std::get<StructDef*>(member);
      result[struct_->identifier()] = struct_;
    }
  }
  return result;
}

std::vector<TypeDefinition> Module::GetTypeDefinitions() const {
  std::vector<TypeDefinition> results;
  for (auto& member : top_) {
    if (std::holds_alternative<TypeAlias*>(member)) {
      TypeAlias* td = std::get<TypeAlias*>(member);
      results.push_back(td);
    } else if (std::holds_alternative<EnumDef*>(member)) {
      EnumDef* enum_def = std::get<EnumDef*>(member);
      results.push_back(enum_def);
    } else if (std::holds_alternative<StructDef*>(member)) {
      StructDef* struct_def = std::get<StructDef*>(member);
      results.push_back(struct_def);
    }
  }
  return results;
}

std::vector<AstNode*> Module::GetChildren(bool want_types) const {
  std::vector<AstNode*> results;
  results.reserve(top_.size());
  for (ModuleMember member : top_) {
    results.push_back(ToAstNode(member));
  }
  return results;
}

absl::StatusOr<TypeDefinition> Module::GetTypeDefinition(
    std::string_view name) const {
  absl::flat_hash_map<std::string, TypeDefinition> map =
      GetTypeDefinitionByName();
  auto it = map.find(name);
  if (it == map.end()) {
    return absl::NotFoundError(
        absl::StrCat("Could not find type definition for name: ", name));
  }
  return it->second;
}

absl::Status Module::AddTop(ModuleMember member,
                            const MakeCollisionError& make_collision_error) {
  // Get name
  std::optional<std::string> member_name = absl::visit(
      Visitor{
          [](Function* f) { return std::make_optional(f->identifier()); },
          [](Proc* p) { return std::make_optional(p->identifier()); },
          [](TestFunction* tf) { return std::make_optional(tf->identifier()); },
          [](TestProc* tp) {
            return std::make_optional(tp->proc()->identifier());
          },
          [](QuickCheck* qc) { return std::make_optional(qc->identifier()); },
          [](TypeAlias* td) { return std::make_optional(td->identifier()); },
          [](StructDef* sd) { return std::make_optional(sd->identifier()); },
          [](ConstantDef* cd) { return std::make_optional(cd->identifier()); },
          [](EnumDef* ed) { return std::make_optional(ed->identifier()); },
          [](Import* i) { return std::make_optional(i->identifier()); },
          [](ConstAssert* n) -> std::optional<std::string> {
            return std::nullopt;
          },
      },
      member);

  if (member_name.has_value() && top_by_name_.contains(member_name.value())) {
    const AstNode* node = ToAstNode(top_by_name_.at(member_name.value()));
    const Span existing_span = node->GetSpan().value();
    const AstNode* new_node = ToAstNode(member);
    const Span new_span = new_node->GetSpan().value();
    if (make_collision_error != nullptr) {
      return make_collision_error(name_, member_name.value(), existing_span,
                                  node, new_span, new_node);
    }
    return absl::InvalidArgumentError(absl::StrFormat(
        "Module %s already contains a member named %s @ %s: %s", name_,
        member_name.value(), existing_span.ToString(), node->ToString()));
  }

  top_.push_back(member);
  if (member_name.has_value()) {
    top_by_name_.insert({member_name.value(), member});
  }
  return absl::OkStatus();
}

std::string_view GetModuleMemberTypeName(const ModuleMember& module_member) {
  return absl::visit(Visitor{
                         [](Function*) { return "function"; },
                         [](Proc*) { return "proc"; },
                         [](TestFunction*) { return "test-function"; },
                         [](TestProc*) { return "test-proc"; },
                         [](QuickCheck*) { return "quick-check"; },
                         [](TypeAlias*) { return "type-alias"; },
                         [](StructDef*) { return "struct-definition"; },
                         [](ConstantDef*) { return "constant-definition"; },
                         [](EnumDef*) { return "enum-definition"; },
                         [](Import*) { return "import"; },
                         [](ConstAssert*) { return "const-assert"; },
                     },
                     module_member);
}

absl::StatusOr<ModuleMember> AsModuleMember(AstNode* node) {
  // clang-format off
  if (auto* n = dynamic_cast<Function*    >(node)) { return ModuleMember(n); }
  if (auto* n = dynamic_cast<TestFunction*>(node)) { return ModuleMember(n); }
  if (auto* n = dynamic_cast<QuickCheck*  >(node)) { return ModuleMember(n); }
  if (auto* n = dynamic_cast<TypeAlias*   >(node)) { return ModuleMember(n); }
  if (auto* n = dynamic_cast<StructDef*   >(node)) { return ModuleMember(n); }
  if (auto* n = dynamic_cast<ConstantDef* >(node)) { return ModuleMember(n); }
  if (auto* n = dynamic_cast<EnumDef*     >(node)) { return ModuleMember(n); }
  if (auto* n = dynamic_cast<Import*      >(node)) { return ModuleMember(n); }
  // clang-format on
  return absl::InvalidArgumentError("AST node is not a module-level member: " +
                                    node->ToString());
}

absl::StatusOr<IndexRhs> AstNodeToIndexRhs(AstNode* node) {
  // clang-format off
  if (auto* n = dynamic_cast<Slice*     >(node)) { return IndexRhs(n); }
  if (auto* n = dynamic_cast<WidthSlice*>(node)) { return IndexRhs(n); }
  if (auto* n = dynamic_cast<Expr*      >(node)) { return IndexRhs(n); }
  // clang-format on
  return absl::InvalidArgumentError("AST node is not a valid 'index': " +
                                    node->ToString());
}

TypeAnnotation::~TypeAnnotation() = default;

// -- class TypeRefTypeAnnotation

TypeRefTypeAnnotation::TypeRefTypeAnnotation(
    Module* owner, Span span, TypeRef* type_ref,
    std::vector<ExprOrType> parametrics)
    : TypeAnnotation(owner, std::move(span)),
      type_ref_(type_ref),
      parametrics_(std::move(parametrics)) {}

TypeRefTypeAnnotation::~TypeRefTypeAnnotation() = default;

std::vector<AstNode*> TypeRefTypeAnnotation::GetChildren(
    bool want_types) const {
  std::vector<AstNode*> results = {type_ref_};
  for (const ExprOrType& e : parametrics_) {
    if (std::holds_alternative<TypeAnnotation*>(e)) {
      if (want_types) {
        results.push_back(std::get<TypeAnnotation*>(e));
      }
    } else {
      results.push_back(std::get<Expr*>(e));
    }
  }
  return results;
}

std::string TypeRefTypeAnnotation::ToString() const {
  std::string parametric_str;
  if (!parametrics_.empty()) {
    std::vector<std::string> pieces;
    pieces.reserve(parametrics_.size());
    for (const ExprOrType& e : parametrics_) {
      pieces.push_back(ToAstNode(e)->ToString());
    }
    parametric_str = absl::StrCat("<", absl::StrJoin(pieces, ", "), ">");
  }
  return absl::StrCat(type_ref_->ToString(), parametric_str);
}

// -- class ArrayTypeAnnotation

ArrayTypeAnnotation::ArrayTypeAnnotation(Module* owner, Span span,
                                         TypeAnnotation* element_type,
                                         Expr* dim)
    : TypeAnnotation(owner, std::move(span)),
      element_type_(element_type),
      dim_(dim) {}

ArrayTypeAnnotation::~ArrayTypeAnnotation() = default;

std::vector<AstNode*> ArrayTypeAnnotation::GetChildren(bool want_types) const {
  return {element_type_, dim_};
}

std::string ArrayTypeAnnotation::ToString() const {
  return absl::StrFormat("%s[%s]", element_type_->ToString(), dim_->ToString());
}

// -- class BuiltinNameDef

BuiltinNameDef::~BuiltinNameDef() = default;

// -- class SplatStructInstance

bool IsConstant(AstNode* node) {
  if (IsOneOf<ConstantArray, Number, ConstRef, ColonRef>(node)) {
    return true;
  }
  if (Cast* n = dynamic_cast<Cast*>(node)) {
    return IsConstant(n->expr());
  }
  if (StructInstance* n = dynamic_cast<StructInstance*>(node)) {
    for (const auto& [name, expr] : n->GetUnorderedMembers()) {
      if (!IsConstant(expr)) {
        return false;
      }
    }
    return true;
  }
  if (XlsTuple* n = dynamic_cast<XlsTuple*>(node)) {
    return std::all_of(n->members().begin(), n->members().end(), IsConstant);
  }
  if (Expr* e = dynamic_cast<Expr*>(node)) {
    auto children = e->GetChildren(/*want_types=*/false);
    return std::all_of(children.begin(), children.end(), IsConstant);
  }
  return false;
}

std::vector<AstNode*> SplatStructInstance::GetChildren(bool want_types) const {
  std::vector<AstNode*> results;
  results.reserve(members_.size() + 1);
  for (auto& item : members_) {
    results.push_back(item.second);
  }
  results.push_back(splatted_);
  return results;
}

std::string SplatStructInstance::ToStringInternal() const {
  std::string type_name;
  if (std::holds_alternative<StructDef*>(struct_ref_)) {
    type_name = std::get<StructDef*>(struct_ref_)->identifier();
  } else {
    type_name = ToAstNode(struct_ref_)->ToString();
  }

  std::string members_str = absl::StrJoin(
      members_, ", ",
      [](std::string* out, const std::pair<std::string, Expr*>& member) {
        absl::StrAppendFormat(out, "%s: %s", member.first,
                              member.second->ToString());
      });
  return absl::StrFormat("%s { %s, ..%s }", type_name, members_str,
                         splatted_->ToString());
}

std::vector<AstNode*> MatchArm::GetChildren(bool want_types) const {
  std::vector<AstNode*> results;
  results.reserve(patterns_.size());
  for (NameDefTree* ndt : patterns_) {
    results.push_back(ndt);
  }
  results.push_back(expr_);
  return results;
}

// -- class Match

Match::~Match() = default;

std::vector<AstNode*> Match::GetChildren(bool want_types) const {
  std::vector<AstNode*> results = {matched_};
  for (MatchArm* arm : arms_) {
    results.push_back(arm);
  }
  return results;
}

std::string Match::ToStringInternal() const {
  std::string result = absl::StrFormat("match %s {\n", matched_->ToString());
  for (MatchArm* arm : arms_) {
    absl::StrAppend(&result, Indent(absl::StrCat(arm->ToString(), ",\n"),
                                    kRustSpacesPerIndent));
  }
  absl::StrAppend(&result, "}");
  return result;
}

// -- class Index

Index::~Index() = default;

std::string Index::ToStringInternal() const {
  std::string lhs = lhs_->ToString();
  if (WeakerThan(lhs_->GetPrecedence(), GetPrecedenceInternal())) {
    Parenthesize(&lhs);
  }
  return absl::StrFormat("%s[%s]", lhs, ToAstNode(rhs_)->ToString());
}

// -- class WidthSlice

WidthSlice::~WidthSlice() = default;

std::string WidthSlice::ToString() const {
  return absl::StrFormat("%s+:%s", start_->ToString(), width_->ToString());
}

// -- class Slice

Slice::~Slice() = default;

std::vector<AstNode*> Slice::GetChildren(bool want_types) const {
  std::vector<AstNode*> results;
  if (start_ != nullptr) {
    results.push_back(start_);
  }
  if (limit_ != nullptr) {
    results.push_back(limit_);
  }
  return results;
}

std::string Slice::ToString() const {
  if (start_ != nullptr && limit_ != nullptr) {
    return absl::StrFormat("%s:%s", start_->ToString(), limit_->ToString());
  }
  if (start_ != nullptr) {
    return absl::StrFormat("%s:", start_->ToString());
  }
  if (limit_ != nullptr) {
    return absl::StrFormat(":%s", limit_->ToString());
  }
  return ":";
}

// -- class EnumDef

EnumDef::EnumDef(Module* owner, Span span, NameDef* name_def,
                 TypeAnnotation* type_annotation,
                 std::vector<EnumMember> values, bool is_public)
    : AstNode(owner),
      span_(std::move(span)),
      name_def_(name_def),
      type_annotation_(type_annotation),
      values_(std::move(values)),
      is_public_(is_public) {}

EnumDef::~EnumDef() = default;

bool EnumDef::HasValue(std::string_view name) const {
  for (const auto& item : values_) {
    if (item.name_def->identifier() == name) {
      return true;
    }
  }
  return false;
}

absl::StatusOr<Expr*> EnumDef::GetValue(std::string_view name) const {
  for (const EnumMember& item : values_) {
    if (item.name_def->identifier() == name) {
      return item.value;
    }
  }
  return absl::NotFoundError(absl::StrFormat(
      "Enum %s has no value with name \"%s\"", identifier(), name));
}

std::string EnumDef::ToString() const {
  std::string type_str;
  if (type_annotation_ != nullptr) {
    type_str = absl::StrCat(" : " + type_annotation_->ToString());
  }
  std::string result = absl::StrFormat(
      "%senum %s%s {\n", is_public_ ? "pub " : "", identifier(), type_str);

  auto value_to_string = [](Expr* value) -> std::string {
    if (Number* number = dynamic_cast<Number*>(value)) {
      return number->ToStringNoType();
    }
    return value->ToString();
  };

  for (const auto& item : values_) {
    absl::StrAppendFormat(&result, "%s%s = %s,\n", kRustOneIndent,
                          item.name_def->identifier(),
                          value_to_string(item.value));
  }
  absl::StrAppend(&result, "}");
  return result;
}

// -- class Instantiation

Instantiation::Instantiation(Module* owner, Span span, Expr* callee,
                             std::vector<ExprOrType> explicit_parametrics)
    : Expr(owner, std::move(span)),
      callee_(callee),
      explicit_parametrics_(std::move(explicit_parametrics)) {}

Instantiation::~Instantiation() = default;

std::string Instantiation::FormatParametrics() const {
  if (explicit_parametrics_.empty()) {
    return "";
  }

  return absl::StrCat("<",
                      absl::StrJoin(explicit_parametrics_, ", ",
                                    [](std::string* out, ExprOrType e) {
                                      absl::StrAppend(out,
                                                      ToAstNode(e)->ToString());
                                    }),
                      ">");
}

// -- class Invocation

Invocation::Invocation(Module* owner, Span span, Expr* callee,
                       std::vector<Expr*> args,
                       std::vector<ExprOrType> explicit_parametrics)
    : Instantiation(owner, std::move(span), callee,
                    std::move(explicit_parametrics)),
      args_(std::move(args)) {}

Invocation::~Invocation() = default;

std::vector<AstNode*> Invocation::GetChildren(bool want_types) const {
  std::vector<AstNode*> results = {callee()};
  for (const ExprOrType& eot : explicit_parametrics()) {
    results.push_back(ToAstNode(eot));
  }
  for (Expr* arg : args_) {
    results.push_back(arg);
  }
  return results;
}

std::string Invocation::FormatArgs() const {
  return absl::StrJoin(args_, ", ", [](std::string* out, Expr* e) {
    absl::StrAppend(out, e->ToString());
  });
}

// -- class Spawn

Spawn::Spawn(Module* owner, Span span, Expr* callee, Invocation* config,
             Invocation* next, std::vector<ExprOrType> explicit_parametrics)
    : Instantiation(owner, std::move(span), callee,
                    std::move(explicit_parametrics)),
      config_(config),
      next_(next) {}

Spawn::~Spawn() = default;

std::vector<AstNode*> Spawn::GetChildren(bool want_types) const {
  return {config_, next_};
}

std::string Spawn::ToStringInternal() const {
  std::string param_str;
  if (!explicit_parametrics().empty()) {
    param_str = FormatParametrics();
  }

  std::string config_args = absl::StrJoin(
      config_->args(), ", ",
      [](std::string* out, Expr* e) { absl::StrAppend(out, e->ToString()); });

  return absl::StrFormat("spawn %s%s(%s)", callee()->ToString(), param_str,
                         config_args);
}

// -- class ConstAssert

ConstAssert::ConstAssert(Module* owner, Span span, Expr* arg)
    : AstNode(owner), span_(std::move(span)), arg_(arg) {}

ConstAssert::~ConstAssert() = default;

std::vector<AstNode*> ConstAssert::GetChildren(bool want_types) const {
  return std::vector<AstNode*>{arg()};
}

std::string ConstAssert::ToString() const {
  return absl::StrFormat("const_assert!(%s);", arg()->ToString());
}

// -- class ZeroMacro

ZeroMacro::ZeroMacro(Module* owner, Span span, ExprOrType type)
    : Expr(owner, std::move(span)), type_(type) {}

ZeroMacro::~ZeroMacro() = default;

std::vector<AstNode*> ZeroMacro::GetChildren(bool want_types) const {
  if (want_types) {
    return {ToAstNode(type_)};
  }
  return {};
}

std::string ZeroMacro::ToStringInternal() const {
  return absl::StrFormat("zero!<%s>()", ToAstNode(type_)->ToString());
}

// -- class FormatMacro

FormatMacro::FormatMacro(Module* owner, Span span, std::string macro,
                         std::vector<FormatStep> format,
                         std::vector<Expr*> args)
    : Expr(owner, std::move(span)),
      macro_(std::move(macro)),
      format_(std::move(format)),
      args_(std::move(args)) {}

FormatMacro::~FormatMacro() = default;

std::vector<AstNode*> FormatMacro::GetChildren(bool want_types) const {
  std::vector<AstNode*> results;
  results.reserve(args_.size());
  for (Expr* arg : args_) {
    results.push_back(arg);
  }
  return results;
}

std::string FormatMacro::ToStringInternal() const {
  std::string format_string = "\"";
  for (const auto& step : format_) {
    if (std::holds_alternative<std::string>(step)) {
      absl::StrAppend(&format_string, std::get<std::string>(step));
    } else {
      absl::StrAppend(&format_string,
                      std::string(FormatPreferenceToXlsSpecifier(
                          std::get<FormatPreference>(step))));
    }
  }
  absl::StrAppend(&format_string, "\"");
  return absl::StrFormat("%s(%s, %s)", macro_, format_string, FormatArgs());
}

std::string FormatMacro::FormatArgs() const {
  return absl::StrJoin(args_, ", ", [](std::string* out, Expr* e) {
    absl::StrAppend(out, e->ToString());
  });
}

// -- class StructDef

StructDef::StructDef(Module* owner, Span span, NameDef* name_def,
                     std::vector<ParametricBinding*> parametric_bindings,
                     std::vector<std::pair<NameDef*, TypeAnnotation*>> members,
                     bool is_public)
    : AstNode(owner),
      span_(std::move(span)),
      name_def_(name_def),
      parametric_bindings_(std::move(parametric_bindings)),
      members_(std::move(members)),
      public_(is_public) {}

StructDef::~StructDef() = default;

std::vector<AstNode*> StructDef::GetChildren(bool want_types) const {
  std::vector<AstNode*> results = {name_def_};
  for (auto* pb : parametric_bindings_) {
    results.push_back(pb);
  }
  for (const auto& pair : members_) {
    results.push_back(pair.first);
    results.push_back(pair.second);
  }
  return results;
}

std::string StructDef::ToString() const {
  std::string parametric_str;
  if (!parametric_bindings_.empty()) {
    std::string guts =
        absl::StrJoin(parametric_bindings_, ", ",
                      [](std::string* out, ParametricBinding* binding) {
                        absl::StrAppend(out, binding->ToString());
                      });
    parametric_str = absl::StrFormat("<%s>", guts);
  }
  std::string result = absl::StrFormat(
      "%sstruct %s%s {\n", public_ ? "pub " : "", identifier(), parametric_str);
  for (const auto& item : members_) {
    absl::StrAppendFormat(&result, "%s%s: %s,\n", kRustOneIndent,
                          item.first->ToString(), item.second->ToString());
  }
  absl::StrAppend(&result, "}");
  return result;
}

std::vector<std::string> StructDef::GetMemberNames() const {
  std::vector<std::string> names;
  names.reserve(members_.size());
  for (auto& item : members_) {
    names.push_back(item.first->identifier());
  }
  return names;
}

// -- class StructInstance

StructInstance::StructInstance(
    Module* owner, Span span, StructRef struct_ref,
    std::vector<std::pair<std::string, Expr*>> members)
    : Expr(owner, std::move(span)),
      struct_ref_(struct_ref),
      members_(std::move(members)) {}

StructInstance::~StructInstance() = default;

std::vector<std::pair<std::string, Expr*>> StructInstance::GetOrderedMembers(
    const StructDef* struct_def) const {
  std::vector<std::pair<std::string, Expr*>> result;
  for (const std::string& name : struct_def->GetMemberNames()) {
    result.push_back({name, GetExpr(name).value()});
  }
  return result;
}

absl::StatusOr<Expr*> StructInstance::GetExpr(std::string_view name) const {
  for (const auto& item : members_) {
    if (item.first == name) {
      return item.second;
    }
  }
  return absl::NotFoundError(
      absl::StrFormat("Name is not present in struct instance: \"%s\"", name));
}

// -- class SplatStructInstance

SplatStructInstance::SplatStructInstance(
    Module* owner, Span span, StructRef struct_ref,
    std::vector<std::pair<std::string, Expr*>> members, Expr* splatted)
    : Expr(owner, std::move(span)),
      struct_ref_(struct_ref),
      members_(std::move(members)),
      splatted_(splatted) {}

SplatStructInstance::~SplatStructInstance() = default;

// -- class Unop

Unop::~Unop() = default;

std::string Unop::ToStringInternal() const {
  std::string operand = operand_->ToString();
  if (WeakerThan(operand_->GetPrecedence(), GetPrecedenceInternal())) {
    Parenthesize(&operand);
  }
  return absl::StrFormat("%s%s", UnopKindToString(unop_kind_), operand);
}

std::string UnopKindToString(UnopKind k) {
  switch (k) {
    case UnopKind::kInvert:
      return "!";
    case UnopKind::kNegate:
      return "-";
  }
  return absl::StrFormat("<invalid UnopKind(%d)>", static_cast<int>(k));
}

// -- class Binop

Binop::~Binop() = default;

Precedence Binop::GetPrecedenceInternal() const {
  switch (binop_kind_) {
    case BinopKind::kShl:
      return Precedence::kShift;
    case BinopKind::kShr:
      return Precedence::kShift;
    case BinopKind::kLogicalAnd:
      return Precedence::kLogicalAnd;
    case BinopKind::kLogicalOr:
      return Precedence::kLogicalOr;
    // bitwise
    case BinopKind::kXor:
      return Precedence::kBitwiseXor;
    case BinopKind::kOr:
      return Precedence::kBitwiseOr;
    case BinopKind::kAnd:
      return Precedence::kBitwiseAnd;
    // comparisons
    case BinopKind::kEq:
    case BinopKind::kNe:
    case BinopKind::kGe:
    case BinopKind::kGt:
    case BinopKind::kLt:
    case BinopKind::kLe:
      return Precedence::kComparison;
    // weak arithmetic
    case BinopKind::kAdd:
    case BinopKind::kSub:
      return Precedence::kWeakArithmetic;
    // strong arithmetic
    case BinopKind::kMul:
    case BinopKind::kDiv:
    case BinopKind::kMod:
      return Precedence::kStrongArithmetic;
    case BinopKind::kConcat:
      return Precedence::kConcat;
  }
  XLS_LOG(FATAL) << "Invalid binop kind: " << static_cast<int>(binop_kind_);
}

absl::StatusOr<BinopKind> BinopKindFromString(std::string_view s) {
#define HANDLE(__enum, __unused, __operator) \
  if (s == __operator) {                     \
    return BinopKind::__enum;                \
  }
  XLS_DSLX_BINOP_KIND_EACH(HANDLE)
#undef HANDLE
  return absl::InvalidArgumentError(
      absl::StrFormat("Invalid BinopKind string: \"%s\"", s));
}

Binop::Binop(Module* owner, Span span, BinopKind binop_kind, Expr* lhs,
             Expr* rhs)
    : Expr(owner, std::move(span)),
      binop_kind_(binop_kind),
      lhs_(lhs),
      rhs_(rhs) {}

std::string Binop::ToStringInternal() const {
  Precedence op_precedence = GetPrecedenceInternal();
  std::string lhs = lhs_->ToString();
  {
    Precedence lhs_precedence = lhs_->GetPrecedence();
    XLS_VLOG(10) << "lhs_expr: `" << lhs << "` precedence: " << lhs_precedence
                 << " op_precedence: " << op_precedence;
    if (WeakerThan(lhs_precedence, op_precedence)) {
      Parenthesize(&lhs);
    } else if (binop_kind_ == BinopKind::kLt &&
               lhs_->kind() == AstNodeKind::kCast && !lhs_->in_parens()) {
      // If there is an open angle bracket, and the LHS is suffixed with a type,
      // we parenthesize it to avoid ambiguity; e.g.
      //
      //    foo as bar < baz
      //           ^~~~~~~~^
      //
      // We don't know whether `bar<baz` is the start of a parametric type
      // instantiation, so we force conservative parenthesization:
      //
      //    (foo as bar) < baz
      Parenthesize(&lhs);
    }
  }

  std::string rhs = rhs_->ToString();
  {
    if (WeakerThan(rhs_->GetPrecedence(), op_precedence)) {
      Parenthesize(&rhs);
    }
  }
  return absl::StrFormat("%s %s %s", lhs, BinopKindFormat(binop_kind_), rhs);
}

// -- class Block

Block::Block(Module* owner, Span span, std::vector<Statement*> statements,
             bool trailing_semi)
    : Expr(owner, std::move(span)),
      statements_(std::move(statements)),
      trailing_semi_(trailing_semi) {
  if (statements_.empty()) {
    XLS_CHECK(trailing_semi) << "empty block but trailing_semi is false";
  }
}

Block::~Block() = default;

std::string Block::ToInlineString() const {
  // A formatting special case: if there are no statements (and implicitly a
  // trailing semi since an empty block gives unit type) we just give back
  // braces without any semicolon inside.
  if (statements_.empty()) {
    XLS_CHECK(trailing_semi_);
    return "{}";
  }

  std::string s = absl::StrCat(
      "{ ",
      absl::StrJoin(statements_, "; ", [](std::string* out, Statement* stmt) {
        absl::StrAppend(out, stmt->ToString());
      }));
  if (trailing_semi_) {
    absl::StrAppend(&s, ";");
  }
  absl::StrAppend(&s, " }");
  return s;
}

std::string Block::ToStringInternal() const {
  // A formatting special case: if there are no statements (and implicitly a
  // trailing semi since an empty block gives unit type) we just give back
  // braces without any semicolon inside.
  if (statements_.empty()) {
    XLS_CHECK(trailing_semi_);
    return "{}";
  }

  std::vector<std::string> stmts;
  for (size_t i = 0; i < statements_.size(); ++i) {
    Statement* stmt = statements_[i];
    if (std::holds_alternative<Expr*>(stmt->wrapped())) {
      if (i + 1 == statements_.size() && !trailing_semi_) {
        stmts.push_back(stmt->ToString());
      } else {
        stmts.push_back(stmt->ToString() + ";");
      }
    } else {
      stmts.push_back(stmt->ToString());
    }
  }
  return absl::StrFormat(
      "{\n%s\n}", Indent(absl::StrJoin(stmts, "\n"), kRustSpacesPerIndent));
}

// -- class For

For::For(Module* owner, Span span, NameDefTree* names,
         TypeAnnotation* type_annotation, Expr* iterable, Block* body,
         Expr* init)
    : Expr(owner, std::move(span)),
      names_(names),
      type_annotation_(type_annotation),
      iterable_(iterable),
      body_(body),
      init_(init) {}

For::~For() = default;

std::vector<AstNode*> For::GetChildren(bool want_types) const {
  std::vector<AstNode*> results = {names_};
  if (want_types && type_annotation_ != nullptr) {
    results.push_back(type_annotation_);
  }
  results.push_back(iterable_);
  results.push_back(body_);
  results.push_back(init_);
  return results;
}

// -- class Function

Function::Function(Module* owner, Span span, NameDef* name_def,
                   std::vector<ParametricBinding*> parametric_bindings,
                   std::vector<Param*> params, TypeAnnotation* return_type,
                   Block* body, Tag tag, bool is_public)
    : AstNode(owner),
      span_(std::move(span)),
      name_def_(name_def),
      parametric_bindings_(std::move(parametric_bindings)),
      params_(std::move(params)),
      return_type_(return_type),
      body_(body),
      tag_(tag),
      is_public_(is_public) {}

Function::~Function() = default;

std::vector<AstNode*> Function::GetChildren(bool want_types) const {
  std::vector<AstNode*> results;
  results.push_back(name_def());
  if (tag_ == Tag::kNormal) {
    // The parametric bindings of a proc are shared between the proc itself and
    // the two functions it contains. Thus, they should have a single owner, the
    // proc, and the other two functions "borrow" them.
    for (ParametricBinding* binding : parametric_bindings()) {
      results.push_back(binding);
    }
  }
  for (Param* p : params_) {
    results.push_back(p);
  }
  if (return_type_ != nullptr && want_types) {
    results.push_back(return_type_);
  }
  results.push_back(body());
  return results;
}

std::string Function::ToString() const {
  std::string parametric_str;
  if (!parametric_bindings().empty()) {
    parametric_str = absl::StrFormat(
        "<%s>",
        absl::StrJoin(
            parametric_bindings(), ", ",
            [](std::string* out, ParametricBinding* parametric_binding) {
              absl::StrAppend(out, parametric_binding->ToString());
            }));
  }
  std::string params_str =
      absl::StrJoin(params(), ", ", [](std::string* out, Param* param) {
        absl::StrAppend(out, param->ToString());
      });
  std::string return_type_str = " ";
  if (return_type_ != nullptr) {
    return_type_str = " -> " + return_type_->ToString() + " ";
  }
  std::string pub_str = is_public() ? "pub " : "";
  std::string annotation_str;
  if (extern_verilog_module_.has_value()) {
    annotation_str = absl::StrFormat("#[extern_verilog(\"%s\")]\n",
                                     extern_verilog_module_->code_template());
  }
  return absl::StrFormat("%s%sfn %s%s(%s)%s%s", annotation_str, pub_str,
                         name_def_->ToString(), parametric_str, params_str,
                         return_type_str, body_->ToString());
}

std::string Function::ToUndecoratedString(std::string_view identifier) const {
  std::string params_str =
      absl::StrJoin(params(), ", ", [](std::string* out, Param* param) {
        absl::StrAppend(out, param->ToString());
      });
  return absl::StrFormat("%s(%s) %s", identifier, params_str,
                         body_->ToString());
}

absl::btree_set<std::string> Function::GetFreeParametricKeySet() const {
  std::vector<std::string> keys = GetFreeParametricKeys();
  return absl::btree_set<std::string>(keys.begin(), keys.end());
}

std::vector<std::string> Function::GetFreeParametricKeys() const {
  std::vector<std::string> results;
  for (ParametricBinding* b : parametric_bindings_) {
    if (b->expr() == nullptr) {
      results.push_back(b->name_def()->identifier());
    }
  }
  return results;
}

// -- class TestFunction

TestFunction::~TestFunction() = default;

// -- class Proc

Proc::Proc(Module* owner, Span span, NameDef* name_def,
           NameDef* config_name_def, NameDef* next_name_def,
           const std::vector<ParametricBinding*>& parametric_bindings,
           std::vector<ProcMember*> members, Function* config, Function* next,
           Function* init, bool is_public)
    : AstNode(owner),
      span_(std::move(span)),
      name_def_(name_def),
      config_name_def_(config_name_def),
      next_name_def_(next_name_def),
      parametric_bindings_(parametric_bindings),
      config_(config),
      next_(next),
      init_(init),
      members_(std::move(members)),
      is_public_(is_public) {}

Proc::~Proc() = default;

std::vector<AstNode*> Proc::GetChildren(bool want_types) const {
  std::vector<AstNode*> results = {name_def()};
  for (ParametricBinding* pb : parametric_bindings_) {
    results.push_back(pb);
  }
  for (ProcMember* p : members_) {
    results.push_back(p);
  }
  results.push_back(config_);
  results.push_back(next_);
  results.push_back(init_);
  return results;
}

std::string Proc::ToString() const {
  std::string pub_str = is_public() ? "pub " : "";
  std::string parametric_str;
  if (!parametric_bindings().empty()) {
    parametric_str = absl::StrFormat(
        "<%s>",
        absl::StrJoin(
            parametric_bindings(), ", ",
            [](std::string* out, ParametricBinding* parametric_binding) {
              absl::StrAppend(out, parametric_binding->ToString());
            }));
  }
  auto param_append = [](std::string* out, const Param* p) {
    out->append(absl::StrCat(p->ToString(), ";"));
  };
  auto member_append = [](std::string* out, const ProcMember* member) {
    out->append(absl::StrCat(member->ToString(), ";"));
  };
  std::string config_params_str =
      absl::StrJoin(config_->params(), ", ", param_append);
  std::string state_params_str =
      absl::StrJoin(next_->params(), ", ", param_append);
  std::string members_str = absl::StrJoin(members_, "\n", member_append);
  if (!members_str.empty()) {
    members_str.append("\n");
  }

  // Init functions are special, since they shouldn't be printed with
  // parentheses (since they can't take args).
  std::string init_str = Indent(
      absl::StrCat("init ", init_->body()->ToString()), kRustSpacesPerIndent);

  constexpr std::string_view kTemplate = R"(%sproc %s%s {
%s%s
%s
%s
})";
  return absl::StrFormat(
      kTemplate, pub_str, name_def()->identifier(), parametric_str,
      Indent(members_str, kRustSpacesPerIndent),
      Indent(config_->ToUndecoratedString("config"), kRustSpacesPerIndent),
      init_str,
      Indent(next_->ToUndecoratedString("next"), kRustSpacesPerIndent));
}

std::vector<std::string> Proc::GetFreeParametricKeys() const {
  // TODO(rspringer): 2021-09-29: Mutants found holes in test coverage here.
  std::vector<std::string> results;
  for (ParametricBinding* b : parametric_bindings_) {
    if (b->expr() == nullptr) {
      results.push_back(b->name_def()->identifier());
    }
  }
  return results;
}

// -- class MatchArm

MatchArm::MatchArm(Module* owner, Span span, std::vector<NameDefTree*> patterns,
                   Expr* expr)
    : AstNode(owner),
      span_(std::move(span)),
      patterns_(std::move(patterns)),
      expr_(expr) {
  XLS_CHECK(!patterns_.empty());
}

MatchArm::~MatchArm() = default;

Span MatchArm::GetPatternSpan() const {
  return Span(patterns_[0]->span().start(), patterns_.back()->span().limit());
}

Match::Match(Module* owner, Span span, Expr* matched,
             std::vector<MatchArm*> arms)
    : Expr(owner, std::move(span)), matched_(matched), arms_(std::move(arms)) {}

// -- class NameRef

NameRef::~NameRef() = default;

// -- class ConstRef

ConstRef::~ConstRef() = default;

// -- class Range

Range::Range(Module* owner, Span span, Expr* start, Expr* end)
    : Expr(owner, std::move(span)), start_(start), end_(end) {}

Range::~Range() = default;

std::string Range::ToStringInternal() const {
  return absl::StrFormat("%s..%s", start_->ToString(), end_->ToString());
}

// -- class Cast

Cast::~Cast() = default;

std::string Cast::ToStringInternal() const {
  std::string lhs = expr_->ToString();
  Precedence arg_precedence = expr_->GetPrecedence();
  if (WeakerThan(arg_precedence, Precedence::kAs)) {
    XLS_VLOG(10) << absl::StreamFormat(
        "expr `%s` precedence: %s weaker than 'as'", lhs,
        PrecedenceToString(arg_precedence));
    Parenthesize(&lhs);
  }
  return absl::StrFormat("%s as %s", lhs, type_annotation_->ToString());
}

// -- class TestProc

TestProc::~TestProc() = default;

std::string TestProc::ToString() const {
  return absl::StrFormat("#[test_proc]\n%s", proc_->ToString());
}

// -- class BuiltinTypeAnnotation

BuiltinTypeAnnotation::BuiltinTypeAnnotation(Module* owner, Span span,
                                             BuiltinType builtin_type,
                                             BuiltinNameDef* builtin_name_def)
    : TypeAnnotation(owner, std::move(span)),
      builtin_type_(builtin_type),
      builtin_name_def_(builtin_name_def) {}

BuiltinTypeAnnotation::~BuiltinTypeAnnotation() = default;

std::vector<AstNode*> BuiltinTypeAnnotation::GetChildren(
    bool want_types) const {
  return std::vector<AstNode*>{};
}

int64_t BuiltinTypeAnnotation::GetBitCount() const {
  return GetBuiltinTypeBitCount(builtin_type_).value();
}

bool BuiltinTypeAnnotation::GetSignedness() const {
  return GetBuiltinTypeSignedness(builtin_type_).value();
}

// -- class ChannelTypeAnnotation

ChannelTypeAnnotation::ChannelTypeAnnotation(
    Module* owner, Span span, ChannelDirection direction,
    TypeAnnotation* payload, std::optional<std::vector<Expr*>> dims)
    : TypeAnnotation(owner, std::move(span)),
      direction_(direction),
      payload_(payload),
      dims_(dims) {}

ChannelTypeAnnotation::~ChannelTypeAnnotation() = default;

std::string ChannelTypeAnnotation::ToString() const {
  std::vector<std::string> dims;
  if (dims_.has_value()) {
    for (const Expr* dim : dims_.value()) {
      dims.push_back(absl::StrCat("[", dim->ToString(), "]"));
    }
  }
  return absl::StrFormat("chan<%s>%s %s", payload_->ToString(),
                         absl::StrJoin(dims, ""),
                         direction_ == ChannelDirection::kIn ? "in" : "out");
}

// -- class TupleTypeAnnotation

TupleTypeAnnotation::TupleTypeAnnotation(Module* owner, Span span,
                                         std::vector<TypeAnnotation*> members)
    : TypeAnnotation(owner, std::move(span)), members_(std::move(members)) {}

TupleTypeAnnotation::~TupleTypeAnnotation() = default;

std::string TupleTypeAnnotation::ToString() const {
  std::string guts =
      absl::StrJoin(members_, ", ", [](std::string* out, TypeAnnotation* t) {
        absl::StrAppend(out, t->ToString());
      });
  return absl::StrFormat("(%s%s)", guts, members_.size() == 1 ? "," : "");
}

// -- class Statement

/* static */ absl::StatusOr<std::variant<Expr*, TypeAlias*, Let*, ConstAssert*>>
Statement::NodeToWrapped(AstNode* n) {
  if (auto* e = dynamic_cast<Expr*>(n)) {
    return e;
  }
  if (auto* t = dynamic_cast<TypeAlias*>(n)) {
    return t;
  }
  if (auto* l = dynamic_cast<Let*>(n)) {
    return l;
  }
  if (auto* d = dynamic_cast<ConstAssert*>(n)) {
    return d;
  }
  return absl::InvalidArgumentError(absl::StrCat(
      "AST node could not be wrapped in a statement: ", n->GetNodeTypeName()));
}

Statement::Statement(Module* owner, Statement::Wrapped wrapped)
    : AstNode(owner), wrapped_(wrapped) {
  XLS_CHECK_NE(ToAstNode(wrapped_), this);
}

std::optional<Span> Statement::GetSpan() const {
  AstNode* wrapped = ToAstNode(wrapped_);
  XLS_CHECK_NE(wrapped, nullptr);
  XLS_CHECK_NE(wrapped, this);
  return wrapped->GetSpan();
}

// -- class WildcardPattern

WildcardPattern::~WildcardPattern() = default;

// -- class QuickCheck

QuickCheck::QuickCheck(Module* owner, Span span, Function* f,
                       std::optional<int64_t> test_count)
    : AstNode(owner), span_(std::move(span)), f_(f), test_count_(test_count) {}

QuickCheck::~QuickCheck() = default;

std::string QuickCheck::ToString() const {
  std::string test_count_str;
  if (test_count_.has_value()) {
    test_count_str = absl::StrFormat("(test_count=%d)", *test_count_);
  }
  return absl::StrFormat("#[quickcheck%s]\n%s", test_count_str, f_->ToString());
}

// -- class TupleIndex

TupleIndex::~TupleIndex() = default;

TupleIndex::TupleIndex(Module* owner, Span span, Expr* lhs, Number* index)
    : Expr(owner, std::move(span)), lhs_(lhs), index_(index) {}

absl::Status TupleIndex::Accept(AstNodeVisitor* v) const {
  return v->HandleTupleIndex(this);
}

absl::Status TupleIndex::AcceptExpr(ExprVisitor* v) const {
  return v->HandleTupleIndex(this);
}

std::string TupleIndex::ToStringInternal() const {
  return absl::StrCat(lhs_->ToString(), ".", index_->ToString());
}

std::vector<AstNode*> TupleIndex::GetChildren(bool want_types) const {
  return {lhs_, index_};
}

// -- class XlsTuple

XlsTuple::~XlsTuple() = default;

std::string XlsTuple::ToStringInternal() const {
  std::string result = "(";
  for (int64_t i = 0; i < members_.size(); ++i) {
    absl::StrAppend(&result, members_[i]->ToString());
    if (i != members_.size() - 1) {
      absl::StrAppend(&result, ", ");
    }
  }
  if (members_.size() == 1 || has_trailing_comma()) {
    // Singleton tuple requires a trailing comma to avoid being parsed as a
    // parenthesized expression.
    absl::StrAppend(&result, ",");
  }
  absl::StrAppend(&result, ")");
  return result;
}

std::string StructRefToText(const StructRef& struct_ref) {
  if (std::holds_alternative<StructDef*>(struct_ref)) {
    return std::get<StructDef*>(struct_ref)->identifier();
  }
  if (std::holds_alternative<ColonRef*>(struct_ref)) {
    return std::get<ColonRef*>(struct_ref)->ToString();
  }
  XLS_LOG(FATAL)
      << "Unhandled alternative for converting struct reference to string.";
}

// -- class NameDefTree

NameDefTree::~NameDefTree() = default;

std::vector<AstNode*> NameDefTree::GetChildren(bool want_types) const {
  if (std::holds_alternative<Leaf>(tree_)) {
    return {ToAstNode(std::get<Leaf>(tree_))};
  }
  const Nodes& nodes = std::get<Nodes>(tree_);
  return ToAstNodes<NameDefTree>(nodes);
}

std::string NameDefTree::ToString() const {
  if (is_leaf()) {
    return ToAstNode(leaf())->ToString();
  }

  std::string guts =
      absl::StrJoin(nodes(), ", ", [](std::string* out, NameDefTree* node) {
        absl::StrAppend(out, node->ToString());
      });
  return absl::StrFormat("(%s)", guts);
}

std::vector<NameDefTree::Leaf> NameDefTree::Flatten() const {
  if (is_leaf()) {
    return {leaf()};
  }
  std::vector<Leaf> results;
  for (const NameDefTree* node : std::get<Nodes>(tree_)) {
    auto node_leaves = node->Flatten();
    results.insert(results.end(), node_leaves.begin(), node_leaves.end());
  }
  return results;
}

std::vector<NameDef*> NameDefTree::GetNameDefs() const {
  std::vector<NameDef*> results;
  for (Leaf leaf : Flatten()) {
    if (std::holds_alternative<NameDef*>(leaf)) {
      results.push_back(std::get<NameDef*>(leaf));
    }
  }
  return results;
}

std::vector<std::variant<NameDefTree::Leaf, NameDefTree*>>
NameDefTree::Flatten1() const {
  if (is_leaf()) {
    return {leaf()};
  }
  std::vector<std::variant<Leaf, NameDefTree*>> result;
  for (NameDefTree* ndt : nodes()) {
    if (ndt->is_leaf()) {
      result.push_back(ndt->leaf());
    } else {
      result.push_back(ndt);
    }
  }
  return result;
}

// -- class Let

Let::Let(Module* owner, Span span, NameDefTree* name_def_tree,
         TypeAnnotation* type_annotation, Expr* rhs, bool is_const)
    : AstNode(owner),
      span_(std::move(span)),
      name_def_tree_(name_def_tree),
      type_annotation_(type_annotation),
      rhs_(rhs),
      is_const_(is_const) {}

Let::~Let() = default;

std::vector<AstNode*> Let::GetChildren(bool want_types) const {
  std::vector<AstNode*> results = {name_def_tree_};
  if (type_annotation_ != nullptr && want_types) {
    results.push_back(type_annotation_);
  }
  results.push_back(rhs_);
  return results;
}

std::string Let::ToString() const {
  return absl::StrFormat(
      "%s %s%s = %s;", is_const_ ? "const" : "let", name_def_tree_->ToString(),
      type_annotation_ == nullptr ? "" : ": " + type_annotation_->ToString(),
      rhs_->ToString());
}

// -- class Expr

Expr::~Expr() = default;

std::string Expr::ToString() const {
  std::string s = ToStringInternal();
  if (in_parens()) {
    Parenthesize(&s);
  }
  return s;
}

// -- class String

String::~String() = default;

// -- class Number

Number::Number(Module* owner, Span span, std::string text,
               NumberKind number_kind, TypeAnnotation* type_annotation)
    : Expr(owner, std::move(span)),
      text_(std::move(text)),
      number_kind_(number_kind),
      type_annotation_(type_annotation) {}

Number::~Number() = default;

std::vector<AstNode*> Number::GetChildren(bool want_types) const {
  if (type_annotation_ == nullptr) {
    return {};
  }
  return {type_annotation_};
}

std::string Number::ToStringInternal() const {
  std::string formatted_text = text_;
  if (number_kind_ == NumberKind::kCharacter) {
    if (text_[0] == '\'' || text_[0] == '\\') {
      formatted_text = absl::StrCat(R"(\)", formatted_text);
    }
    formatted_text = absl::StrCat("'", formatted_text, "'");
  }
  if (type_annotation_ != nullptr) {
    return absl::StrFormat("%s:%s", type_annotation_->ToString(),
                           formatted_text);
  }
  return formatted_text;
}

std::string Number::ToStringNoType() const { return text_; }

absl::StatusOr<bool> Number::FitsInType(int64_t bit_count) const {
  XLS_RET_CHECK_GE(bit_count, 0);
  switch (number_kind_) {
    case NumberKind::kBool:
      return bit_count >= 1;
    case NumberKind::kCharacter:
      return bit_count >= CHAR_BIT;
    case NumberKind::kOther: {
      XLS_ASSIGN_OR_RETURN(auto sm, GetSignAndMagnitude(text_));
      auto [sign, bits] = sm;
      return bit_count >= bits.bit_count();
    }
  }
  return absl::InternalError(
      absl::StrFormat("Unreachable; invalid number kind: %d", number_kind_));
}

absl::StatusOr<Bits> Number::GetBits(int64_t bit_count) const {
  XLS_RET_CHECK_GE(bit_count, 0);
  switch (number_kind_) {
    case NumberKind::kBool: {
      Bits result(bit_count);
      return result.UpdateWithSet(0, text_ == "true");
    }
    case NumberKind::kCharacter: {
      XLS_RET_CHECK_EQ(text_.size(), 1);
      Bits result = Bits::FromBytes(/*bytes=*/{static_cast<uint8_t>(text_[0])},
                                    /*bit_count=*/CHAR_BIT);
      return bits_ops::ZeroExtend(result, bit_count);
    }
    case NumberKind::kOther: {
      XLS_ASSIGN_OR_RETURN(auto sm, GetSignAndMagnitude(text_));
      auto [sign, bits] = sm;
      XLS_RET_CHECK_GE(bits.bit_count(), 0);
      XLS_RET_CHECK(bit_count >= bits.bit_count()) << absl::StreamFormat(
          "Internal error: %s Cannot fit number value %s in %d bits; %d "
          "required: `%s`",
          span().ToString(), text_, bit_count, bits.bit_count(), ToString());
      bits = bits_ops::ZeroExtend(bits, bit_count);
      if (sign) {
        bits = bits_ops::Negate(bits);
      }
      return bits;
    }
  }
  return absl::InternalError(absl::StrFormat(
      "Invalid NumberKind: %d", static_cast<int64_t>(number_kind_)));
}

TypeAlias::TypeAlias(Module* owner, Span span, NameDef* name_def,
                     TypeAnnotation* type, bool is_public)
    : AstNode(owner),
      span_(std::move(span)),
      name_def_(name_def),
      type_annotation_(type),
      is_public_(is_public) {}

TypeAlias::~TypeAlias() = default;

// -- class Array

Array::~Array() = default;

std::string Array::ToStringInternal() const {
  std::string type_prefix;
  if (type_annotation_ != nullptr) {
    type_prefix = absl::StrCat(type_annotation_->ToString(), ":");
  }
  return absl::StrFormat("%s[%s%s]", type_prefix,
                         absl::StrJoin(members_, ", ",
                                       [](std::string* out, Expr* expr) {
                                         absl::StrAppend(out, expr->ToString());
                                       }),
                         has_ellipsis_ ? ", ..." : "");
}

std::vector<AstNode*> Array::GetChildren(bool want_types) const {
  std::vector<AstNode*> results;
  if (want_types && type_annotation_ != nullptr) {
    results.push_back(type_annotation_);
  }
  for (Expr* member : members_) {
    XLS_CHECK(member != nullptr);
    results.push_back(member);
  }
  return results;
}

std::vector<AstNode*> Statement::GetChildren(bool want_types) const {
  return {ToAstNode(wrapped_)};
}

Span ExprOrTypeSpan(const ExprOrType &expr_or_type) {
  return absl::visit(Visitor{
    [](Expr* expr) { return expr->span(); },
    [](TypeAnnotation* type) { return type->span(); },
  }, expr_or_type);
}

absl::StatusOr<std::vector<AstNode*>> CollectUnder(AstNode* root,
                                                   bool want_types) {
  std::vector<AstNode*> nodes;

  class CollectVisitor : public AstNodeVisitor {
   public:
    explicit CollectVisitor(std::vector<AstNode*>& nodes) : nodes_(nodes) {}

#define DECLARE_HANDLER(__type)                           \
  absl::Status Handle##__type(const __type* n) override { \
    nodes_.push_back(const_cast<__type*>(n));             \
    return absl::OkStatus();                              \
  }
    XLS_DSLX_AST_NODE_EACH(DECLARE_HANDLER)
#undef DECLARE_HANDLER

   private:
    std::vector<AstNode*>& nodes_;
  } collect_visitor(nodes);

  XLS_RETURN_IF_ERROR(WalkPostOrder(root, &collect_visitor, want_types));
  return nodes;
}

}  // namespace xls::dslx
