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

#include "xls/dslx/type_system/type.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "xls/common/casts.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "xls/dslx/channel_direction.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/parametric_expression.h"
#include "xls/ir/bits_ops.h"

namespace xls::dslx {
namespace {

ParametricExpression::EnvValue TypeDimToEnvValue(const TypeDim& dim) {
  if (std::holds_alternative<InterpValue>(dim.value())) {
    return std::get<InterpValue>(dim.value());
  }
  return ParametricExpression::EnvValue(&dim.parametric());
}

std::vector<std::unique_ptr<Type>> CloneStructMembers(
    const std::vector<std::unique_ptr<Type>>& members) {
  std::vector<std::unique_ptr<Type>> cloned_members;
  cloned_members.reserve(members.size());
  for (auto& next : members) {
    cloned_members.push_back(next->CloneToUnique());
  }
  return cloned_members;
}

absl::flat_hash_map<std::string, TypeDim> CombineNominalTypeDims(
    const StructDefBase& struct_def_base,
    const absl::flat_hash_map<std::string, TypeDim>& existing_dims,
    const absl::flat_hash_map<std::string, TypeDim>& added_dims) {
  absl::flat_hash_map<std::string, TypeDim> combined_dims = existing_dims;
  for (const ParametricBinding* binding :
       struct_def_base.parametric_bindings()) {
    const auto it = added_dims.find(binding->identifier());
    if (it == added_dims.end()) {
      continue;
    }
    const auto existing_it = combined_dims.find(binding->identifier());
    if (existing_it != combined_dims.end()) {
      // Don't overwrite a dim that already has a concrete value. We believe
      // nothing will attempt this, now that resolution uses
      // `ResolveNominalTypeDims` to have the `StructType` request the ones it
      // wants.
      CHECK(std::holds_alternative<TypeDim::OwnedParametric>(
          existing_it->second.value()));
    }
    combined_dims.insert_or_assign(binding->identifier(), it->second.Clone());
  }
  return combined_dims;
}

absl::flat_hash_map<std::string, TypeDim> ResolveDims(
    const absl::flat_hash_map<std::string, TypeDim>& dims,
    const ParametricExpression::Env& env) {
  absl::flat_hash_map<std::string, TypeDim> new_dims = dims;
  for (auto& [key, dim] : new_dims) {
    if (std::holds_alternative<TypeDim::OwnedParametric>(dim.value())) {
      dim = TypeDim(
          std::get<TypeDim::OwnedParametric>(dim.value())->Evaluate(env));
    }
  }
  return new_dims;
}

absl::StatusOr<std::vector<std::unique_ptr<Type>>> MapMemberSizes(
    const StructTypeBase& type,
    const std::function<absl::StatusOr<TypeDim>(TypeDim)>& f) {
  std::vector<std::unique_ptr<Type>> new_members;
  for (const std::unique_ptr<Type>& member : type.members()) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> mapped, member->MapSize(f));
    new_members.push_back(std::move(mapped));
  }
  return new_members;
}

}  // namespace

Type::~Type() = default;

/* static */ bool Type::Equal(absl::Span<const std::unique_ptr<Type>> a,
                              absl::Span<const std::unique_ptr<Type>> b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (int64_t i = 0; i < a.size(); ++i) {
    if (*a[i] != *b[i]) {
      return false;
    }
  }
  return true;
}

/* static */ std::vector<std::unique_ptr<Type>> Type::CloneSpan(
    absl::Span<const std::unique_ptr<Type>> ts) {
  std::vector<std::unique_ptr<Type>> result;
  result.reserve(ts.size());
  for (const auto& t : ts) {
    CHECK(t != nullptr);
    VLOG(10) << "CloneSpan; cloning: "
             << t->ToStringInternal(FullyQualify::kNo, nullptr);
    result.push_back(t->CloneToUnique());
  }
  return result;
}

std::unique_ptr<Type> Type::MakeUnit() {
  return std::make_unique<TupleType>(std::vector<std::unique_ptr<Type>>{});
}

absl::StatusOr<std::unique_ptr<Type>> Type::FromInterpValue(
    const InterpValue& value) {
  if (value.tag() == InterpValueTag::kUBits ||
      value.tag() == InterpValueTag::kSBits) {
    XLS_ASSIGN_OR_RETURN(int64_t bit_count, value.GetBitCount());
    return std::make_unique<BitsType>(/*is_signed*/ value.IsSigned(),
                                      /*size=*/bit_count);
  }

  if (value.tag() == InterpValueTag::kArray) {
    XLS_ASSIGN_OR_RETURN(const std::vector<InterpValue>* elements,
                         value.GetValues());
    if (elements->empty()) {
      return absl::InvalidArgumentError(
          "Cannot get the Type of a 0-element array.");
    }
    XLS_ASSIGN_OR_RETURN(auto element_type, FromInterpValue(elements->at(0)));
    XLS_ASSIGN_OR_RETURN(int64_t size, value.GetLength());
    XLS_RET_CHECK_EQ(static_cast<uint32_t>(size), size);
    auto dim = TypeDim::CreateU32(static_cast<uint32_t>(size));
    return std::make_unique<ArrayType>(std::move(element_type), dim);
  }

  if (value.tag() == InterpValueTag::kTuple) {
    XLS_ASSIGN_OR_RETURN(const std::vector<InterpValue>* elements,
                         value.GetValues());
    std::vector<std::unique_ptr<Type>> members;
    members.reserve(elements->size());
    for (const auto& element : *elements) {
      XLS_ASSIGN_OR_RETURN(auto member, FromInterpValue(element));
      members.push_back(std::move(member));
    }

    return std::make_unique<TupleType>(std::move(members));
  }

  return absl::InvalidArgumentError(
      "Only bits, array, and tuple types can be converted into concrete.");
}

