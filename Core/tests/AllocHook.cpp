// Test-only: replaces the global allocation functions so every heap allocation is counted
// (see AllocCounter.h). Compiled straight into ftcore_tests, never into ftcore or Unreal;
// it forwards straight to malloc/free.
#include "AllocCounter.h"

#include <cstddef>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace {

struct Installer {
    Installer() { ftcore_test::detail::markHookInstalled(); }
} g_installer;

void* plainAlloc(std::size_t size) noexcept {
    void* p = std::malloc(size ? size : 1);
    if (p) ftcore_test::detail::recordAlloc(size);
    return p;
}

void* alignedAlloc(std::size_t size, std::size_t align) noexcept {
    if (size == 0) size = 1;
    void* p = nullptr;
#ifdef _WIN32
    p = _aligned_malloc(size, align);
#else
    if (align < sizeof(void*)) align = sizeof(void*);
    if (posix_memalign(&p, align, size) != 0) p = nullptr;
#endif
    if (p) ftcore_test::detail::recordAlloc(size);
    return p;
}

void plainFree(void* p) noexcept { std::free(p); }

void alignedFree(void* p) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void* orThrow(void* p, std::size_t size, std::size_t align, bool isAligned) {
    while (!p) {
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
        p = isAligned ? alignedAlloc(size, align) : plainAlloc(size);
    }
    return p;
}

} // namespace

void* operator new(std::size_t n) { return orThrow(plainAlloc(n), n, 0, false); }
void* operator new[](std::size_t n) { return orThrow(plainAlloc(n), n, 0, false); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return plainAlloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return plainAlloc(n); }

void* operator new(std::size_t n, std::align_val_t a) {
    return orThrow(alignedAlloc(n, static_cast<std::size_t>(a)), n, static_cast<std::size_t>(a), true);
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return orThrow(alignedAlloc(n, static_cast<std::size_t>(a)), n, static_cast<std::size_t>(a), true);
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return alignedAlloc(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return alignedAlloc(n, static_cast<std::size_t>(a));
}

void operator delete(void* p) noexcept { plainFree(p); }
void operator delete[](void* p) noexcept { plainFree(p); }
void operator delete(void* p, std::size_t) noexcept { plainFree(p); }
void operator delete[](void* p, std::size_t) noexcept { plainFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { plainFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { plainFree(p); }

void operator delete(void* p, std::align_val_t) noexcept { alignedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { alignedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { alignedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { alignedFree(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { alignedFree(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { alignedFree(p); }
