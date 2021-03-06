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
//
// Bindings class (name to defining AST node tracking) for use in parsing.

#ifndef XLS_DSLX_CPP_BINDINGS_H_
#define XLS_DSLX_CPP_BINDINGS_H_

#include "xls/dslx/cpp_ast.h"

namespace xls::dslx {

// Bindings (name references in the environment that map back to definition
// points in the AST) resolve to this BoundNode variant, which are all kinds of
// definitions.
using BoundNode = absl::variant<Enum*, TypeDef*, ConstantDef*, NameDef*,
                                BuiltinNameDef*, Struct*, Import*>;

// Checks that n is one of the BoundNode variants shown above, and returns it in
// the BoundNode variant. Returns an error status if the AST node is not one of
// the supported variant types.
absl::StatusOr<BoundNode> ToBoundNode(AstNode* n);

// Returns a string, useful for reporting in error messages, for the type of the
// AST node contained inside of the given BoundNode variant; e.g. "Import".
std::string BoundNodeGetTypeString(const BoundNode& bn);

// Encodes ParseError data in a canonical way inside of an invalid argument
// error.
//
// When these propagate up to a Python boundary we throw them as exceptions
// using the data encoded in the absl::Status message. See ParseErrorGetData()
// for the utility used to extract the data from the error message text.
inline absl::Status ParseError(const Span& span, absl::string_view message) {
  return absl::InvalidArgumentError(
      absl::StrFormat("ParseError: %s %s", span.ToString(), message));
}

// Returns (span, message), or an error status if "status" is not a valid
// ParseError.
absl::StatusOr<std::pair<Span, std::string>> ParseErrorGetData(
    const absl::Status& status);
absl::StatusOr<Span> ParseErrorGetSpan(const absl::Status& status);
absl::StatusOr<std::string> ParseErrorGetText(const absl::Status& status);

// Maps identifiers to the AST node that bound that identifier (also known as
// the lexical environment).
//
// The datatype is "stackable" so that we can easily take the bindings at a
// given point in the program (say in a function) and extend it with a new scope
// by stacking a fresh Bindings object on top (also sometimes referred to as a
// "scope chain"). For example:
//
//    Binding builtin_bindings;
//    builtin_bindings.Add("range", m.Make<BuiltinNameDef>("range"));
//
//    // Create a fresh scope, with no need to copy the builtin_bindings object.
//    Bindings function_bindings(&builtin_bindings);
//    XLS_ASSIGN_OR_RETURN(Function* f, ParseFunction(&function_bindings));
//
// We can do this because bindings are immutable and stack according to lexical
// scope; new bindings in the worst case only shadow previous bindings.
class Bindings {
 public:
  Bindings(Bindings* parent = nullptr) : parent_(parent) {}

  // Returns whether there are any local bindings (i.e. bindings that are not
  // set in parent/ancestors).
  bool HasLocalBindings() const { return !local_bindings_.empty(); }

  // Adds a local binding.
  void Add(std::string name, BoundNode binding) {
    local_bindings_[std::move(name)] = binding;
  }

  // Returns the AST node bound to 'name'.
  absl::optional<BoundNode> ResolveNode(absl::string_view name) const {
    auto it = local_bindings_.find(name);
    if (it == local_bindings_.end()) {
      if (parent_ != nullptr) {
        return parent_->ResolveNode(name);
      }
      return absl::nullopt;
    }

    return it->second;
  }

  bool ResolveNodeIsTypeDefinition(absl::string_view name) const {
    absl::optional<BoundNode> bn = ResolveNode(name);
    if (!bn) {
      return false;
    }
    return absl::holds_alternative<Enum*>(*bn) ||
           absl::holds_alternative<TypeDef*>(*bn) ||
           absl::holds_alternative<Struct*>(*bn);
  }

  // As above, but flags a ParseError() if the binding cannot be resolved,
  // attributing the source of the binding resolution as span.
  absl::StatusOr<BoundNode> ResolveNodeOrError(absl::string_view name,
                                               const Span& span) const {
    absl::optional<BoundNode> result = ResolveNode(name);
    if (result.has_value()) {
      return *std::move(result);
    }
    return ParseError(
        span, absl::StrFormat("Cannot find a definition for name: '%s'", name));
  }

  // Resolves "name" as an AST binding and returns the associated NameDefNode.
  //
  // Returns nullopt if no AST node binding is found associated with "name".
  absl::optional<AnyNameDef> ResolveNameOrNullopt(absl::string_view name) const;

  // As above, but returns a ParseError status.
  absl::StatusOr<AnyNameDef> ResolveNameOrError(absl::string_view name,
                                                const Span& span) const;

  // Returns whether there is an AST binding associated with "name".
  bool HasName(absl::string_view name) const {
    return ResolveNode(name).has_value();
  }

 private:
  Bindings* parent_;
  absl::flat_hash_map<std::string, BoundNode> local_bindings_;
};

// Returns the name definition node (either builtin or user-defined) associated
// with the given binding data.
AnyNameDef BoundNodeToAnyNameDef(BoundNode bn);

// Returns the text span where the binding data is defined.
//
// For a builtin name definition, a "fake" span is returned (that spans no
// characters in the filename "<builtin>").
Span BoundNodeGetSpan(BoundNode bn);

}  // namespace xls::dslx

#endif  // XLS_DSLX_CPP_BINDINGS_H_