// -- class TypeDim

/* static */ absl::StatusOr<int64_t> TypeDim::GetAs64Bits(
    const std::variant<InterpValue, OwnedParametric>& variant) {
  if (std::holds_alternative<InterpValue>(variant)) {
    return std::get<InterpValue>(variant).GetBitValueViaSign();
  }

  return absl::InvalidArgumentError(
      "Can't evaluate a ParametricExpression to an integer.");
}

TypeDim::TypeDim(const TypeDim& other)
    : value_(std::move(other.Clone().value_)) {}

TypeDim::TypeDim(std::variant<InterpValue, OwnedParametric> value)
    : value_(std::move(value)) {
  if (std::holds_alternative<OwnedParametric>(value_)) {
    const OwnedParametric& parametric = std::get<OwnedParametric>(value_);
    CHECK(!parametric->const_value().has_value());
  }
}

TypeDim TypeDim::Clone() const {
  if (std::holds_alternative<InterpValue>(value_)) {
    return TypeDim(std::get<InterpValue>(value_));
  }
  if (std::holds_alternative<std::unique_ptr<ParametricExpression>>(value_)) {
    return TypeDim(
        std::get<std::unique_ptr<ParametricExpression>>(value_)->Clone());
  }
  LOG(FATAL) << "Impossible TypeDim value variant.";
}

std::string TypeDim::ToString() const {
  if (std::holds_alternative<std::unique_ptr<ParametricExpression>>(value_)) {
    return std::get<std::unique_ptr<ParametricExpression>>(value_)->ToString();
  }
  // Note: we don't print out the type/width of the InterpValue that serves as
  // the dimension, because printing `uN[u32:42]` would appear odd vs just
  // `uN[42]`.
  //
  // TODO(https://github.com/google/xls/issues/450) the best solution may to be
  // to have a size type that all InterpValues present on real type dimensions
  // must be. Things are trickier nowadays because we want to permit arbitrary
  // InterpValues to be passed as parametrics, not just ones that become (used)
  // dimension data -- we need to allow for e.g. signed types which may not end
  // up in any particular dimension position.
  return BitsToString(std::get<InterpValue>(value_).GetBitsOrDie());
}

ModuleType::~ModuleType() = default;

absl::Status ModuleType::Accept(TypeVisitor& v) const {
  return v.HandleModule(*this);
}

std::string ModuleType::ToStringInternal(FullyQualify fully_qualify,
                                         const FileTable*) const {
  return absl::StrFormat("typeof(module:%s)", module_.name());
}

absl::StatusOr<std::unique_ptr<Type>> ModuleType::MapSize(
    const MapFn& f) const {
  return std::make_unique<ModuleType>(module_);
}

static std::string VariantToString(
    const std::variant<InterpValue, const ParametricExpression*>& variant) {
  return absl::visit(Visitor{[](const InterpValue& value) {
                               return absl::StrFormat("InterpValue{%s}",
                                                      value.ToString());
                             },
                             [](const ParametricExpression* pe) {
                               return absl::StrFormat(
                                   "ParametricExpression{%s}", pe->ToString());
                             }},
                     variant);
}

static std::string VariantToString(
    const std::variant<InterpValue, TypeDim::OwnedParametric>& variant) {
  return absl::visit(Visitor{[](const InterpValue& value) {
                               return absl::StrFormat("InterpValue{%s}",
                                                      value.ToString());
                             },
                             [](const TypeDim::OwnedParametric& pe) {
                               return absl::StrFormat(
                                   "ParametricExpression{%s}", pe->ToString());
                             }},
                     variant);
}

std::string TypeDim::ToDebugString() const { return VariantToString(value_); }

bool TypeDim::operator==(
    const std::variant<InterpValue, const ParametricExpression*>& other) const {
  VLOG(10) << "TypeDim::operator==; this: " << VariantToString(value_)
           << " other variant: " << VariantToString(other);
  return absl::visit(
      Visitor{
          [this](const InterpValue& other_value) {
            if (std::holds_alternative<InterpValue>(value_)) {
              return std::get<InterpValue>(value_) == other_value;
            }
            return false;
          },
          [this](const ParametricExpression* other_pe) {
            return std::holds_alternative<
                       std::unique_ptr<ParametricExpression>>(value_) &&
                   *std::get<std::unique_ptr<ParametricExpression>>(value_) ==
                       *other_pe;
          },
      },
      other);
}

bool TypeDim::operator==(const TypeDim& other) const {
  // Extracts the variant from "other" and delegates to the variant-based
  // equality comparison.
  using VariantT = std::variant<InterpValue, const ParametricExpression*>;
  VariantT other_variant = absl::visit(
      Visitor{
          [](const InterpValue& v) -> VariantT { return v; },
          [](const std::unique_ptr<ParametricExpression>& p) -> VariantT {
            return p.get();
          },
      },
      other.value_);
  return *this == other_variant;
}

absl::StatusOr<TypeDim> TypeDim::Mul(const TypeDim& rhs) const {
  if (std::holds_alternative<InterpValue>(value_) &&
      std::holds_alternative<InterpValue>(rhs.value_)) {
    XLS_ASSIGN_OR_RETURN(
        InterpValue result,
        std::get<InterpValue>(value_).Mul(std::get<InterpValue>(rhs.value_)));
    return TypeDim(std::move(result));
  }
  if (IsParametric() && rhs.IsParametric()) {
    return TypeDim(std::make_unique<ParametricMul>(parametric().Clone(),
                                                   rhs.parametric().Clone()));
  }
  return absl::InvalidArgumentError(absl::StrFormat(
      "Cannot multiply dimensions: %s * %s", ToString(), rhs.ToString()));
}

