#pragma once

#include <map>
#include <string>
#include <variant>
#include <vector>

#include "dds/common/crc32c.h"

namespace dds::bench {

// The part of a result file (BENCHMARKS.md section 3) that the driver knows: experiment, params,
// dataset, and one counter map per measured sample. tools/finalize_result.py adds the run id, the
// commit, the machine and build records, and the median/min/max summary.
class Report {
 public:
  using Value = std::variant<double, std::string>;

  // Every result records which CRC32C version ran (D103): it changes ingest and restore speed.
  explicit Report(std::string experiment) : experiment_(std::move(experiment)) {
    params_["crc32c"] = std::string(Crc32cImplementation());
  }
  void Param(const std::string& key, Value v) { params_[key] = std::move(v); }
  void Dataset(const std::string& key, Value v) { dataset_[key] = std::move(v); }
  // One measured run. Warm-up runs are simply not added.
  void AddSample(double wall_s, std::map<std::string, double> counters) {
    samples_.push_back({wall_s, std::move(counters)});
  }
  // Writes the JSON to `path`. False if it cannot be written.
  bool Write(const std::string& path) const;

 private:
  struct Sample {
    double wall_s;
    std::map<std::string, double> counters;
  };
  std::string experiment_;
  std::map<std::string, Value> params_;
  std::map<std::string, Value> dataset_;
  std::vector<Sample> samples_;
};

}  // namespace dds::bench
