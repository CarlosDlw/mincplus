// A header that pulls in another header. `minc_limits.h` sits next to this
// file, and a quoted include searches the including file's own directory
// first, so no `-I` is needed here at all.

#ifndef MINC_EXAMPLE_CONFIG_H
#define MINC_EXAMPLE_CONFIG_H

#include "minc_limits.h"

// `PAGES` comes from minc_limits.h, so this file only compiles because the
// include above is real inclusion and not a forward declaration.
#define APP_NAME minc_example

#endif
