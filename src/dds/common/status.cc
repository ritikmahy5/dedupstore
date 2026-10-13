#include "dds/common/status.h"

#include <string>
#include <utility>

namespace dds {

const char* StatusCodeName(StatusCode code) {
  switch (code) {
    case StatusCode::kOk:
      return "OK";
    case StatusCode::kNotFound:
      return "NotFound";
    case StatusCode::kCorruption:
      return "Corruption";
    case StatusCode::kIoError:
      return "IoError";
    case StatusCode::kInvalidArgument:
      return "InvalidArgument";
    case StatusCode::kAlreadyExists:
      return "AlreadyExists";
    case StatusCode::kBusy:
      return "Busy";
    case StatusCode::kAborted:
      return "Aborted";
    case StatusCode::kIncompatible:
      return "Incompatible";
  }
  return "Unknown";
}

std::optional<StatusCode> StatusCodeFromWire(uint16_t value) {
  if (value > static_cast<uint16_t>(StatusCode::kIncompatible)) return std::nullopt;
  return static_cast<StatusCode>(value);
}

Status::Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

Status Status::NotFound(std::string message) { return {StatusCode::kNotFound, std::move(message)}; }
Status Status::Corruption(std::string message) {
  return {StatusCode::kCorruption, std::move(message)};
}
Status Status::IoError(std::string message) { return {StatusCode::kIoError, std::move(message)}; }
Status Status::InvalidArgument(std::string message) {
  return {StatusCode::kInvalidArgument, std::move(message)};
}
Status Status::AlreadyExists(std::string message) {
  return {StatusCode::kAlreadyExists, std::move(message)};
}
Status Status::Busy(std::string message) { return {StatusCode::kBusy, std::move(message)}; }
Status Status::Aborted(std::string message) { return {StatusCode::kAborted, std::move(message)}; }
Status Status::Incompatible(std::string message) {
  return {StatusCode::kIncompatible, std::move(message)};
}
Status Status::FromCode(StatusCode code, std::string message) {
  if (code == StatusCode::kOk) return {};
  return {code, std::move(message)};
}

std::string Status::ToString() const {
  if (ok()) return "OK";
  return std::string(StatusCodeName(code_)) + ": " + message_;
}

Status Status::WithContext(std::string_view context) const {
  if (ok()) return *this;
  return {code_, std::string(context) + ": " + message_};
}

}  // namespace dds
