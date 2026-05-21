// ring_buffer.cpp
// Explicit template instantiations for common ring-buffer capacities so that
// the linker can find them without each TU pulling in the full template.
// The template is header-only; this file exists only as an explicit
// instantiation point to speed up builds.

#include "monitor/ring_buffer.hpp"

namespace cymon {

// Instantiate for the default session depth
template class TimestampedRingBuffer<1024>;
// Instantiate for a small depth used in tests
template class TimestampedRingBuffer<64>;

}  // namespace cymon
