#pragma once

#include <optional>
#include <type_traits>
#include <utility>

#include "dds/common/check.h"
#include "dds/common/status.h"

namespace dds {

// Either a value of type T or a non-OK Status (D6). Construct it implicitly from either one:
//
//   Result<Manifest> Decode(...) {
//     if (bad) return Status::Corruption("...");
//     return manifest;
//   }
//
// Accessing the value of an error Result is a bug and aborts through DDS_CHECK.
template <class T>
class [[nodiscard]] Result {
 public:
  // From anything convertible to T (a T, a derived unique_ptr, a string literal for a string).
  template <class U>
    requires(std::is_convertible_v<U &&, T> && !std::is_same_v<std::decay_t<U>, Status>)
  Result(U&& value)  // NOLINT(google-explicit-constructor): implicit by design, like the T itself
      : value_(std::forward<U>(value)) {}

  // From an error. An OK status here would leave the Result holding nothing: a bug.
  Result(Status status)  // NOLINT(google-explicit-constructor)
      : status_(std::move(status)) {
    DDS_CHECK(!status_.ok());
  }

  bool ok() const { return status_.ok(); }
  const Status& status() const { return status_; }

  // value_ holds a value exactly when status_ is OK (the constructors guarantee it); checking
  // has_value() directly lets static analysis see that the dereference below is safe.
  T& value() & {
    DDS_CHECK(value_.has_value());
    return *value_;
  }
  const T& value() const& {
    DDS_CHECK(value_.has_value());
    return *value_;
  }
  T&& value() && {
    DDS_CHECK(value_.has_value());
    return std::move(*value_);
  }

  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace dds