absl::StatusOr<TypeDim> TypeDim::Add(const TypeDim& rhs) const {
  if (std::holds_alternative<InterpValue>(value_) &&
      std::holds_alternative<InterpValue>(rhs.value_)) {
    XLS_ASSIGN_OR_RETURN(
        InterpValue result,
        std::get<InterpValue>(value_).Add(std::get<InterpValue>(rhs.value_)));
    return TypeDim(result);
  }
  if (IsParametric() && rhs.IsParametric()) {
    return TypeDim(std::make_unique<ParametricAdd>(parametric().Clone(),
                                                   rhs.parametric().Clone()));
  }
  return absl::InvalidArgumentError(absl::StrFormat(
      "Cannot add dimensions: %s + %s", ToString(), rhs.ToString()));
}

absl::StatusOr<TypeDim> TypeDim::CeilOfLog2() const {
  if (std::holds_alternative<InterpValue>(value_)) {
    XLS_ASSIGN_OR_RETURN(InterpValue result,
                         std::get<InterpValue>(value_).CeilOfLog2());
    return TypeDim(result);
  }
  if (IsParametric()) {
    return TypeDim(std::make_unique<ParametricWidth>(parametric().Clone()));
  }
  return absl::InvalidArgumentError(absl::StrFormat(
      "Cannot find the bit width of dimension: %s", ToString()));
}

absl::StatusOr<int64_t> TypeDim::GetAsInt64() const {
  if (std::holds_alternative<InterpValue>(value_)) {
    InterpValue value = std::get<InterpValue>(value_);
    if (!value.IsBits()) {
      return absl::InvalidArgumentError(
          "Cannot convert non-bits type to int64_t.");
    }

    if (value.IsSigned()) {
      return value.GetBitValueSigned();
    }
    return value.GetBitValueUnsigned();
  }

  std::optional<InterpValue> maybe_value = parametric().const_value();
  if (maybe_value.has_value()) {
    InterpValue value = maybe_value.value();
    if (value.IsBits()) {
      if (value.IsSigned()) {
        return value.GetBitValueSigned();
      }
      return value.GetBitValueUnsigned();
    }
  }

  return absl::InvalidArgumentError(
      absl::StrCat("Expected concrete type dimension to be integral; got: ",
                   std::get<OwnedParametric>(value_)->ToString()));
}

absl::StatusOr<bool> TypeDim::GetAsBool() const {
  if (std::holds_alternative<InterpValue>(value_)) {
    InterpValue value = std::get<InterpValue>(value_);
    if (!value.IsBits()) {
      return absl::InvalidArgumentError(
          "Cannot convert non-bits type to bool.");
    }

    XLS_RET_CHECK(!value.IsSigned());
    return value.GetBitValueUnsigned();
  }

  return absl::InvalidArgumentError(
      absl::StrCat("Expected concrete type dimension to be integral; got: ",
                   std::get<OwnedParametric>(value_)->ToString()));
}

// -- TypeDimMap

void TypeDimMap::Insert(std::string_view identifier, TypeDim dim) {
  const TypeDim& stored_dim =
      dims_.insert_or_assign(identifier, std::move(dim)).first->second;
  env_.insert_or_assign(identifier, TypeDimToEnvValue(stored_dim));
}

TypeDimMap::TypeDimMap(
    const absl::flat_hash_map<std::string, InterpValue>& values) {
  for (const auto& [key, value] : values) {
    Insert(key, TypeDim(value));
  }
}

// -- Type

bool Type::CompatibleWith(const Type& other) const {
  if (*this == other) {
    return true;  // Equality implies compatibility.
  }

  // For types that encapsulate other types, they may not be strictly equal, but
  // the contained types may still be compatible.
  if (auto [t, u] = std::make_pair(dynamic_cast<const TupleType*>(this),
                                   dynamic_cast<const TupleType*>(&other));
      t != nullptr && u != nullptr) {
    return t->CompatibleWith(*u);
  }

  if (auto [t, u] = std::make_pair(dynamic_cast<const ArrayType*>(this),
                                   dynamic_cast<const ArrayType*>(&other));
      t != nullptr && u != nullptr) {
    return t->element_type().CompatibleWith(u->element_type()) &&
           t->size() == u->size();
  }

  return false;
}

bool Type::IsUnit() const {
  if (auto* t = dynamic_cast<const TupleType*>(this)) {
    return t->empty();
  }
  return false;
}

bool Type::IsStruct() const {
  return dynamic_cast<const StructType*>(this) != nullptr;
}

bool Type::IsProc() const {
  return dynamic_cast<const ProcType*>(this) != nullptr;
}

bool Type::IsEnum() const {
  return dynamic_cast<const EnumType*>(this) != nullptr;
}

bool Type::IsArray() const {
  return dynamic_cast<const ArrayType*>(this) != nullptr;
}

bool Type::IsMeta() const {
  return dynamic_cast<const MetaType*>(this) != nullptr;
}

bool Type::IsToken() const {
  return dynamic_cast<const TokenType*>(this) != nullptr;
}

bool Type::IsTuple() const {
  return dynamic_cast<const TupleType*>(this) != nullptr;
}

bool Type::IsFunction() const {
  return dynamic_cast<const FunctionType*>(this) != nullptr;
}

bool Type::IsModule() const {
  return dynamic_cast<const ModuleType*>(this) != nullptr;
}

const EnumType& Type::AsEnum() const {
  auto* s = dynamic_cast<const EnumType*>(this);
  CHECK(s != nullptr) << "Type is not an enum: " << *this;
  return *s;
}

const StructType& Type::AsStruct() const {
  auto* s = dynamic_cast<const StructType*>(this);
  CHECK(s != nullptr) << "Type is not a struct: " << *this;
  return *s;
}

const ProcType& Type::AsProc() const {
  auto* s = dynamic_cast<const ProcType*>(this);
  CHECK(s != nullptr) << "Type is not a proc: " << *this;
  return *s;
}

