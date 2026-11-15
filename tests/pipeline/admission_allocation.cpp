#include <cstddef>
#include <cstdlib>
#include <new>

namespace admission_allocation {
thread_local bool reject = false;
}

// Test-thread fault only. Prepared COPY/commit must not allocate; background
// test orchestration remains unaffected.
void* operator new(std::size_t size) {
    if (!admission_allocation::reject) {
        if (auto* pointer = std::malloc(size == 0 ? 1 : size)) {
            return pointer;
        }
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* pointer) noexcept { std::free(pointer); }

void operator delete[](void* pointer) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    void* pointer = nullptr;
    if (!admission_allocation::reject &&
        posix_memalign(&pointer, static_cast<std::size_t>(alignment), size == 0 ? 1 : size) == 0) {
        return pointer;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    std::free(pointer);
}
