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

#include "xls/passes/context_sensitive_range_query_engine.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/data_structures/inline_bitmap.h"
#include "xls/data_structures/leaf_type_tree.h"
#include "xls/ir/bits.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/dfs_visitor.h"
#include "xls/ir/function_base.h"
#include "xls/ir/interval.h"
#include "xls/ir/interval_ops.h"
#include "xls/ir/interval_set.h"
#include "xls/ir/node.h"
#include "xls/ir/node_iterator.h"
#include "xls/ir/node_util.h"
#include "xls/ir/nodes.h"
#include "xls/ir/op.h"
#include "xls/ir/ternary.h"
#include "xls/ir/type.h"
#include "xls/passes/predicate_state.h"
#include "xls/passes/query_engine.h"
#include "xls/passes/range_query_engine.h"

namespace xls {

namespace {

// A canonical representation of a range check holding the low-boundary,
// variable, and high-boundary.
struct CanonicalRange {
  // The low value which 'param' is compared to.
  Node* low_value;
  // The cmp used to compare low-value with param. Executed as `(low_cmp
  // param low_value)`. That is low_value is on the left. This is one of 'SGt',
  // 'UGt', 'SGe', or 'UGe'.
  Op low_cmp;
  // The parameter which is being constrained by the range.
  Node* param;
  // The cmp used to compare low-value with param. Executed as `(high_cmp
  // param high_value)`. That is low_value is on the left. This is one of 'SLt',
  // 'ULt', 'SLe', or 'ULe'.
  Op high_cmp;
  // The high value which 'param' is compared to.
  Node* high_value;

  // The actual instruction which implements the (low_cmp low_value param)
  // operation.
  CompareOp* low_range;
  // The actual instruction which implements the (high_cmp high_value param)
  // operation.
  CompareOp* high_range;
};

// Class which can back-propagate node ranges.
//
// This is currently limited to a single step.
class BackPropagate final : public DfsVisitorWithDefault {
 public:
  explicit BackPropagate(const RangeQueryEngine& base) : base_(base) {}
  absl::flat_hash_map<Node*, RangeData> ranges() const { return result_; }
  void AddGiven(Node* n, RangeData data) { result_[n] = std::move(data); }
  absl::Status DefaultHandler(Node* node) final { return absl::OkStatus(); }
  absl::Status HandleNe(CompareOp* ne) final { return UnifyExactMatch(ne); }
  absl::Status HandleEq(CompareOp* eq) final { return UnifyExactMatch(eq); }
  absl::Status HandleSGe(CompareOp* cmp) final { return UnifyComparison(cmp); }
  absl::Status HandleSGt(CompareOp* cmp) final { return UnifyComparison(cmp); }
  absl::Status HandleSLe(CompareOp* cmp) final { return UnifyComparison(cmp); }
  absl::Status HandleSLt(CompareOp* cmp) final { return UnifyComparison(cmp); }
  absl::Status HandleUGe(CompareOp* cmp) final { return UnifyComparison(cmp); }
  absl::Status HandleUGt(CompareOp* cmp) final { return UnifyComparison(cmp); }
  absl::Status HandleULe(CompareOp* cmp) final { return UnifyComparison(cmp); }
  absl::Status HandleULt(CompareOp* cmp) final { return UnifyComparison(cmp); }
  absl::Status HandleNaryAnd(NaryOp* and_op) final {
    return MaybeUnifyAnd(and_op);
  }

 private:
  absl::Status MaybeUnifyAnd(NaryOp* and_op) {
    // To simplify the unification logic we only handle a single 'range' op
    // This is to catch the dslx a..b match type.
    // TODO(allight): 2023-09-07 We could do better in the positive case since
    // we know everything is true.
    if (and_op->operand_count() != 2) {
      return absl::OkStatus();
    }
    auto canonical_range = ExtractRange(and_op->operand(0), and_op->operand(1));
    if (canonical_range) {
      return UnifyRangeComparison(
          *canonical_range, ternary_ops::IsKnownOne(*result_[and_op].ternary));
    }
    return absl::OkStatus();
  }

