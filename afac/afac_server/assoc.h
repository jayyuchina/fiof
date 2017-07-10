/* define anything that hash table used */
#ifndef ASSOC_H
#define ASSOC_H
#include <sys/types.h>
#include <inttypes.h>
#include "afac.h"

void assoc_init(int hashpower_init);
file_cache_t *assoc_find(char *path, int length, const uint32_t hv);
int assoc_insert(file_cache_t *file, uint32_t hv);
int assoc_delete(const unsigned long fd, uint32_t hv);
extern unsigned int hashpower;
#endif
