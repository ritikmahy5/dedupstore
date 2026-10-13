#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dds {

// What went wrong. The values are explicit because they cross the wire in a replication ERROR
// message (PROTOCOL.md section 3), so they must never be renumbered. SPEC.md section 3 maps each
// code to a CLI exit code.
enum class StatusCode : uint16_t {
  kOk = 0,
  kNotFound = 1,
  kCorruption = 2,       // stored or received bytes are wrong (G4)
  kIoError = 3,          // an OS call failed
  kInvalidArgument = 4,  // the caller passed something unusable, or a limit was exceeded
  kAlreadyExists = 5,    // a target that must be new is not
  kBusy = 6,             // the repository is locked by another live process (D16)
  kAborted = 7,          // a concurrent operation made the request impossible
  kIncompatible = 8,     // format version or replication parameter mismatch
};

// "OK", "NotFound", "Corruption", ...
const char* StatusCodeName(StatusCode code);

// Converts a value received on the wire. Returns nullopt for values this version does not know.
std::optional<StatusCode> StatusCodeFromWire(uint16_t value);

// The result of a fallible operation: OK, or a code plus a message naming the file and offset
// where relevant (ARCHITECTURE.md section 9). Library code never throws (D6); every fallible
// function returns a Status or a Result<T>, and [[nodiscard]] makes ignoring one a warning.
class [[nodiscard]] Status {
 public:
  Status() = default;  // OK

  static Status Ok() { return {}; }
  static Status NotFound(std::string message);
  static Status Corruption(std::string message);
  static Status IoError(std::string message);
  static Status InvalidArgument(std::string message);
  static Status AlreadyExists(std::string message);
  static Status Busy(std::string message);
  static Status Aborted(std::string message);
  static Status Incompatible(std::string message);
  // Any code, for example one decoded from a replication ERROR message.
  static Status FromCode(StatusCode code, std::string message);

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

  // "OK" or "<CodeName>: <message>".
  std::string ToString() const;

  // The same error with "<context>: " prepended to the message. OK stays OK. Used by callers that
  // know where the failing bytes came from (for example the container file name).
  Status WithContext(std::string_view context) const;

 private:
  Status(StatusCode code, std::string message);

  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

}  // namespace dds

// DDS_TRY(expr): evaluates a Status expression once and returns it from the enclosing function
// if it is not OK (D6). For a Result<T>, write DDS_TRY(result.status()).
#define DDS_TRY(expr)                                  \
  do {                                                 \
    ::dds::Status dds_try_status_ = (expr);            \
    if (!dds_try_status_.ok()) return dds_try_status_; \
  } while (false)