  // Extract the CanonicalRange comparison out of the two and'd comparisons.
  //
  // Returns nullopt if the elements do not form a range check.
  std::optional<CanonicalRange> ExtractRange(Node* element_one,
                                             Node* element_two) {
    const std::array<Op, 8> cmp_ops{
        Op::kSLe, Op::kSLt, Op::kSGe, Op::kSGt,
        Op::kULe, Op::kULt, Op::kUGe, Op::kUGt,
    };
    if (!element_one->OpIn(cmp_ops) || !element_two->OpIn(cmp_ops)) {
      return std::nullopt;
    }
    // canonicalize both to
    // (<OP> <COMMON> <DIFFERENT>)
    // A range check is 'x in range(start, end)' this is represented in the IR
    // as (and (< start x) (< x end)). To simplify handling we make both ends
    // ordered in the 'x' 'start/end' direction.
    Op e1_op;
    Op e2_op;
    Node* e1_comparator;
    Node* e2_comparator;
    Node* common;
    if (element_one->operand(0) == element_two->operand(0)) {
      // Already in canonical order
      common = element_one->operand(0);
      e1_op = element_one->op();
      e2_op = element_two->op();
      e1_comparator = element_one->operand(1);
      e2_comparator = element_two->operand(1);
    } else if (element_one->operand(1) == element_two->operand(0)) {
      // element2 in canonical order
      common = element_one->operand(1);
      e1_op = *ReverseComparisonOp(element_one->op());
      e2_op = element_two->op();
      e1_comparator = element_one->operand(0);
      e2_comparator = element_two->operand(1);
    } else if (element_one->operand(0) == element_two->operand(1)) {
      // element1 in canonical order
      common = element_one->operand(0);
      e1_op = element_one->op();
      e2_op = *ReverseComparisonOp(element_two->op());
      e1_comparator = element_one->operand(1);
      e2_comparator = element_two->operand(0);
    } else if (element_one->operand(1) == element_two->operand(1)) {
      // both in reversed order
      common = element_one->operand(1);
      e1_op = *ReverseComparisonOp(element_one->op());
      e2_op = *ReverseComparisonOp(element_two->op());
      e1_comparator = element_one->operand(0);
      e2_comparator = element_two->operand(0);
    } else {
      // Not a range, no common comparator.
      return std::nullopt;
    }
    // order the operations
    std::array<Op, 4> low_ops{Op::kSGe, Op::kSGt, Op::kUGe, Op::kUGt};
    std::array<Op, 4> high_ops{Op::kSLe, Op::kSLt, Op::kULe, Op::kULt};
    if (absl::c_find(low_ops, e1_op) != low_ops.cend() &&
        absl::c_find(high_ops, e2_op) != high_ops.cend()) {
      return CanonicalRange{
          .low_value = e1_comparator,
          .low_cmp = e1_op,
          .param = common,
          .high_cmp = e2_op,
          .high_value = e2_comparator,
          .low_range = element_one->As<CompareOp>(),
          .high_range = element_two->As<CompareOp>(),
      };
    }
    if (absl::c_find(high_ops, e1_op) != high_ops.cend() &&
        absl::c_find(low_ops, e2_op) != low_ops.cend()) {
      return CanonicalRange{
          .low_value = e2_comparator,
          .low_cmp = e2_op,
          .param = common,
          .high_cmp = e1_op,
          .high_value = e1_comparator,
          .low_range = element_two->As<CompareOp>(),
          .high_range = element_one->As<CompareOp>(),
      };
    }
    return std::nullopt;
  }