const MetaType& Type::AsMeta() const {
  auto* s = dynamic_cast<const MetaType*>(this);
  CHECK(s != nullptr) << "Type is not a MetaType: " << *this;
  return *s;
}

const ArrayType& Type::AsArray() const {
  auto* s = dynamic_cast<const ArrayType*>(this);
  CHECK(s != nullptr) << "Type is not an array: " << *this;
  return *s;
}

const TupleType& Type::AsTuple() const {
  auto* s = dynamic_cast<const TupleType*>(this);
  CHECK(s != nullptr) << "Type is not a tuple: " << *this;
  return *s;
}

// -- TokenType

TokenType::~TokenType() = default;

// -- MetaType

MetaType::~MetaType() = default;

// -- BitsConstructorType

BitsConstructorType::BitsConstructorType(TypeDim is_signed)
    : is_signed_(std::move(is_signed)) {
  VLOG(10) << "BitsConstructorType constructor; is_signed: "
           << is_signed_.ToDebugString();

  // Validate that if we hold a parametric it is not just wrapping a concrete
  // value -- that should always be represented with just the concrete value
  // directly.
  if (!is_signed_.IsParametric()) {
    // Check that the InterpValue is a boolean.
    const InterpValue& value = std::get<InterpValue>(is_signed_.value());
    CHECK(value.IsBool())
        << "BitsConstructorType is_signed must be a boolean; got: "
        << value.ToString();
  }
}

BitsConstructorType::~BitsConstructorType() = default;

absl::Status BitsConstructorType::Accept(TypeVisitor& v) const {
  return v.HandleBitsConstructor(*this);
}

absl::StatusOr<std::unique_ptr<Type>> BitsConstructorType::MapSize(
    const MapFn& f) const {
  XLS_ASSIGN_OR_RETURN(TypeDim new_is_signed, f(is_signed_));
  if (!new_is_signed.IsParametric()) {
    // Check that the concrete value that we got for signedness is a boolean.
    const InterpValue& value = std::get<InterpValue>(new_is_signed.value());
    XLS_RET_CHECK(value.IsBool())
        << "Bits constructor `is_signed` value must be a boolean; got: "
        << value.ToString();
  }
  return std::make_unique<BitsConstructorType>(std::move(new_is_signed));
}

bool BitsConstructorType::operator==(const Type& other) const {
  VLOG(10) << "BitsConstructorType::operator==; this: " << *this
           << " other: " << other;
  if (auto* t = dynamic_cast<const BitsConstructorType*>(&other)) {
    bool result = t->is_signed_ == is_signed_;
    VLOG(10) << "BitsConstructorType::operator==; result: " << result;
    return result;
  }
  if (auto* b = dynamic_cast<const BitsType*>(&other)) {
    return TypeDim::CreateBool(b->is_signed()) == is_signed_ &&
           b->size() == TypeDim::CreateU32(0);
  }
  return false;
}

std::string BitsConstructorType::ToStringInternal(FullyQualify fully_qualify,
                                                  const FileTable*) const {
  return absl::StrFormat("xN[is_signed=%s]", is_signed_.ToString());
}

std::string BitsConstructorType::GetDebugTypeName() const {
  return "bits-constructor";
}

bool BitsConstructorType::HasEnum() const { return false; }
bool BitsConstructorType::HasToken() const { return false; }

std::vector<TypeDim> BitsConstructorType::GetAllDims() const {
  std::vector<TypeDim> result;
  result.push_back(is_signed_.Clone());
  return result;
}

absl::StatusOr<TypeDim> BitsConstructorType::GetTotalBitCount() const {
  return TypeDim::CreateU32(0);
}

std::unique_ptr<Type> BitsConstructorType::CloneToUnique() const {
  return std::make_unique<BitsConstructorType>(is_signed_.Clone());
}

// -- BitsType

BitsType::BitsType(bool is_signed, int64_t size)
    : BitsType(is_signed,
               TypeDim(InterpValue::MakeU32(static_cast<uint32_t>(size)))) {
  CHECK_EQ(size, static_cast<uint32_t>(size));
}

BitsType::BitsType(bool is_signed, TypeDim size)
    : is_signed_(is_signed), size_(std::move(size)) {}

bool BitsType::operator==(const Type& other) const {
  VLOG(10) << "BitsType::operator==; this: " << *this << " other: " << other;
  if (auto* t = dynamic_cast<const BitsType*>(&other)) {
    return t->is_signed_ == is_signed_ && t->size_ == size_;
  }
  if (IsArrayOfBitsConstructor(other)) {
    const auto* a = down_cast<const ArrayType*>(&other);
    const auto* bc = down_cast<const BitsConstructorType*>(&a->element_type());
    return a->size() == size_ &&
           bc->is_signed() == TypeDim::CreateBool(is_signed());
  }
  return false;
}

absl::StatusOr<std::unique_ptr<Type>> BitsType::MapSize(
    const std::function<absl::StatusOr<TypeDim>(TypeDim)>& f) const {
  XLS_ASSIGN_OR_RETURN(TypeDim new_size, f(size_));
  return std::make_unique<BitsType>(is_signed_, new_size);
}

std::string BitsType::ToStringInternal(FullyQualify fully_qualify,
                                       const FileTable*) const {
  return absl::StrFormat("%cN[%s]", is_signed_ ? 's' : 'u', size_.ToString());
}

std::string BitsType::GetDebugTypeName() const {
  return is_signed_ ? "sbits" : "ubits";
}

std::unique_ptr<BitsType> BitsType::ToUBits() const {
  return std::make_unique<BitsType>(false, size_.Clone());
}

// -- StructTypeBase

