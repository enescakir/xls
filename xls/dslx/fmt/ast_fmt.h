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

#ifndef XLS_DSLX_FMT_AST_FMT_H_
#define XLS_DSLX_FMT_AST_FMT_H_

#include <cstdint>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"
#include "xls/dslx/fmt/pretty_print.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/comment_data.h"
#include "xls/dslx/frontend/pos.h"

namespace xls::dslx {

// API convenience wrapper around comment data that the scanner produces -- this
// allows us to look up "what comments an AST node is responsible for" via
// `GetComments()` providing the AST node span.
class Comments {
 public:
  static Comments Create(absl::Span<const CommentData> comments);

  // Returns all the comments related to the given `node_span`.
  //
  // This is a convenient way for nodes to query for all their related comments.
  std::vector<const CommentData*> GetComments(const Span& node_span) const;

 private:
  explicit Comments(absl::flat_hash_map<int64_t, CommentData> line_to_comment)
      : line_to_comment_(std::move(line_to_comment)) {}

  absl::flat_hash_map<int64_t, CommentData> line_to_comment_;
};

// Functions with this signature create a pretty printable document from the AST
// node "n".

DocRef Fmt(const Statement& n, const Comments& comments, DocArena& arena);

DocRef Fmt(const Function& n, const Comments& comments, DocArena& arena);

}  // namespace xls::dslx

#endif  // XLS_DSLX_FMT_AST_FMT_H_
