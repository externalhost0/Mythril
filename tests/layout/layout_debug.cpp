// a consumer's config, not the library's, decides these macros
#undef NDEBUG
#ifndef DEBUG
#define DEBUG
#endif
#define MYTH_LAYOUT_FN DebugLayouts
#include "layout.inc"
