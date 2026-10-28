#pragma once
// The SDK's runtime/cpu/dispatch.cpp includes this on every host that is not Windows. It uses none of it there (the
// game's timer runs on std::chrono), and clang's own header refuses to compile off x86, so it is reached only there.
#if defined(__x86_64__) || defined(__i386__)
#include_next <x86intrin.h>
#endif
