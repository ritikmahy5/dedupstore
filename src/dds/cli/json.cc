#include "dds/cli/json.h"

#include <cmath>
#include <cstdio>

#include "dds/common/check.h"

namespace dds::cli {

namespace {

bool IsContinuation(unsigned char b) { return (b & 0xC0) == 0x80; }

// The length of the well-formed UTF-8 sequence starting at s[i] (whose byte is >= 0x80), or 0 if
// there is none. The ranges are RFC 3629 section 4's table: the narrowed second-byte ranges after
// E0, ED, F0, and F4 exclude overlong forms, UTF-16 surrogates, and code points above U+10FFFF.
size_t Utf8SequenceLength(std::string_view s, size_t i) {
  const auto at = [&](size_t k) { return static_cast<unsigned char>(s[i + k]); };
  const unsigned char b0 = at(0);
  size_t n = 0;
  unsigned char lo = 0x80, hi = 0xBF;  // allowed range of the second byte
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    n = 2;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    n = 3;
    if (b0 == 0xE0) lo = 0xA0;
    if (b0 == 0xED) hi = 0x9F;
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    n = 4;
    if (b0 == 0xF0) lo = 0x90;
    if (b0 == 0xF4) hi = 0x8F;
  } else {
    return 0;  // 80-C1 (continuation bytes, overlong 2-byte leads) and F5-FF are never leads
  }
  if (s.size() - i < n || at(1) < lo || at(1) > hi) return 0;
  for (size_t k = 2; k < n; ++k) {
    if (!IsContinuation(at(k))) return 0;
  }
  return n;
}

}  // namespace

bool IsValidUtf8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    if (static_cast<unsigned char>(s[i]) < 0x80) {
      ++i;
      continue;
    }
    const size_t n = Utf8SequenceLength(s, i);
    if (n == 0) return false;
    i += n;
  }
  return true;
}

void JsonWriter::BeforeValue() {
  if (stack_.empty()) {
    DDS_CHECK(out_.empty());  // a document has one top-level value
    return;
  }
  Level& top = stack_.back();
  if (top.is_object) {
    DDS_CHECK(top.expecting_value);  // object members need a Key first
    top.expecting_value = false;
  } else {
    if (!top.empty) out_ += ',';
    top.empty = false;
  }
}

void JsonWriter::BeginObject() {
  BeforeValue();
  out_ += '{';
  stack_.push_back({.is_object = true});
}

void JsonWriter::EndObject() {
  DDS_CHECK(!stack_.empty() && stack_.back().is_object && !stack_.back().expecting_value);
  stack_.pop_back();
  out_ += '}';
}

void JsonWriter::BeginArray() {
  BeforeValue();
  out_ += '[';
  stack_.push_back({.is_object = false});
}

void JsonWriter::EndArray() {
  DDS_CHECK(!stack_.empty() && !stack_.back().is_object);
  stack_.pop_back();
  out_ += ']';
}

void JsonWriter::Key(std::string_view key) {
  DDS_CHECK(!stack_.empty() && stack_.back().is_object && !stack_.back().expecting_value);
  Level& top = stack_.back();
  if (!top.empty) out_ += ',';
  top.empty = false;
  out_ += '"';
  AppendEscaped(key);
  out_ += "\":";
  top.expecting_value = true;
}

void JsonWriter::String(std::string_view value) {
  BeforeValue();
  out_ += '"';
  AppendEscaped(value);
  out_ += '"';
}

void JsonWriter::Int(int64_t value) {
  BeforeValue();
  out_ += std::to_string(value);
}

void JsonWriter::Uint(uint64_t value) {
  BeforeValue();
  out_ += std::to_string(value);
}

void JsonWriter::Double(double value) {
  BeforeValue();
  if (!std::isfinite(value)) {
    out_ += "null";
    return;
  }
  char buf[32];
  // The C locale is never changed in dds, so '.' is the decimal separator.
  std::snprintf(buf, sizeof(buf), "%.6g", value);
  out_ += buf;
}

void JsonWriter::Bool(bool value) {
  BeforeValue();
  out_ += value ? "true" : "false";
}

void JsonWriter::Null() {
  BeforeValue();
  out_ += "null";
}

std::string JsonWriter::Take() {
  DDS_CHECK(stack_.empty());
  return std::move(out_);
}

void JsonWriter::AppendEscaped(std::string_view s) {
  constexpr char kHex[] = "0123456789abcdef";
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (static_cast<unsigned char>(c) >= 0x80) {
      const size_t n = Utf8SequenceLength(s, i);
      if (n == 0) {
        out_ += "\xef\xbf\xbd";  // U+FFFD REPLACEMENT CHARACTER, for one invalid byte
        ++i;
      } else {
        out_.append(s.substr(i, n));
        i += n;
      }
      continue;
    }
    switch (c) {
      case '"':
        out_ += "\\\"";
        break;
      case '\\':
        out_ += "\\\\";
        break;
      case '\n':
        out_ += "\\n";
        break;
      case '\r':
        out_ += "\\r";
        break;
      case '\t':
        out_ += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out_ += "\\u00";
          out_ += kHex[(c >> 4) & 0xF];
          out_ += kHex[c & 0xF];
        } else {
          out_ += c;
        }
    }
    ++i;
  }
}

}  // namespace dds::cli
