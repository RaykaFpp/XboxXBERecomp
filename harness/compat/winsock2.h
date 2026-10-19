#pragma once
// The harness compiles with -I<harness>/compat, so a source that still asks for <winsock2.h> by its Windows
// name lands here; the POSIX socket mapping itself lives with the runtime that uses it.
#include "xdk/winsock_compat.h"
