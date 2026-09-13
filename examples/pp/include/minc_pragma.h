// A header with no `#ifndef` guard. It is protected by the *operator* form of
// `#pragma once`, so the guard is one line instead of three.
//
// `_Pragma("once")` is C99 and means exactly what `#pragma once` means: the
// preprocessor destringizes the operand, lexes it, and hands it to the same
// handler the directive form uses, so the two cannot drift apart. That is also
// why this file is worth having in the corpus -- it is the case that proves the
// operator is not a second implementation.
//
// It is an operator rather than a directive, so it produces no token at all.
// `examples/pp/008_header_names.mx` includes this file twice; the second one is
// elided, which is only true if the mark was really made.

_Pragma("once")

#define PRAGMA_ONCE_SEEN 1
