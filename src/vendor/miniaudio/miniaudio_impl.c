/*
 * miniaudio single-compilation-unit implementation.
 * All other files just #include "vendor/miniaudio.h" for the declarations.
 */
#define MINIAUDIO_IMPLEMENTATION

/* We only need capture + loopback; trim the build a little. */
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION

#include "miniaudio.h"
