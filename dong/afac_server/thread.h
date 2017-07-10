#include "afac.h"
extern struct stats_t stats;
extern void accept_new_conns(const bool do_accept);
extern pthread_mutex_t conn_lock;
file_cache_t *file_item_alloc_with_lock(unsigned long fd);
int file_item_delete_with_lock(file_cache_t *file_head);
