#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dds::cli {

// Builds one compact JSON document for `--json` output (SPEC.md section 3). Commas are inserted
// automatically; the caller only has to nest Begin/End calls correctly and put a Key before each
// value inside an object (a mistake is a bug and trips DDS_CHECK).
//
//   JsonWriter w;
//   w.BeginObject(); w.Key("chunks"); w.Uint(12); w.EndObject();
//   std::string text = w.Take();   // {"chunks":12}
// Whether `s` is well-formed UTF-8 (RFC 3629 section 4): no overlong forms, no UTF-16
// surrogates, nothing above U+10FFFF.
bool IsValidUtf8(std::string_view s);

class JsonWriter {
 public:
  void BeginObject();
  void EndObject();
  void BeginArray();
  void EndArray();
  void Key(std::string_view key);

  // Escapes '"', '\', and control characters, and writes each byte that is not part of a valid
  // UTF-8 sequence as U+FFFD, because JSON text must be UTF-8 (RFC 8259 section 8.1) while file
  // names may be any bytes (D41). Callers that need such a name exactly also write it in hex.
  void String(std::string_view value);
  void Int(int64_t value);
  void Uint(uint64_t value);
  // Up to 6 significant digits; infinities and NaN, which JSON cannot express, become null.
  void Double(double value);
  void Bool(bool value);
  void Null();

  // The finished document. All containers must be closed.
  std::string Take();

 private:
  // Writes the separator a new value needs at the current position.
  void BeforeValue();
  void AppendEscaped(std::string_view s);

  struct Level {
    bool is_object;
    bool empty = true;
    bool expecting_value = false;  // objects only: a Key was written, its value is next
  };
  std::string out_;
  std::vector<Level> stack_;
};

}  // namespace dds::cli