  // Extract interval sets from the range given the range check succeeds or
  // fails (value_is_in_range).
  absl::Status UnifyRangeComparison(const CanonicalRange& range,
                                    bool value_is_in_range) {
    IntervalSet low_interval =
        base_.GetIntervalSetTree(range.low_value).Get({});
    IntervalSet high_interval =
        base_.GetIntervalSetTree(range.high_value).Get({});
    IntervalSet base_interval = base_.GetIntervalSetTree(range.param).Get({});
    bool left_is_open = range.low_cmp == Op::kSGt || range.low_cmp == Op::kUGt;
    bool right_is_open =
        range.high_cmp == Op::kSLt || range.high_cmp == Op::kULt;
    IntervalSet range_interval(base_interval.BitCount());
    if (left_is_open && right_is_open) {
      range_interval.AddInterval(Interval::Open(*low_interval.LowerBound(),
                                                *high_interval.UpperBound()));
    } else if (left_is_open && !right_is_open) {
      range_interval.AddInterval(Interval::LeftOpen(
          *low_interval.LowerBound(), *high_interval.UpperBound()));
    } else if (!left_is_open && right_is_open) {
      range_interval.AddInterval(Interval::RightOpen(
          *low_interval.LowerBound(), *high_interval.UpperBound()));
    } else {
      range_interval.AddInterval(Interval::Closed(*low_interval.LowerBound(),
                                                  *high_interval.UpperBound()));
    }
    range_interval.Normalize();
    if (value_is_in_range) {
      // Value is in range, intersect with range.
      IntervalSetTree true_tree(range.low_range->GetType());
      true_tree.Set({}, IntervalSet::Precise(Bits::AllOnes(1)));
      RangeData true_range{
          .ternary = ternary_ops::BitsToTernary(Bits::AllOnes(1)),
          .interval_set = true_tree};
      result_[range.low_range] = true_range;
      result_[range.high_range] = true_range;
      IntervalSet constrained_param =
          IntervalSet::Intersect(base_interval, range_interval);
      result_[range.param] =
          RangeData{.ternary = interval_ops::ExtractTernaryVector(
                        constrained_param, range.param),
                    .interval_set = IntervalSetTree(range.param->GetType(),
                                                    constrained_param)};
    } else {
      // Outside of range, add the inverse.
      IntervalSet constrained_param = IntervalSet::Intersect(
          base_interval, IntervalSet::Complement(range_interval));
      result_[range.param] =
          RangeData{.ternary = interval_ops::ExtractTernaryVector(
                        constrained_param, range.param),
                    .interval_set = IntervalSetTree(range.param->GetType(),
                                                    constrained_param)};
    }
    return absl::OkStatus();
  }

  absl::Status UnifyComparison(CompareOp* cmp) {
    XLS_RET_CHECK(cmp->op() == Op::kSLe || cmp->op() == Op::kSLt ||
                  cmp->op() == Op::kSGe || cmp->op() == Op::kSGt ||
                  cmp->op() == Op::kULe || cmp->op() == Op::kULt ||
                  cmp->op() == Op::kUGe || cmp->op() == Op::kUGt)
        << cmp;
    XLS_RET_CHECK(result_[cmp].ternary.has_value() &&
                  ternary_ops::IsFullyKnown(*result_[cmp].ternary))
        << "selector " << cmp
        << " not given actual value during context sensitive range analysis!";
    // Standardize so we are assuming the comparison is true.
    XLS_ASSIGN_OR_RETURN(Op invert, InvertComparisonOp(cmp->op()));
    Op op = ternary_ops::IsKnownOne(*result_[cmp].ternary) ? cmp->op() : invert;
    Node* l_op = cmp->operand(0);
    Node* r_op = cmp->operand(1);
    IntervalSet l_interval = base_.GetIntervalSetTree(l_op).Get({});
    IntervalSet r_interval = base_.GetIntervalSetTree(r_op).Get({});
    if (!l_interval.IsPrecise() && !r_interval.IsPrecise()) {
      return UnifyImpreciseComparison(l_op, r_op, l_interval, r_interval);
    }
    // Standardize so right side is always precise.
    bool is_signed =
        op == Op::kSLe || op == Op::kSLt || op == Op::kSGe || op == Op::kSGt;
    bool is_or_equals =
        op == Op::kULe || op == Op::kUGe || op == Op::kSLe || op == Op::kSGe;
    bool is_less_than =
        op == Op::kSLe || op == Op::kULe || op == Op::kSLt || op == Op::kULt;
    if (l_interval.IsPrecise()) {
      // We want to ensure that the constant is always on the right to simplify
      // UnifyLiteralComparison. This requires doing a transform 'ReverseOp'
      // such that '(op L R) == ((ReverseOp op) R L)'. The transform that works
      // for this is replacing '</<=' with '>/>=' and vice versa.
      is_less_than = !is_less_than;
      std::swap(l_op, r_op);
      std::swap(l_interval, r_interval);
    }
    return UnifyLiteralComparison(l_op, l_interval,
                                  r_interval.GetPreciseValue().value(),
                                  is_or_equals, is_less_than, is_signed);
  }

