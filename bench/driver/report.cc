#include "driver/report.h"

#include <cmath>
#include <fstream>

#include "dds/cli/json.h"

namespace dds::bench {

namespace {

// Counts must stay exact (ground-truth rule: no rounded numbers): integral values below 2^53 are
// written as integers; anything else keeps JsonWriter's 6 significant digits, which for timings in
// seconds is microsecond resolution or better on runs under 10 s.
void WriteNumber(cli::JsonWriter& w, double v) {
  if (std::floor(v) == v && std::fabs(v) < 9007199254740992.0) {
    w.Int(static_cast<int64_t>(v));
  } else {
    w.Double(v);
  }
}

void WriteMap(cli::JsonWriter& w, const std::map<std::string, Report::Value>& m) {
  w.BeginObject();
  for (const auto& [key, value] : m) {
    w.Key(key);
    if (const double* d = std::get_if<double>(&value)) {
      WriteNumber(w, *d);
    } else {
      w.String(std::get<std::string>(value));
    }
  }
  w.EndObject();
}

}  // namespace

bool Report::Write(const std::string& path) const {
  cli::JsonWriter w;
  w.BeginObject();
  w.Key("experiment");
  w.String(experiment_);
  w.Key("params");
  WriteMap(w, params_);
  w.Key("dataset");
  WriteMap(w, dataset_);
  w.Key("samples");
  w.BeginArray();
  for (const Sample& s : samples_) {
    w.BeginObject();
    w.Key("wall_s");
    WriteNumber(w, s.wall_s);
    w.Key("counters");
    w.BeginObject();
    for (const auto& [key, value] : s.counters) {
      w.Key(key);
      WriteNumber(w, value);
    }
    w.EndObject();
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  std::ofstream out(path, std::ios::trunc);
  out << w.Take() << "\n";
  return static_cast<bool>(out);
}

}  // namespace dds::bench
