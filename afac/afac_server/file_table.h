#ifndef FILE_TABLE_H
#define FILE_TABLE_H

#include "afac.h"

typedef struct file_table{
file_cache_t *avail_list_head;
file_cache_t *alloc_list_head;
file_cache_t *alloc_list_tail;
int alloc_num;
int avail_num;
}file_table_t;

void file_table_init();
file_cache_t *file_item_alloc(unsigned long fd);
int file_item_delete(file_cache_t *file_head);
#endif