  absl::Status UnifyLiteralComparison(Node* variable, const IntervalSet& base,
                                      Bits literal, bool is_or_equals,
                                      bool is_less_than, bool is_signed) {
    // Invert so we can remove elements from the interval.
    IntervalSet invert_base = IntervalSet::Complement(base);
    Bits min_value = is_signed ? Bits::MinSigned(literal.bit_count())
                               : Bits(literal.bit_count());
    Bits max_value = is_signed ? Bits::MaxSigned(literal.bit_count())
                               : Bits::AllOnes(literal.bit_count());
    Bits epsilon = UBits(1, literal.bit_count());
    if (is_less_than) {
      if (is_or_equals) {
        // variable <= literal
        if (literal == max_value) {
          // Nothing to restrict.
          // is V <= std::numeric_limits::max(). Always true.
          return absl::OkStatus();
        }
        literal = bits_ops::Add(literal, epsilon);
      }
      // variable < literal
      invert_base.AddInterval(Interval(literal, max_value));
    } else {
      if (is_or_equals) {
        // variable >= literal
        if (literal == min_value) {
          // nothing to restrict
          // is v >= std::numeric_limits::min(). Always true.
          return absl::OkStatus();
        }
        literal = bits_ops::Sub(literal, epsilon);
      }
      // variable > literal
      invert_base.AddInterval(Interval(min_value, literal));
    }
    invert_base.Normalize();
    IntervalSet restricted_set = IntervalSet::Complement(invert_base);
    if (restricted_set.Intervals().empty()) {
      // This implies the condition is actually unreachable impossible (since
      // we unify to bottom). For now just leave unconstrained.
      // TODO(allight): 2023-09-25: We can do better and should probably try to
      // communicate and remove the impossible cases here. This would need to be
      // done in narrowing or strength reduction by removing the associated
      // branches.
      return absl::OkStatus();
    }
    RangeData result{
        .ternary = interval_ops::ExtractTernaryVector(restricted_set),
        .interval_set = IntervalSetTree(variable->GetType(), {restricted_set}),
    };
    result_[variable] = result;
    return absl::OkStatus();
  }

  absl::Status UnifyImpreciseComparison(Node* l_op, Node* r_op,
                                        const IntervalSet& l_interval,
                                        const IntervalSet& r_interval) {
    // TODO(allight): 2023-08-10 This is much more complex and will be
    // implemented later.
    return absl::OkStatus();
  }

