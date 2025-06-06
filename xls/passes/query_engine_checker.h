// Copyright 2025 The XLS Authors
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

#ifndef XLS_PASSES_QUERY_ENGINE_CHECKER_H_
#define XLS_PASSES_QUERY_ENGINE_CHECKER_H_

#include "absl/status/status.h"
#include "xls/ir/package.h"
#include "xls/passes/optimization_pass.h"
#include "xls/passes/pass_base.h"

namespace xls {

// Invariant checker which verifies that all optimization query engines have
// internally consistent state.
class QueryEngineChecker : public OptimizationInvariantChecker {
 public:
  absl::Status Run(Package* p, const OptimizationPassOptions& options,
                   PassResults* results,
                   OptimizationContext& context) const override;
};

}  // namespace xls

#endif  // XLS_PASSES_QUERY_ENGINE_CHECKER_H_
