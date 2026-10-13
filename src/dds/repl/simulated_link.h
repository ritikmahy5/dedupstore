#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "dds/repl/connection.h"

namespace dds {

// What the simulated network does (D28).
struct LinkOptions {
  std::chrono::microseconds one_way_latency{0};
  uint64_t bytes_per_second = 0;  // 0: unlimited
  // Break the connection once this many bytes (both directions together) have been sent: the peer
  // receives exactly the bytes up to that offset, possibly half a frame, and then nothing.
  std::optional<uint64_t> disconnect_after_bytes;
};

// A connected pair of in-process Connections modelling a WAN link (D28): each direction is a
// serialization queue, so a message finishes transmitting at max(now, link_free) + size / bandwidth
// and arrives one latency later. Content is deterministic; timing is real time (receivers wait on a
// condition variable until a chunk's arrival time). Closing either end, or reaching
// disconnect_after_bytes, breaks both directions.
std::pair<std::unique_ptr<Connection>, std::unique_ptr<Connection>> MakeSimulatedLink(
    const LinkOptions& options);

}  // namespace dds
