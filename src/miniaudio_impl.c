/* Single translation unit for the miniaudio implementation.
 * Decoding/encoding backends are disabled: FFmpeg does all decoding and
 * miniaudio is used purely as a cross-platform playback device. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"