StructTypeBase::StructTypeBase(
    std::vector<std::unique_ptr<Type>> members, const StructDefBase& struct_def,
    absl::flat_hash_map<std::string, TypeDim> nominal_type_dims_by_identifier)
    : members_(std::move(members)),
      struct_def_base_(struct_def),
      nominal_type_dims_by_identifier_(
          std::move(nominal_type_dims_by_identifier)) {
  CHECK_EQ(members_.size(), struct_def_base_.members().size());
  for (const std::unique_ptr<Type>& member_type : members_) {
    CHECK(!member_type->IsMeta()) << *member_type;
  }
}

bool StructTypeBase::HasEnum() const {
  return absl::c_any_of(members_,
                        [](const auto& type) { return type->HasEnum(); });
}

bool StructTypeBase::HasToken() const {
  return absl::c_any_of(members_,
                        [](const auto& type) { return type->HasToken(); });
}

std::string StructTypeBase::ToErrorString() const {
  return absl::StrFormat("struct '%s' structure: %s",
                         struct_def_base_.identifier(),
                         ToStringInternal(FullyQualify::kNo, nullptr));
}

std::string StructTypeBase::ToStringInternal(
    FullyQualify fully_qualify, const FileTable* file_table) const {
  std::string guts;
  for (int64_t i = 0; i < members().size(); ++i) {
    if (i != 0) {
      absl::StrAppend(&guts, ", ");
    }
    absl::StrAppendFormat(
        &guts, "%s: %s", GetMemberName(i),
        GetMemberType(i).ToStringInternal(fully_qualify, file_table));
  }
  if (!guts.empty()) {
    guts = absl::StrCat(" ", guts, " ");
  }
  std::string struct_name = struct_def_base_.identifier();
  if (fully_qualify == FullyQualify::kYes) {
    CHECK(file_table != nullptr);
    struct_name = absl::StrCat(struct_def_base_.span().GetFilename(*file_table),
                               ":", struct_name);
  }
  return absl::StrCat(struct_name, " {", guts, "}");
}

absl::StatusOr<std::vector<std::string>> StructTypeBase::GetMemberNames()
    const {
  std::vector<std::string> results;
  results.reserve(members().size());
  for (int64_t i = 0; i < members().size(); ++i) {
    results.push_back(std::string(GetMemberName(i)));
  }
  return results;
}

absl::StatusOr<int64_t> StructTypeBase::GetMemberIndex(
    std::string_view name) const {
  XLS_ASSIGN_OR_RETURN(std::vector<std::string> names, GetMemberNames());
  auto it = std::find(names.begin(), names.end(), name);
  if (it == names.end()) {
    return absl::NotFoundError(
        absl::StrFormat("Name not present in tuple type %s: %s",
                        ToStringInternal(FullyQualify::kNo, nullptr), name));
  }
  return std::distance(names.begin(), it);
}

std::optional<const Type*> StructTypeBase::GetMemberTypeByName(
    std::string_view target) const {
  for (int64_t i = 0; i < members().size(); ++i) {
    if (GetMemberName(i) == target) {
      return &GetMemberType(i);
    }
  }
  return std::nullopt;
}

std::vector<TypeDim> StructTypeBase::GetAllDims() const {
  std::vector<TypeDim> results;
  for (const std::unique_ptr<Type>& type : members_) {
    std::vector<TypeDim> t_dims = type->GetAllDims();
    for (auto& dim : t_dims) {
      results.push_back(std::move(dim));
    }
  }
  return results;
}

absl::StatusOr<TypeDim> StructTypeBase::GetTotalBitCount() const {
  auto sum = TypeDim::CreateU32(0);
  for (const std::unique_ptr<Type>& t : members_) {
    XLS_ASSIGN_OR_RETURN(TypeDim elem_bit_count, t->GetTotalBitCount());
    XLS_ASSIGN_OR_RETURN(sum, sum.Add(elem_bit_count));
  }

  return sum;
}

bool StructTypeBase::HasNamedMember(std::string_view target) const {
  for (int64_t i = 0; i < members().size(); ++i) {
    if (GetMemberName(i) == target) {
      return true;
    }
  }
  return false;
}

bool StructTypeBase::operator==(const Type& other) const {
  if (auto* t = dynamic_cast<const StructType*>(&other)) {
    return Equal(members_, t->members_) &&
           &struct_def_base_ == &t->struct_def_base_;
  }
  return false;
}

// -- StructType

absl::StatusOr<std::unique_ptr<Type>> StructType::MapSize(
    const std::function<absl::StatusOr<TypeDim>(TypeDim)>& f) const {
  XLS_ASSIGN_OR_RETURN(std::vector<std::unique_ptr<Type>> new_members,
                       MapMemberSizes(*this, f));
  return std::make_unique<StructType>(std::move(new_members), nominal_type(),
                                      nominal_type_dims_by_identifier());
}

std::unique_ptr<Type> StructType::ResolveNominalTypeDims(
    const ParametricExpression::Env& env) const {
  absl::flat_hash_map<std::string, TypeDim> new_dims =
      ResolveDims(nominal_type_dims_by_identifier(), env);
  return std::make_unique<StructType>(CloneStructMembers(members()),
                                      nominal_type(), std::move(new_dims));
}

std::unique_ptr<Type> StructType::AddNominalTypeDims(
    const absl::flat_hash_map<std::string, TypeDim>& added_dims) const {
  return std::make_unique<StructType>(
      CloneStructMembers(members()), nominal_type(),
      CombineNominalTypeDims(struct_def_base(),
                             nominal_type_dims_by_identifier(), added_dims));
}

// -- ProcType