  absl::Status UnifyExactMatch(CompareOp* eq) {
    XLS_RET_CHECK(eq->GetType()->GetFlatBitCount() == 1);
    XLS_CHECK(eq->op() == Op::kEq || eq->op() == Op::kNe) << eq;
    // Implies that ranges must be the same (since otherwise there would be
    // some numbers where condition is false)
    Node* a = eq->operand(0);
    Node* b = eq->operand(1);
    IntervalSetTree a_intervals = base_.GetIntervalSetTree(a);
    IntervalSetTree b_intervals = base_.GetIntervalSetTree(b);

    if (result_[eq].ternary == ternary_ops::BitsToTernary(eq->op() == Op::kEq
                                                              ? UBits(1, 1)
                                                              : UBits(0, 1))) {
      // Case: (L == R) == TRUE
      // Case: (L != R) == FALSE
      IntervalSetTree unified = IntervalSetTree::Zip<IntervalSet, IntervalSet>(
          IntervalSet::Intersect, a_intervals, b_intervals);

      if (absl::c_any_of(unified.elements(), [](const IntervalSet& set) {
            return set.NumberOfIntervals() == 0;
          })) {
        // This implies the condition is actually unreachable impossible
        // (since we unify to bottom on an element). For now just leave
        // unconstrained.
        // TODO(allight): 2023-09-25: We can do better and should probably try
        // to communicate and remove the impossible cases here. This would
        // need to be done in narrowing or strength reduction by removing the
        // associated branches.
        return absl::OkStatus();
      }
      RangeData joined{
          .ternary =
              a->GetType()->IsBits()
                  ? std::make_optional(
                        interval_ops::ExtractTernaryVector(unified.Get({})))
                  : std::nullopt,
          .interval_set = unified};
      result_[a] = joined;
      result_[b] = joined;
    } else {
      // Case: (L == R) == FALSE
      // Case: (L != R) == TRUE
      // Basically only have any information if a or b is precise.
      auto is_precise = [](const IntervalSetTree& tree) -> bool {
        return absl::c_all_of(tree.elements(),
                              std::mem_fn(&IntervalSet::IsPrecise));
      };
      // TODO(allight): 2023-08-16, We should possibly do this element by
      // element instead of forcing all elements of any tuples to be precise.
      // That makes this much more complicated however.
      if (is_precise(a_intervals) || is_precise(b_intervals)) {
        Node* precise = is_precise(a_intervals) ? a : b;
        const IntervalSetTree& precise_intervals =
            precise == a ? a_intervals : b_intervals;
        Node* imprecise = precise == a ? b : a;
        bool is_bits = precise->GetType()->IsBits();

        std::optional<TernaryVector> ternary =
            is_bits ? std::make_optional(base_.GetTernary(precise).Get({}))
                    : std::nullopt;
        result_[precise] =
            RangeData{.ternary = ternary,
                      .interval_set = base_.GetIntervalSetTree(precise)};
        IntervalSetTree imprecise_complement_interval =
            base_.GetIntervalSetTree(imprecise).Map<IntervalSet>(
                &IntervalSet::Complement);
        // Remove the single known precise value from the imprecise values
        // range.
        XLS_RETURN_IF_ERROR(imprecise_complement_interval.ForEach(
            [&](Type* type, IntervalSet& imprecise,
                absl::Span<const int64_t> location) -> absl::Status {
              XLS_RET_CHECK(precise_intervals.Get(location).IsPrecise());
              imprecise.AddInterval(
                  precise_intervals.Get(location).Intervals().front());
              imprecise.Normalize();
              return absl::OkStatus();
            }));
        IntervalSetTree imprecise_interval =
            imprecise_complement_interval.Map<IntervalSet>(
                &IntervalSet::Complement);
        if (absl::c_any_of(imprecise_interval.elements(),
                           [](const IntervalSet& set) {
                             return set.NumberOfIntervals() == 0;
                           })) {
          // This implies the condition is actually unreachable
          // (since we unify to bottom on some element). For now just leave
          // unconstrained.
          // TODO(allight): 2023-09-25: We can do better and should probably try
          // to communicate and remove the impossible cases here. This would
          // need to be done in narrowing or strength reduction by removing the
          // associated branches.
          return absl::OkStatus();
        }
        result_[imprecise] = RangeData{
            .ternary =
                is_bits ? std::make_optional(interval_ops::ExtractTernaryVector(
                              imprecise_interval.Get({})))
                        : std::nullopt,
            .interval_set = imprecise_interval,
        };
      } else {
        // TODO(allight): 2023-08-10 Technically there is information to be
        // gleaned here if |L \intersect R| == 1 but probably not worth it. For
        // now just ignore this case.
        return absl::OkStatus();
      }
    }
    return absl::OkStatus();
  }

