#include "dds/gc/planner.h"

#include <algorithm>
#include <unordered_map>

#include "dds/repo/repository.h"

namespace dds {

std::vector<PlannedContainer> MakePlan(RepoInternals& repo,
                                       const std::vector<Candidate>& candidates, GcRun run,
                                       Epoch e_min, double threshold) {
  std::unordered_map<uint32_t, uint64_t> live;
  for (const Candidate& c : candidates) live[c.id] = 0;
  for (size_t shard = 0; shard < ChunkIndex::kShardCount; ++shard) {
    repo.index.ForEachInShard(shard, [&](const IndexEntry& e) {
      auto it = live.find(e.container_id);
      if (it == live.end()) return;
      // The same rule RemoveIfDead applies (6.1), so the plan and the decisions agree.
      if (e.mark == run || IsProtected(e, e_min)) it->second += e.length;
    });
  }
  std::vector<PlannedContainer> plan;
  plan.reserve(candidates.size());
  for (const Candidate& c : candidates) {
    PlannedContainer p{c, live[c.id], PlanAction::kKeep};
    if (p.live_bytes == 0) {
      p.action = PlanAction::kDelete;
    } else if (static_cast<double>(p.live_bytes) <
               threshold * static_cast<double>(c.payload_bytes)) {
      p.action = PlanAction::kCompact;
    }
    plan.push_back(p);
  }
  std::sort(plan.begin(), plan.end(), [](const PlannedContainer& a, const PlannedContainer& b) {
    return a.candidate.id < b.candidate.id;
  });
  return plan;
}

}  // namespace dds