absl::StatusOr<std::unique_ptr<Type>> ProcType::MapSize(
    const std::function<absl::StatusOr<TypeDim>(TypeDim)>& f) const {
  XLS_ASSIGN_OR_RETURN(std::vector<std::unique_ptr<Type>> new_members,
                       MapMemberSizes(*this, f));
  return std::make_unique<ProcType>(std::move(new_members), nominal_type(),
                                    nominal_type_dims_by_identifier());
}

std::unique_ptr<Type> ProcType::ResolveNominalTypeDims(
    const ParametricExpression::Env& env) const {
  absl::flat_hash_map<std::string, TypeDim> new_dims =
      ResolveDims(nominal_type_dims_by_identifier(), env);
  return std::make_unique<ProcType>(CloneStructMembers(members()),
                                    nominal_type(), std::move(new_dims));
}

std::unique_ptr<Type> ProcType::AddNominalTypeDims(
    const absl::flat_hash_map<std::string, TypeDim>& added_dims) const {
  return std::make_unique<ProcType>(
      CloneStructMembers(members()), nominal_type(),
      CombineNominalTypeDims(struct_def_base(),
                             nominal_type_dims_by_identifier(), added_dims));
}

// -- TupleType

TupleType::TupleType(std::vector<std::unique_ptr<Type>> members)
    : members_(std::move(members)) {
#ifndef NDEBUG
  for (const auto& member : members_) {
    DCHECK(member != nullptr);
  }
#endif
}

bool TupleType::operator==(const Type& other) const {
  if (auto* t = dynamic_cast<const TupleType*>(&other)) {
    return Equal(members_, t->members_);
  }
  return false;
}

bool TupleType::HasEnum() const {
  return absl::c_any_of(members_,
                        [](const auto& type) { return type->HasEnum(); });
}

bool TupleType::HasToken() const {
  return absl::c_any_of(members_,
                        [](const auto& type) { return type->HasToken(); });
}

bool TupleType::empty() const { return members_.empty(); }

int64_t TupleType::size() const { return members_.size(); }

bool TupleType::CompatibleWith(const TupleType& other) const {
  if (members_.size() != other.members_.size()) {
    return false;
  }
  for (int64_t i = 0; i < members_.size(); ++i) {
    if (!members_[i]->CompatibleWith(*other.members_[i])) {
      return false;
    }
  }
  // Same member count and all compatible members.
  return true;
}

absl::StatusOr<std::unique_ptr<Type>> TupleType::MapSize(
    const std::function<absl::StatusOr<TypeDim>(TypeDim)>& f) const {
  std::vector<std::unique_ptr<Type>> new_members;
  for (const auto& member_type : members_) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> mapped, member_type->MapSize(f));
    new_members.push_back(std::move(mapped));
  }
  return std::make_unique<TupleType>(std::move(new_members));
}

std::unique_ptr<Type> TupleType::CloneToUnique() const {
  return std::make_unique<TupleType>(CloneSpan(members_));
}

std::string TupleType::ToStringInternal(FullyQualify fully_qualify,
                                        const FileTable* file_table) const {
  std::string guts = absl::StrJoin(
      members_, ", ",
      [file_table](std::string* out, const std::unique_ptr<Type>& m) {
        absl::StrAppend(out,
                        m->ToStringInternal(FullyQualify::kNo, file_table));
      });
  return absl::StrCat("(", guts, ")");
}

std::string TupleType::ToInlayHintString() const {
  std::string guts = absl::StrJoin(
      members_, ", ", [](std::string* out, const std::unique_ptr<Type>& m) {
        absl::StrAppend(out, m->ToInlayHintString());
      });
  return absl::StrCat("(", guts, ")");
}

std::vector<TypeDim> TupleType::GetAllDims() const {
  std::vector<TypeDim> results;
  for (const std::unique_ptr<Type>& t : members_) {
    std::vector<TypeDim> t_dims = t->GetAllDims();
    for (auto& dim : t_dims) {
      results.push_back(std::move(dim));
    }
  }
  return results;
}

absl::StatusOr<TypeDim> TupleType::GetTotalBitCount() const {
  auto sum = TypeDim::CreateU32(0);
  for (const std::unique_ptr<Type>& t : members_) {
    XLS_ASSIGN_OR_RETURN(TypeDim elem_bit_count, t->GetTotalBitCount());
    XLS_ASSIGN_OR_RETURN(sum, sum.Add(elem_bit_count));
  }

  return sum;
}

// -- ArrayType

ArrayType::ArrayType(std::unique_ptr<Type> element_type, const TypeDim& size)
    : element_type_(std::move(element_type)), size_(size) {
  CHECK(!element_type_->IsMeta())
      << "Array element cannot be a metatype because arrays cannot hold types; "
         "got: "
      << element_type_->ToStringInternal(FullyQualify::kNo, nullptr);
}

absl::StatusOr<std::unique_ptr<Type>> ArrayType::MapSize(
    const std::function<absl::StatusOr<TypeDim>(TypeDim)>& f) const {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> new_element_type,
                       element_type_->MapSize(f));
  XLS_ASSIGN_OR_RETURN(TypeDim new_size, f(size_));
  return std::make_unique<ArrayType>(std::move(new_element_type),
                                     std::move(new_size));
}

std::string ArrayType::ToStringInternal(FullyQualify fully_qualify,
                                        const FileTable* file_table) const {
  return absl::StrFormat(
      "%s[%s]", element_type_->ToStringInternal(fully_qualify, file_table),
      size_.ToString());
}

std::string ArrayType::ToInlayHintString() const {
  return absl::StrFormat("%s[%s]", element_type_->ToInlayHintString(),
                         size_.ToString());
}

