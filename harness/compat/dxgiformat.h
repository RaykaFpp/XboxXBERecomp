#pragma once
// The harness compiles with -I<harness>/compat, so a source that still asks for <dxgiformat.h> by its
// Windows name lands here; the formats themselves live with the runtime that produces them.
#include "gpu/dxgi_format.h"
