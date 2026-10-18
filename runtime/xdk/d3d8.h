#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

// read-only: the XDK entry points write it and mark what they wrote for the render thread (gpu::State::Mark)
const gpu::State& State();
std::recursive_mutex& Lock();

}  // namespace d3d