bool ArrayType::operator==(const Type& other) const {
  VLOG(10) << "ArrayType::operator==; this: " << *this << " other: " << other;
  if (auto* o = dynamic_cast<const ArrayType*>(&other)) {
    return size_ == o->size_ && *element_type_ == *o->element_type_;
  }
  if (IsBitsConstructor(element_type())) {
    if (auto* b = dynamic_cast<const BitsType*>(&other)) {
      const auto* bc =
          dynamic_cast<const BitsConstructorType*>(&element_type());
      VLOG(10) << "size: " << size() << " b->size: " << b->size()
               << " bc->is_signed(): " << bc->is_signed()
               << " b->is_signed(): " << b->is_signed();
      return size() == b->size() &&
             bc->is_signed() == TypeDim::CreateBool(b->is_signed());
    }
  }
  return false;
}

std::vector<TypeDim> ArrayType::GetAllDims() const {
  std::vector<TypeDim> results;
  results.push_back(size_.Clone());
  std::vector<TypeDim> element_dims = element_type_->GetAllDims();
  for (auto& dim : element_dims) {
    results.push_back(std::move(dim));
  }
  return results;
}

absl::StatusOr<TypeDim> ArrayType::GetTotalBitCount() const {
  // For the bits constructor (xN) although the element type is "sizeless"
  // (i.e. like `bits`, `xN[false]` doesn't have a size on its own, it needs to
  // be placed in an array), when it is instantiated via an array type it has
  // the given size; i.e. the size of `xN[false][N]` is `N`.
  if (IsBitsConstructor(element_type())) {
    return size_;
  }

  XLS_ASSIGN_OR_RETURN(TypeDim elem_bits, element_type_->GetTotalBitCount());
  return elem_bits.Mul(size_);
}

ArrayType::InnerMostElementType ArrayType::GetInnermostElementType() const {
  const ArrayType* array_type = this;
  bool all_dims_known = true;
  while (true) {
    all_dims_known = all_dims_known && array_type->size().GetAsInt64().ok();
    const ArrayType* nested_array_type =
        dynamic_cast<const ArrayType*>(&array_type->element_type());
    if (nested_array_type == nullptr) {
      break;
    }
    array_type = nested_array_type;
  }
  return {std::cref(array_type->element_type()), std::cref(*array_type),
          all_dims_known};
}

int ArrayType::ArrayDimensions() const {
  const Type* element_type = element_type_.get();
  int size = 1;
  while (const ArrayType* child_type =
             dynamic_cast<const ArrayType*>(element_type)) {
    size++;
    element_type = &child_type->element_type();
  }
  return size;
}

// -- EnumType

absl::StatusOr<std::unique_ptr<Type>> EnumType::MapSize(
    const std::function<absl::StatusOr<TypeDim>(TypeDim)>& f) const {
  XLS_ASSIGN_OR_RETURN(TypeDim new_size, f(size_));
  return std::make_unique<EnumType>(enum_def_, std::move(new_size), is_signed_,
                                    members_);
}

std::string EnumType::ToStringInternal(FullyQualify fully_qualify,
                                       const FileTable* file_table) const {
  if (fully_qualify == FullyQualify::kYes) {
    return absl::StrCat(enum_def_.span().GetFilename(*file_table), ":",
                        enum_def_.identifier());
  }
  return enum_def_.identifier();
}

std::vector<TypeDim> EnumType::GetAllDims() const {
  std::vector<TypeDim> result;
  result.push_back(size_.Clone());
  return result;
}

// -- FunctionType

absl::StatusOr<std::unique_ptr<Type>> FunctionType::MapSize(
    const std::function<absl::StatusOr<TypeDim>(TypeDim)>& f) const {
  std::vector<std::unique_ptr<Type>> new_params;
  for (const auto& param : params_) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> new_param, param->MapSize(f));
    new_params.push_back(std::move(new_param));
  }
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> new_return_type,
                       return_type_->MapSize(f));
  return std::make_unique<FunctionType>(std::move(new_params),
                                        std::move(new_return_type));
}

bool FunctionType::operator==(const Type& other) const {
  if (auto* o = dynamic_cast<const FunctionType*>(&other)) {
    if (params_.size() != o->params_.size()) {
      return false;
    }
    for (int64_t i = 0; i < params_.size(); ++i) {
      if (*params_[i] != *o->params_[i]) {
        return false;
      }
    }
    return *return_type_ == *o->return_type_;
  }
  return false;
}

std::vector<const Type*> FunctionType::GetParams() const {
  std::vector<const Type*> results;
  results.reserve(params_.size());
  for (const auto& param : params_) {
    results.push_back(param.get());
  }
  return results;
}

std::string FunctionType::ToStringInternal(FullyQualify fully_qualify,
                                           const FileTable* file_table) const {
  std::string params_str = absl::StrJoin(
      params_, ", ",
      [fully_qualify, file_table](std::string* out,
                                  const std::unique_ptr<Type>& t) {
        absl::StrAppend(out, t->ToStringInternal(fully_qualify, file_table));
      });
  return absl::StrFormat(
      "(%s) -> %s", params_str,
      return_type_->ToStringInternal(fully_qualify, file_table));
}

std::vector<TypeDim> FunctionType::GetAllDims() const {
  std::vector<TypeDim> results;
  for (const auto& param : params_) {
    std::vector<TypeDim> param_dims = param->GetAllDims();
    for (auto& dim : param_dims) {
      results.push_back(std::move(dim));
    }
  }
  return results;
}

absl::StatusOr<TypeDim> FunctionType::GetTotalBitCount() const {
  auto sum = TypeDim::CreateU32(0);
  for (const auto& param : params_) {
    XLS_ASSIGN_OR_RETURN(TypeDim param_bits, param->GetTotalBitCount());
    XLS_ASSIGN_OR_RETURN(sum, sum.Add(param_bits));
  }
  return sum;
}

ChannelType::ChannelType(std::unique_ptr<Type> payload_type,
                         ChannelDirection direction)
    : payload_type_(std::move(payload_type)), direction_(direction) {
  CHECK(payload_type_ != nullptr);
  CHECK(!payload_type_->IsMeta());
}

