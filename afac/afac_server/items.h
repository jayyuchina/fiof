#ifndef ITEMS_H
#define ITEMS_H
#include <stdio.h>
#include <stdlib.h>
#include "afac.h"
item *do_item_alloc(const size_t offset, const size_t length, const int nbytes);
item *do_item_find(const unsigned long fd, const unsigned long offset, const uint32_t hv);

void item_free(item *it);
bool item_size_ok(const size_t nkey, const int flags, const int nbytes);

int  do_item_link(item * it,file_cache_t * file_head);     /** may fail if transgresses limits */
void do_item_unlink(item * it,file_cache_t * file_head);
void do_item_unlink_nolock(item *it, file_cache_t * file_head);
void do_item_remove(item *it);
void do_item_update(item *it);   /** update LRU time to current and reposition */
int  do_item_replace(item *it, item *new_it, const uint32_t hv);
item *do_item_get(const char *key, const size_t nkey, const uint32_t hv);
item *do_item_touch(const char *key, const size_t nkey, uint32_t exptime, const uint32_t hv);
extern pthread_mutex_t cache_lock;
#endif
