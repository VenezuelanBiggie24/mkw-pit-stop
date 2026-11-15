// System V fibers on native PS5 stacks with a 16 KiB guard.
#include "platform.h"
#include "ps5_memory.h"

#include <stdlib.h>
#include <sys/mman.h>

static void *fiber_map_stack(size_t *stack) {
    if (!*stack || *stack > SIZE_MAX - (2 * PS5_MEM_PAGE - 1)) return MAP_FAILED;
    *stack = ((*stack + PS5_MEM_PAGE - 1) & ~(size_t)(PS5_MEM_PAGE - 1)) + PS5_MEM_PAGE;
    void *mapped = mmap(NULL, *stack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (mapped != MAP_FAILED && mprotect(mapped, PS5_MEM_PAGE, PROT_NONE)) {
        munmap(mapped, *stack);
        return MAP_FAILED;
    }
    return mapped;
}

#include "fiber_x86.h"
