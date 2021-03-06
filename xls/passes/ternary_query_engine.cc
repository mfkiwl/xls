// Copyright 2020 Google LLC
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

#include "xls/passes/ternary_query_engine.h"

#include <limits>

#include "absl/container/inlined_vector.h"
#include "absl/strings/str_cat.h"
#include "xls/common/status/status_macros.h"
#include "xls/data_structures/leaf_type_tree.h"
#include "xls/ir/abstract_node_evaluator.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/dfs_visitor.h"
#include "xls/ir/node_iterator.h"
#include "xls/passes/ternary_evaluator.h"

namespace xls {

Bits TernaryVectorToKnownBits(const TernaryEvaluator::Vector& ternary_vector) {
  // Use InlinedVector to avoid std::vector<bool> specialization madness.
  absl::InlinedVector<bool, 1> bits(ternary_vector.size());
  for (int64 i = 0; i < bits.size(); ++i) {
    bits[i] = (ternary_vector[i] == TernaryValue::kKnownOne ||
               ternary_vector[i] == TernaryValue::kKnownZero);
  }
  return Bits(bits);
}

Bits TernaryVectorToValueBits(const TernaryEvaluator::Vector& ternary_vector) {
  // Use InlinedVector to avoid std::vector<bool> specialization madness.
  absl::InlinedVector<bool, 1> bits(ternary_vector.size());
  for (int64 i = 0; i < bits.size(); ++i) {
    bits[i] = ternary_vector[i] == TernaryValue::kKnownOne;
  }
  return Bits(bits);
}

/* static */
absl::StatusOr<std::unique_ptr<TernaryQueryEngine>> TernaryQueryEngine::Run(
    Function* f) {
  TernaryEvaluator evaluator;
  absl::flat_hash_map<Node*, TernaryEvaluator::Vector> values;
  for (Node* node : TopoSort(f)) {
    if (!node->GetType()->IsBits()) {
      continue;
    }
    auto create_unknown_vector = [](Node* n) {
      return TernaryEvaluator::Vector(n->BitCountOrDie(),
                                      TernaryValue::kUnknown);
    };
    if (std::any_of(node->operands().begin(), node->operands().end(),
                    [](Node* o) { return !o->GetType()->IsBits(); })) {
      values[node] = create_unknown_vector(node);
      continue;
    }
    std::vector<TernaryEvaluator::Vector> operand_values;
    for (Node* operand : node->operands()) {
      operand_values.push_back(values.at(operand));
    }
    XLS_ASSIGN_OR_RETURN(
        values[node],
        AbstractEvaluate(node, operand_values, &evaluator,
                         /*default_handler=*/create_unknown_vector));
  }

  auto engine = absl::make_unique<TernaryQueryEngine>();
  for (Node* node : f->nodes()) {
    // TODO(meheff): Handle types other than bits.
    if (node->GetType()->IsBits()) {
      engine->known_bits_[node] = TernaryVectorToKnownBits(values.at(node));
      engine->bits_values_[node] = TernaryVectorToValueBits(values.at(node));
    }
  }
  return std::move(engine);
}

bool TernaryQueryEngine::AtMostOneTrue(
    absl::Span<BitLocation const> bits) const {
  int64 maybe_one_count = 0;
  for (const BitLocation& location : bits) {
    if (!IsKnown(location) || IsOne(location)) {
      maybe_one_count++;
    }
  }
  return maybe_one_count <= 1;
}

bool TernaryQueryEngine::AtLeastOneTrue(
    absl::Span<BitLocation const> bits) const {
  for (const BitLocation& location : bits) {
    if (IsOne(location)) {
      return true;
    }
  }
  return false;
}

bool TernaryQueryEngine::KnownEquals(const BitLocation& a,
                                     const BitLocation& b) const {
  return IsKnown(a) && IsKnown(b) && IsOne(a) == IsOne(b);
}

bool TernaryQueryEngine::KnownNotEquals(const BitLocation& a,
                                        const BitLocation& b) const {
  return IsKnown(a) && IsKnown(b) && IsOne(a) != IsOne(b);
}

}  // namespace xls
