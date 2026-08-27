// miniaudio's single translation unit.
//
// Isolated so the ~79k-line implementation is compiled once, with warnings
// off, and nothing else in the harness pays for it.
#define MINIAUDIO_IMPLEMENTATION

// The MA_NO_* feature macros are NOT set here. They change the layout of
// ma_device and ma_device_config, so defining them in this file only would
// give this translation unit a different view of those structs than
// loopback.cpp has -- an ODR violation that corrupts memory rather than
// failing to compile. They are set on the target in CMakeLists.txt so every
// translation unit that sees the header sees the same one.
#include "miniaudio.h"