  absl::flat_hash_map<Node*, RangeData> result_;
  const RangeQueryEngine& base_;
};

// Class to hold givens extracted from select context.
//
// This also restricts the range analysis to end at the select we are
// specializing on since nodes below it can only be specialized to this select
// if we moved them into the selects branches. This sort of transform is not one
// we currently perform.
class ContextGivens final : public RangeDataProvider {
 public:
  ContextGivens(const std::vector<Node*>& topo_sort, const Node* finish,
                const absl::flat_hash_map<Node*, RangeData>& data)
      : topo_sort_(topo_sort), finish_(finish), data_(data) {}

  std::optional<RangeData> GetKnownIntervals(Node* node) final {
    if (data_.contains(node)) {
      return data_.at(node);
    }
    return std::nullopt;
  }

  absl::Status IterateFunction(DfsVisitor* visitor) final {
    for (Node* n : topo_sort_) {
      if (n == finish_) {
        break;
      }
      XLS_RETURN_IF_ERROR(n->VisitSingleNode(visitor));
    }
    return absl::OkStatus();
  }

 private:
  const std::vector<Node*>& topo_sort_;
  const Node* finish_;
  const absl::flat_hash_map<Node*, RangeData>& data_;
};

// Helper to perform the actual analysis and hold together all data needed.
// This is used to fill in the fields of the actual query engine and therefore
// does not own the arena/map that it fills in.
class Analysis {
 public:
  Analysis(
      RangeQueryEngine& base_range,
      std::vector<std::unique_ptr<const RangeQueryEngine>>& arena,
      absl::flat_hash_map<PredicateState, const RangeQueryEngine*>& engines)
      : base_range_(base_range), arena_(arena), engines_(engines) {}

  absl::StatusOr<ReachedFixpoint> Execute(FunctionBase* f) {
    // Get the topological sort once so we don't recalculate it each time.
    topo_sort_ = TopoSort(f).AsVector();
    // Get the base case.
    absl::flat_hash_map<Node*, RangeData> empty;
    ContextGivens base_givens(topo_sort_, /*finish=*/nullptr, /* data=*/empty);
    XLS_RETURN_IF_ERROR(base_range_.PopulateWithGivens(base_givens).status());

    // Get every possible one-hot state.
    // Iterate in same order we walk.
    for (Node* n : topo_sort_) {
      if (n->Is<Select>()) {
        for (int64_t idx = 0; idx < n->As<Select>()->cases().size(); ++idx) {
          all_states_.push_back(PredicateState(n->As<Select>(), idx));
        }
        if (n->As<Select>()->default_value().has_value()) {
          all_states_.push_back(
              PredicateState(n->As<Select>(), PredicateState::kDefaultArm));
        }
      }
    }
    // Bucket states into equivalence classes. Any predicate-states where the
    // arm and selector are identical
    absl::flat_hash_map<std::pair<Node*, PredicateState::ArmT>,
                        std::vector<PredicateState>>
        equivalences;
    equivalences.reserve(all_states_.size());
    for (PredicateState s : all_states_) {
      equivalences.try_emplace({s.node()->As<Select>()->selector(), s.arm()})
          .first->second.push_back(s);
    }
    // NB We don't care what order we examine each equivalence (since all are
    // disjoint).
    for (const auto& [_, states] : equivalences) {
      // Since the all_states_ is in topo the last equiv state is usable for
      // everything.
      // We don't care what order we calculate the equivalences because each is
      // fully disjoint from one another as we consider only a single condition
      // to be true at a time.
      XLS_ASSIGN_OR_RETURN(auto tmp, CalculateRangeGiven(states.back()));
      auto result =
          arena_
              .emplace_back(std::make_unique<RangeQueryEngine>(std::move(tmp)))
              .get();
      for (const PredicateState& ps : states) {
        engines_[ps] = result;
      }
    }
    return ReachedFixpoint::Changed;
  }

