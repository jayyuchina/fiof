#include <stdio.h>
#include <sys/types.h>
#include "afac.h"
#include "radix-tree.h"

#ifndef SLABS_H
#define SLABS_H
void slabs_init(const size_t limit, const double factor, const bool prealloc);
/*find a proper slab for every request*/
unsigned int slabs_clsid(const size_t size);
void *slabs_alloc(size_t size, unsigned int id);
void slabs_free(void *ptr, size_t size, unsigned int id);
#endif
