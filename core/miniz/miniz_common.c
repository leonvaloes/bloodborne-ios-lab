#include "miniz_common.h"

void *miniz_def_alloc_func(void *opaque, size_t items, size_t size)
{
    (void)opaque;
    return malloc(items * size);
}

void miniz_def_free_func(void *opaque, void *address)
{
    (void)opaque;
    free(address);
}

void *miniz_def_realloc_func(void *opaque, void *address, size_t items, size_t size)
{
    (void)opaque;
    return realloc(address, items * size);
}
