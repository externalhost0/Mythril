// a consumer's config, not the library's, decides these macros
#undef DEBUG
#ifndef NDEBUG
#define NDEBUG
#endif
#define MYTH_LAYOUT_FN ReleaseLayouts
#include "layout.inc"