std::string ChannelType::ToStringInternal(FullyQualify fully_qualify,
                                          const FileTable* file_table) const {
  return absl::StrFormat(
      "chan(%s, dir=%s)",
      payload_type_->ToStringInternal(fully_qualify, file_table),
      direction_ == ChannelDirection::kIn ? "in" : "out");
}

std::vector<TypeDim> ChannelType::GetAllDims() const {
  return payload_type_->GetAllDims();
}

absl::StatusOr<TypeDim> ChannelType::GetTotalBitCount() const {
  return payload_type_->GetTotalBitCount();
}

absl::StatusOr<std::unique_ptr<Type>> ChannelType::MapSize(
    const MapFn& f) const {
  XLS_ASSIGN_OR_RETURN(auto new_payload, payload_type_->MapSize(f));
  return std::make_unique<ChannelType>(std::move(new_payload), direction_);
}

bool ChannelType::operator==(const Type& other) const {
  if (auto* o = dynamic_cast<const ChannelType*>(&other)) {
    return *payload_type_ == *o->payload_type_ && direction_ == o->direction_;
  }
  return false;
}

bool ChannelType::HasEnum() const { return payload_type_->HasEnum(); }
bool ChannelType::HasToken() const { return payload_type_->HasToken(); }

std::unique_ptr<Type> ChannelType::CloneToUnique() const {
  return std::make_unique<ChannelType>(payload_type_->CloneToUnique(),
                                       direction_);
}

absl::StatusOr<bool> IsSigned(const Type& c) {
  if (auto* bits = dynamic_cast<const BitsType*>(&c)) {
    return bits->is_signed();
  }
  if (auto* enum_type = dynamic_cast<const EnumType*>(&c)) {
    std::optional<bool> signedness = enum_type->is_signed();
    if (!signedness.has_value()) {
      return absl::InvalidArgumentError(
          "Signedness not present for EnumType: " +
          c.ToStringInternal(FullyQualify::kNo, nullptr));
    }
    return signedness.value();
  }
  const BitsConstructorType* bc;
  if (IsArrayOfBitsConstructor(c, &bc)) {
    const TypeDim& is_signed = bc->is_signed();
    if (is_signed.IsParametric()) {
      return absl::InvalidArgumentError(
          "Cannot determine signedness; type has parametric signedness: " +
          c.ToStringInternal(FullyQualify::kNo, nullptr));
    }
    XLS_ASSIGN_OR_RETURN(int64_t value, is_signed.GetAsInt64());
    return value != 0;
  }
  return absl::InvalidArgumentError(
      "Cannot determined signedness; type is neither enum nor bits: " +
      c.ToStringInternal(FullyQualify::kNo, nullptr));
}

const ParametricSymbol* TryGetParametricSymbol(const TypeDim& dim) {
  if (!std::holds_alternative<TypeDim::OwnedParametric>(dim.value())) {
    return nullptr;
  }
  const ParametricExpression* parametric =
      std::get<TypeDim::OwnedParametric>(dim.value()).get();
  return dynamic_cast<const ParametricSymbol*>(parametric);
}

absl::StatusOr<TypeDim> MetaType::GetTotalBitCount() const {
  return absl::InvalidArgumentError(
      "Cannot get total bit count of a meta-type, as these are not "
      "realizable as values; meta-type: " +
      ToString());
}

bool IsBitsLike(const Type& t) {
  return dynamic_cast<const BitsType*>(&t) != nullptr ||
         IsArrayOfBitsConstructor(t);
}

std::string ToTypeString(const BitsLikeProperties& properties) {
  if (properties.is_signed.IsParametric()) {
    return absl::StrFormat("xN[%s][%s]", properties.is_signed.ToString(),
                           properties.size.ToString());
  }
  bool is_signed = properties.is_signed.GetAsBool().value();
  return absl::StrFormat("%sN[%s]", is_signed ? "s" : "u",
                         properties.size.ToString());
}

std::optional<BitsLikeProperties> GetBitsLike(const Type& t) {
  if (auto* bits_type = dynamic_cast<const BitsType*>(&t);
      bits_type != nullptr) {
    return BitsLikeProperties{
        .is_signed = TypeDim::CreateBool(bits_type->is_signed()),
        .size = bits_type->size()};
  }
  const BitsConstructorType* bc;
  if (IsArrayOfBitsConstructor(t, &bc)) {
    auto* array = dynamic_cast<const ArrayType*>(&t);
    return BitsLikeProperties{.is_signed = bc->is_signed(),
                              .size = array->size()};
  }
  return std::nullopt;
}

static std::optional<bool> GetKnownSignedness(
    const BitsLikeProperties& properties) {
  const TypeDim& is_signed = properties.is_signed;
  if (is_signed.IsParametric()) {
    return std::nullopt;
  }
  CHECK(std::get<InterpValue>(is_signed.value()).IsBool());
  return is_signed.GetAsBool().value();
}

static std::optional<int64_t> GetKnownBitCount(
    const BitsLikeProperties& properties) {
  const TypeDim& size = properties.size;
  if (size.IsParametric()) {
    return std::nullopt;
  }
  return size.GetAsInt64().value();
}

bool IsKnownU1(const BitsLikeProperties& properties) {
  std::optional<bool> signedness = GetKnownSignedness(properties);
  std::optional<int64_t> bit_count = GetKnownBitCount(properties);
  return signedness == false && bit_count == 1;
}

bool IsKnownU32(const BitsLikeProperties& properties) {
  std::optional<bool> signedness = GetKnownSignedness(properties);
  std::optional<int64_t> bit_count = GetKnownBitCount(properties);
  return signedness == false && bit_count == 32;
}

}  // namespace xls::dslx
