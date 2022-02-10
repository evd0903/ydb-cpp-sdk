// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// -----------------------------------------------------------------------------
// bad_any_cast.h
// -----------------------------------------------------------------------------
//
// This header file defines the `y_absl::bad_any_cast` type. 

#ifndef ABSL_TYPES_BAD_ANY_CAST_H_
#define ABSL_TYPES_BAD_ANY_CAST_H_

#include <typeinfo>

#include "y_absl/base/config.h" 

#ifdef ABSL_USES_STD_ANY

#include <any>

namespace y_absl { 
ABSL_NAMESPACE_BEGIN
using std::bad_any_cast;
ABSL_NAMESPACE_END
}  // namespace y_absl 

#else  // ABSL_USES_STD_ANY

namespace y_absl { 
ABSL_NAMESPACE_BEGIN

// -----------------------------------------------------------------------------
// bad_any_cast
// -----------------------------------------------------------------------------
//
// An `y_absl::bad_any_cast` type is an exception type that is thrown when 
// failing to successfully cast the return value of an `y_absl::any` object. 
//
// Example:
//
//   auto a = y_absl::any(65); 
//   y_absl::any_cast<int>(a);         // 65 
//   try {
//     y_absl::any_cast<char>(a); 
//   } catch(const y_absl::bad_any_cast& e) { 
//     std::cout << "Bad any cast: " << e.what() << '\n';
//   }
class bad_any_cast : public std::bad_cast {
 public:
  ~bad_any_cast() override;
  const char* what() const noexcept override;
};

namespace any_internal {

[[noreturn]] void ThrowBadAnyCast();

}  // namespace any_internal
ABSL_NAMESPACE_END
}  // namespace y_absl 

#endif  // ABSL_USES_STD_ANY

#endif  // ABSL_TYPES_BAD_ANY_CAST_H_