 private:
  absl::StatusOr<RangeQueryEngine> CalculateRangeGiven(PredicateState s) const {
    RangeQueryEngine result;
    absl::flat_hash_map<Node*, RangeData> known_data;
    XLS_ASSIGN_OR_RETURN(known_data, ExtractKnownData(s));
    ContextGivens givens(topo_sort_, s.node(), known_data);
    XLS_RETURN_IF_ERROR(result.PopulateWithGivens(givens).status());
    return result;
  }

  absl::StatusOr<absl::flat_hash_map<Node*, RangeData>> ExtractKnownData(
      PredicateState s) const {
    XLS_RET_CHECK(!s.IsBasePredicate())
        << "Can't back-propogate base predicate!";
    Node* selector = s.node()->As<Select>()->selector();
    XLS_CHECK(selector->GetType()->IsBits())
        << "Non-bits select: " << *selector;
    BackPropagate prop(base_range_);
    XLS_ASSIGN_OR_RETURN(RangeData given, ExtractSelectorValue(s, selector));
    prop.AddGiven(selector, given);
    // We could back-propagate arbitrarily but (1) writing the rules for that is
    // tricky and time consuming since we need to do a reverse-topo sort and
    // unification between different users and (2) a single propagation is
    // likely good enough for most things.  This makes sure we figure out that
    // stuff like 'x < 4 == true' implies that x \in [0, 3] and such but we
    // don't need to deal with those tricky issues.
    XLS_RETURN_IF_ERROR(selector->VisitSingleNode(&prop));
    return prop.ranges();
  }

  absl::StatusOr<RangeData> ExtractSelectorValue(PredicateState s,
                                                 Node* selector) const {
    XLS_ASSIGN_OR_RETURN(auto selector_type, selector->GetType()->AsBits());
    int64_t bit_count = selector_type->bit_count();
    IntervalSetTree interval_tree(selector->GetType());
    IntervalSet interval_set(bit_count);
    if (s.IsDefaultArm()) {
      interval_set.AddInterval(
          Interval(UBits(s.node()->As<Select>()->cases().size() + 1, bit_count),
                   Bits::AllOnes(bit_count)));
      interval_set.Normalize();
      interval_tree.Set({}, interval_set);
      return RangeData{
          .ternary = interval_ops::ExtractTernaryVector(interval_set,
                                                        /*source=*/selector),
          .interval_set = interval_tree,
      };
    }
    Bits value = UBits(s.arm_index(), bit_count);
    interval_set.AddInterval(Interval::Precise(value));
    interval_set.Normalize();
    interval_tree.Set({}, interval_set);
    return RangeData{
        .ternary = ternary_ops::BitsToTernary(value),
        .interval_set = interval_tree,
    };
  }

  std::vector<Node*> topo_sort_;
  std::vector<PredicateState> all_states_;
  RangeQueryEngine& base_range_;
  std::vector<std::unique_ptr<const RangeQueryEngine>>& arena_;
  absl::flat_hash_map<PredicateState, const RangeQueryEngine*>& engines_;
};

// A proxy query engine which specializes using select context.
class ProxyContextQueryEngine final : public QueryEngine {
 public:
  ProxyContextQueryEngine(const ContextSensitiveRangeQueryEngine& base,
                          const RangeQueryEngine& range_data)
      : base_(base), range_data_(range_data) {}

  absl::StatusOr<ReachedFixpoint> Populate(FunctionBase* f) override {
    return absl::UnimplementedError(
        "Cannot populate proxy query engine. Populate must be called on "
        "original engine only.");
  }
  bool IsTracked(Node* node) const override { return base_.IsTracked(node); }

  LeafTypeTree<TernaryVector> GetTernary(Node* node) const override {
    return MostSpecific(node).GetTernary(node);
  }

  LeafTypeTree<IntervalSet> GetIntervals(Node* node) const override {
    return MostSpecific(node).GetIntervals(node);
  }

  bool AtMostOneTrue(absl::Span<TreeBitLocation const> bits) const override {
    TernaryVector ternary = GetTernaryOf(bits);
    return std::count_if(ternary.cbegin(), ternary.cend(), [&](TernaryValue v) {
             return v == TernaryValue::kKnownOne || v == TernaryValue::kUnknown;
           }) <= 1;
  }

