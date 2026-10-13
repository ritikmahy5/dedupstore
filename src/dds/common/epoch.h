#pragma once

#include <cstdint>

namespace dds {

// The process-wide epoch counter's value (ARCHITECTURE.md 6.1). It starts at 1 on open and only
// increases. Index entries record the epoch of their last reference (`last_ref`) so a concurrent
// GC can tell a blob a running session may still commit from a dead one (D20).
using Epoch = uint64_t;

// A GC run number. Runs are numbered from 1, so an entry rebuilt on open (mark 0) is never
// "marked in this run" (D50).
using GcRun = uint32_t;

}  // namespace dds
