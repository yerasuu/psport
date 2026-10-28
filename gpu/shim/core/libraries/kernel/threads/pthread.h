// psport: what the vendored Ngs2 headers (core/libraries/ngs2/ngs2_impl.h) take from shadPS4's
// pthread.h. Only the guest's mutex handle: the native Ngs2 keeps its locks in its own types, so
// nothing locks through this one.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "common/types.h"

namespace Libraries::Kernel {
using PthreadMutex = void*;
} // namespace Libraries::Kernel