  bool AtLeastOneTrue(absl::Span<TreeBitLocation const> bits) const override {
    TernaryVector ternary = GetTernaryOf(bits);
    return std::count_if(ternary.cbegin(), ternary.cend(), [&](TernaryValue v) {
             return v == TernaryValue::kKnownOne;
           }) >= 1;
  }

  bool Implies(const TreeBitLocation& a,
               const TreeBitLocation& b) const override {
    return MostSpecific(a.node(), b.node()).Implies(a, b);
  }
  // We're a range-analysis so no data here.
  std::optional<Bits> ImpliedNodeValue(
      absl::Span<const std::pair<TreeBitLocation, bool>> predicate_bit_values,
      Node* node) const override {
    return std::nullopt;
  }

  bool KnownEquals(const TreeBitLocation& a,
                   const TreeBitLocation& b) const override {
    if (!IsKnown(a) || !IsKnown(b)) {
      return false;
    }
    TernaryValue av = GetTernary(a.node()).Get(a.tree_index())[a.bit_index()];
    TernaryValue bv = GetTernary(b.node()).Get(b.tree_index())[b.bit_index()];
    return av != TernaryValue::kUnknown && av == bv;
  }

  // Returns true if 'a' is the inverse of 'b'
  bool KnownNotEquals(const TreeBitLocation& a,
                      const TreeBitLocation& b) const override {
    TernaryValue av = GetTernary(a.node()).Get(a.tree_index())[a.bit_index()];
    TernaryValue bv = GetTernary(b.node()).Get(b.tree_index())[b.bit_index()];
    return av != TernaryValue::kUnknown && bv != TernaryValue::kUnknown &&
           av != bv;
  }

 private:
  TernaryVector GetTernaryOf(absl::Span<TreeBitLocation const> bits) const {
    // TODO(allight): Very inefficient but the AtMost/AtLeastOne don't seem to
    // actually be used?
    InlineBitmap known(bits.size());
    InlineBitmap values(bits.size());
    for (int64_t i = 0; i < bits.size(); ++i) {
      bool bit_known = IsKnown(bits[i]);
      if (bit_known) {
        known.Set(i, true);
        values.Set(i, IsOne(bits[i]));
      }
    }
    return ternary_ops::FromKnownBits(Bits::FromBitmap(known),
                                      Bits::FromBitmap(values));
  }
  const QueryEngine& MostSpecific(Node* nodeA, Node* nodeB) const {
    if (range_data_.HasKnownIntervals(nodeA) &&
        range_data_.HasKnownIntervals(nodeB)) {
      return range_data_;
    }
    return base_;
  }
  const QueryEngine& MostSpecific(Node* node) const {
    if (range_data_.HasKnownIntervals(node)) {
      return range_data_;
    }
    return base_;
  }
  const ContextSensitiveRangeQueryEngine& base_;
  const RangeQueryEngine& range_data_;
};

}  // namespace

absl::StatusOr<ReachedFixpoint> ContextSensitiveRangeQueryEngine::Populate(
    FunctionBase* f) {
  Analysis analysis(base_case_ranges_, arena_, one_hot_ranges_);
  return analysis.Execute(f);
}

std::unique_ptr<QueryEngine>
ContextSensitiveRangeQueryEngine::SpecializeGivenPredicate(
    const absl::flat_hash_set<PredicateState>& state) const {
  // Currently only single element states are supported.
  // We do check for consistency here but really we just ignore extra state
  // elements since something that is true for A is also true for A && B. We
  // don't have any particular strategy for picking which one gets to be the
  // 'real' state just using 'begin'.
  XLS_CHECK_LE(state.size(), 1);
  if (state.empty() || !one_hot_ranges_.contains(*state.cbegin())) {
    return QueryEngine::SpecializeGivenPredicate(state);
  }
  return std::make_unique<ProxyContextQueryEngine>(
      *this, *one_hot_ranges_.at(*state.cbegin()));
}

}  // namespace xls
