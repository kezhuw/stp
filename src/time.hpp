#pragma once

#include "types.hpp"

namespace stp {
namespace time {

// Millisecond resolution.

void sleep(uint64 msecs);

// realtime at startup.
uint64 startup_time();

namespace monotonic {

// zero-based monotonic time from startup.
uint64 now();

// monotonic realtime, same as startup_time() + now().
uint64 real_time();

}

}
}
