#pragma once
// included before every host source by CMake: the SDK's kernel/memory.cpp calls ftruncate, and its guest memory needs a
// file it can map twice
#include <unistd.h>

// macOS has no memfd: an unlinked temporary file gives the same double mapping of guest RAM. The kernel frees it when
// the process ends, as it frees a memfd
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
inline int memfd_create(const char* name, unsigned int) {
    const char* tmp = std::getenv("TMPDIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/xbe-ram-XXXXXX", tmp ? tmp : "/tmp");
    int fd = mkstemp(path);
    if (fd >= 0) unlink(path);
    (void)name;
    return fd;
}

// macOS's pthread_setname_np names the calling thread and takes no thread argument, where the other POSIXes take one
#include <pthread.h>
#define pthread_setname_np(thread, name) (::pthread_setname_np(name))

// The guest commits and decommits memory in its own 4 KB pages (kernel/memory.cpp), while the host's page is 16 KB on
// Apple Silicon and 4 KB on Intel: mprotect and madvise want an address on a page boundary, so the calls are rounded
// out to the host's page. Rounding out is what the host does anyway; on Intel it is a no-op
#include <sys/mman.h>
#include <unistd.h>
inline int xbe_page_aligned_mprotect(void* addr, size_t len, int prot) {
    size_t page = size_t(sysconf(_SC_PAGESIZE));
    uintptr_t first = reinterpret_cast<uintptr_t>(addr) & ~(page - 1);
    uintptr_t end = (reinterpret_cast<uintptr_t>(addr) + len + page - 1) & ~(page - 1);
    return ::mprotect(reinterpret_cast<void*>(first), end - first, prot);
}
inline int xbe_page_aligned_madvise(void* addr, size_t len, int advice) {
    size_t page = size_t(sysconf(_SC_PAGESIZE));
    uintptr_t first = reinterpret_cast<uintptr_t>(addr) & ~(page - 1);
    uintptr_t end = (reinterpret_cast<uintptr_t>(addr) + len + page - 1) & ~(page - 1);
    return ::madvise(reinterpret_cast<void*>(first), end - first, advice);
}
#define mprotect(addr, len, prot) xbe_page_aligned_mprotect((addr), (len), (prot))
#define madvise(addr, len, advice) xbe_page_aligned_madvise((addr), (len), (advice))
