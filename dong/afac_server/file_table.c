#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_table.h"


#define FILE_NUM_MAX 1024*1024
file_table_t file_item_table;

void file_table_init()
{
	file_cache_t *ptr;
	int file_num = FILE_NUM_MAX;
	ptr = calloc(file_num, sizeof(file_cache_t));
	if(ptr == NULL)
		exit(1);
	else{
		int x = 0;
		memset(ptr,0,sizeof(file_cache_t)*file_num);
		file_cache_t *it;
		file_table_t *p;
		p = &file_item_table;
		p->alloc_list_head = 0;
		p->avail_list_head = 0;
		p->alloc_num = 0;
		p->avail_num = 0;
		printf("file_item_table is ok\n");
		printf("size of file_cache_t is %d\n",sizeof(file_cache_t));
		for(x = 0; x < file_num; x++)
		{
				it = (file_cache_t *)ptr;
				it->fd = 0;
				it->f_header = 0;
				it->f_tail = 0;
				it->h_next = 0;
				pthread_mutex_init(&(it->file_object_lock), NULL);
				INIT_RADIX_TREE(&(it->root));
				
				it->prev = 0;
				it->next = p->avail_list_head;
				p->avail_list_head = it;
				p->avail_num++;
				/*printf("is there any problem\n");
				printf("ptr is %d\t x is %d\n",ptr,x); */
				/*because of the address add unit is its calloc's unit file_cache_t*/
				ptr = ptr + 1;
		}
	}
	return;			
}
file_cache_t *file_item_alloc(unsigned long fd)
{
	file_table_t *p;
	file_cache_t *it = NULL;
	p = &file_item_table;
	if(p->avail_num == 0)
	{
		return NULL;
	}else {
	p->avail_num--;
	p->alloc_num++;
	it = p->avail_list_head;
	p->avail_list_head = it->next;
	
	if(p->alloc_num == 1)
	{
		p->alloc_list_tail = it;
	}

	it->prev = 0;
	it->next = p->alloc_list_head;
	p->alloc_list_head = it;
	it->fd = fd;
	it->exp_time = FILE_DELAY_TIME;
	}
	return it;
}
int file_item_delete(file_cache_t *file_head)
{
	file_table_t *p;
	file_cache_t *it;
	p = &file_item_table;
	if(file_head == NULL)
		return 0;
	else {
		p->alloc_num--;
		p->avail_num++;
		if(file_head->prev != NULL)
			file_head->prev->next = file_head->next;
		if(file_head->next != NULL) {
			file_head->next->prev = file_head->prev;
		} else {
			p->alloc_list_tail = file_head->prev;
		}

		file_head->prev = NULL;
		file_head->next = p->avail_list_head;
		p->avail_list_head = file_head;
	}
	return 0;
}
