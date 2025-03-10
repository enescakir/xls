// Copyright 2023 The XLS Authors
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

// Pretty-printing entities. This basic structure is
// similar to the declarative-specification-like mini-language pioneered by
// Wadler et al.

#ifndef XLS_DSLX_FMT_PRETTY_PRINT_H_
#define XLS_DSLX_FMT_PRETTY_PRINT_H_

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "absl/types/span.h"
#include "xls/common/strong_int.h"

namespace xls::dslx {

// A reference to a doc (pretty printable object type) within an arena.
XLS_DEFINE_STRONG_INT_TYPE(DocRef, uint32_t);

namespace pprint_internal {

// Represents a requirement (number of characters required in the line for
// things to fit in their flattened form). For a hard line break the requirement
// is infinite, which is represented via the std::monostate.
//
// Note: this is a bit more cumbersome than using std::optional, and that's kind
// of on purpose, because we want to remind the implementation that's not a
// /lack/ of data it's that the value is /actually infinity/. In a language with
// sum types it'd be `type Requirement = Inf | Num int`
using Requirement = std::variant<int64_t, std::monostate>;

// Shorthand for creating the monostate variant (representing infinite line
// width requirement, i.e. a hard line break) as described above.
inline Requirement InfinityRequirement() { return std::monostate{}; }

// Command for the pretty printer that says we should insert a newline.
struct HardLine {};

// Command for the pretty printer that says, if we're in flat mode, emit
// on_flat, and if we're in break mode, emit on_break.
struct FlatChoice {
  DocRef on_flat;
  DocRef on_break;
};

// Command for the pretty printer that says, if we can emit arg in flat mode, do
// so, otherwise switch into break mode.
struct Group {
  DocRef arg;
};

// Command for the pretty printer that says we should concatenate the two given
// sub-docs.
struct Concat {
  DocRef lhs;
  DocRef rhs;
};

// Command for the pretty printer that says we should nest the doc "arg" at an
// indent of "delta" spaces.
struct Nest {
  int64_t delta;
  DocRef arg;
};

// Command for the pretty printer that says we should set the indent to the
// current column offset and emit "arg" within that indentation.
//
// Note that if you align very close to the text width this can make things very
// ragged, so you may want to use this sparingly (or we could create a facility
// to select between alternative emissions so as not to run very close to the
// ragged edge of the text width).
struct Align {
  DocRef arg;
};

// The basic entity used for pretty printing -- a "doc" has a requirement for
// how many chars it needs to be emitted in flat mode (determined at
// construction time) and a payload (e.g. for things like sub-documents, see
// variants above).
struct Doc {
  // All document entities have a pre-computed flat requirement that's been
  // determined at construction time.
  Requirement flat_requirement;

  // The value can carry more information on what to do in flat/break
  // situations, or nested documents within commands.
  std::variant<std::string, HardLine, FlatChoice, Group, Concat, Nest, Align>
      value;
};

}  // namespace pprint_internal

// Object that holds document entities and provides some very common ones via
// accessors. Compound pretty printed docs can be built up using the factories
// on this object.
class DocArena {
 public:
  DocArena();

  // Creates a literal text string as a document.
  //
  // Note: text string should not include newline characters, those should be
  // managed by sequences like break0() or break1().
  DocRef MakeText(std::string s);

  // Creates a "group" doc -- groups are attempted to be emitted flat as a unit,
  // or if they can't be, the document emitter switches to "break" mode (i.e.
  // line break emission mode) for the scope of emitting "arg_ref".
  //
  // That is, making a group evaluates whether the things within the group can
  // be emitted in flat mode or whether we should switch to break mode.
  //
  // Combining "Group" and "FlatChoice" allows us to say "I'd like to emit this
  // inline if in flat mode" and give an alternative option for how to emit it
  // when we're not in flat mode.
  DocRef MakeGroup(DocRef arg_ref);

  // Creates a "nest" doc that nests "arg_ref" by "delta" spaces.
  DocRef MakeNest(DocRef arg_ref, int64_t delta = 4);

  // Creates a "concat" doc that concatenates lhs and rhs.
  DocRef MakeConcat(DocRef lhs, DocRef rhs);

  // Creates a "flat choice" doc that provides different possibilities (based on
  // whether it appears we'll fit into one line with the on_flat choice, which
  // is preferred).
  DocRef MakeFlatChoice(DocRef on_flat, DocRef on_break);

  // Empty string.
  DocRef empty() const { return empty_; }

  // Single space string.
  DocRef space() const { return space_; }

  // Hard line break (forces a line break).
  DocRef hard_line() const { return hard_line_; }

  // Either a empty string or a hard line break depending on whether we're in
  // flat mode.
  DocRef break0() const { return break0_; }

  // Either a single space or a hard line break depending on whether we're in
  // flat mode.
  DocRef break1() const { return break1_; }

  // Some helpful text to have pre-defined and common.
  DocRef oparen() const { return oparen_; }
  DocRef cparen() const { return cparen_; }
  DocRef comma() const { return comma_; }
  DocRef colon() const { return colon_; }
  DocRef equals() const { return equals_; }
  DocRef dotdot() const { return dotdot_; }
  DocRef underscore() const { return underscore_; }
  DocRef slash_slash() const { return slash_slash_; }
  DocRef ocurl() const { return ocurl_; }
  DocRef ccurl() const { return ccurl_; }
  DocRef semi() const { return semi_; }
  DocRef arrow() const { return arrow_; }

  // Note: the returned reference should not be held across an allocation.
  const pprint_internal::Doc& Deref(DocRef ref) const {
    return items_[uint16_t{ref}];
  }

 private:
  // Note: we use reference indices so we can realloc inline data (instead of
  // boxing everything) and to avoid the variant type being recursive.
  std::vector<pprint_internal::Doc> items_;

  DocRef empty_;
  DocRef space_;
  DocRef hard_line_;
  DocRef break0_;
  DocRef break1_;

  // Some convenient often-used text fragments.
  DocRef oparen_;
  DocRef cparen_;
  DocRef comma_;
  DocRef colon_;
  DocRef equals_;
  DocRef dotdot_;
  DocRef underscore_;
  DocRef slash_slash_;
  DocRef ocurl_;
  DocRef ccurl_;
  DocRef semi_;
  DocRef arrow_;
};

// Helper for concatenating several docs together in left-to-right sequence.
DocRef ConcatN(DocArena& arena, absl::Span<DocRef const> docs);

// Concatenates the docs as in ConcatN and then makes a group around them.
//
// See MakeGroup() for the implications of putting something in a group.
DocRef ConcatNGroup(DocArena& arena, absl::Span<DocRef const> docs);

// The pretty printing routine itself that reflows lines in "doc" to attempt to
// fit them within "text_width".
//
// Note that it's not guaranteed the resulting lines will fit within text_width,
// they need to be defined to be reflowable in a manner that can avoid the text
// width limit.
std::string PrettyPrint(const DocArena& arena, DocRef ref, int64_t text_width);

}  // namespace xls::dslx

#endif  // XLS_DSLX_FMT_PRETTY_PRINT_H_
