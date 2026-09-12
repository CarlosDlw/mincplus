// A header included from another header and from the example itself. It is
// included three times in total, and the guard is what makes that harmless.
//
// The guard is in the canonical form the multiple-include optimization
// recognizes: leading comments, then `#ifndef X` / `#define X` as the first two
// meaningful directives, everything else inside, and a single `#endif` at the
// end. The optimization is only applied to that exact shape, because a
// near-miss heuristic would silently change what the file means.

#ifndef MINC_EXAMPLE_LIMITS_H
#define MINC_EXAMPLE_LIMITS_H

// Object-like macros with no parentheses on purpose: `MAX_ITEMS` is a
// constant, and `(MAX_ITEMS)` would be a different (safer, noisier) choice.
#define MAX_ITEMS 100
#define PER_PAGE 25

// A macro that uses two others. It is expanded where it is *used*, not where it
// is defined, so a later `#undef`/`#define` of either name changes this macro's
// value -- which is exactly the property that makes macros fragile, and why the
// preprocessor records where each one was defined.
#define PAGES ((MAX_ITEMS + PER_PAGE - 1) / PER_PAGE)

#endif
