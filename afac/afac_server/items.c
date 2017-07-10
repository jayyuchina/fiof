#include "afac.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include "items.h"
#include "cache.h"
#include "assoc.h"

#define LARGEST_ID POWER_LARGEST

#ifndef RADIX_GRA
#define RADIX_GRA 4096
#endif

extern struct file_mapping *mapping;
extern pthread_mutex_t *item_heads_tails_locks;

#define BLOCK_SIZE 1024
/*for one class of slabs, a pair of head and tail pointers list all of the items that allocated---dwr*/
item *heads[LARGEST_ID + 1];
item *tails[LARGEST_ID + 1];

/*for one item, it will be three lists, one in radix-tree,
one in head-tail list of current slabs, one in owner file's list*/

static void item_link_q(item * it, file_cache_t *file_head);
static void item_unlink_q(item * it, file_cache_t *file_head);

/* See items.c */
uint64_t get_cas_id(void);

/*@null@*/
item *do_item_alloc(const size_t offset, const size_t length, const int nbytes)
{
	size_t ntotal = sizeof(struct data_item) + nbytes;
	unsigned int id = slabs_clsid(ntotal);
	if(id == 0)
		return NULL;
	if(id == -1) /*need to modified*/
		return NULL;
	item *it;
	it = slabs_alloc(ntotal, id);
	if(it == NULL) {
		return it;
	}
	assert(it->slabs_clsid == 0);
	assert(it != heads[id]);
	
	it->refcount = 0;
	it->next = it->prev = it->f_next = it->f_prev = it->radix_next = 0;
	it->slabs_clsid = id;
	it->nbytes = nbytes;
	it->offset = offset;
	it->block_offset = offset/RADIX_GRA;
	it->block_offset *= RADIX_GRA;
	it->length = length;
	it->data = (char*)it+ sizeof(struct data_item);
	it->it_flags = 0;
	
	return it;	
}

void item_free(item *it)
{
	size_t ntotal = sizeof(struct data_item) + it->nbytes;
	unsigned int clsid;
	/*assert(it->it_flags & ITEM_LINKED == 0);*/
	printf("it->it_flags & ITEM_LINKED == %d\n", it->it_flags & ITEM_LINKED);
	assert(it != heads[it->slabs_clsid]);
	assert(it != tails[it->slabs_clsid]);

	clsid = it->slabs_clsid;
	it->slabs_clsid = 0;
	slabs_free(it, ntotal, clsid);
}


int  do_item_link(item * it,file_cache_t * file_head)
{
	assert((it->it_flags & (ITEM_LINKED | ITEM_SLABBED)) == 0);
	it->file_owner = (void *)file_head;
	it->it_flags = ITEM_LINKED;
	item_link_q(it, file_head);
	/*refcount_incr(&it->refcount);*/
	it->refcount = 0;
	
	return 1;
}
void do_item_unlink(item * it,file_cache_t * file_head)
{	
	if(it->it_flags & ITEM_LINKED != 0)
	{
		it->it_flags &= ~ITEM_LINKED;
		item_unlink_q(it, file_head);
		it->file_owner = NULL;
		do_item_remove(it);
	}
}
void do_item_unlink_nolock(item *it, file_cache_t * file_head)
{
	if(it->it_flags & ITEM_LINKED != 0)
	{
		it->it_flags &= ~ITEM_LINKED;
		item_unlink_q(it, file_head);
		do_item_remove(it);
	}
}
void do_item_remove(item *it)
{
	assert((it->it_flags & ITEM_SLABBED)== 0);
	assert(it->io_flags == 0);
	item_free(it);
}

item *do_item_find(const unsigned long fd, const unsigned long offset, const uint32_t hv)
{
	file_cache_t * file_head = assoc_find(mapping[fd].file_name, mapping[fd].path_length, hv);
	
	item *it;
	unsigned long radix_offset;
	
	if(file_head != NULL) {
		radix_offset = offset/RADIX_GRA;
		it = (item *)radix_tree_lookup(&(file_head->root), radix_offset);
	}else {it = NULL;}
	return it;
}

static void item_link_q(item * it, file_cache_t *file_head)
{
	item **head, **tail;
	pthread_mutex_lock(&item_heads_tails_locks[it->slabs_clsid]);
	
	head = &heads[it->slabs_clsid];
	tail = &tails[it->slabs_clsid];
	assert(it != *head);
	assert((*head && *tail)||(*head ==0 && *tail ==0));
	it->prev = NULL;
	it->next = *head;
	if(it->next) it->next->prev = it;
	*head = it;
	if (*tail ==0) *tail = it;

	pthread_mutex_unlock(&item_heads_tails_locks[it->slabs_clsid]);


	head = &(file_head->f_header);
	tail = &(file_head->f_tail);
	assert(it != *head);
	assert((*head && *tail)||(*head ==0 && *tail ==0));
	it->f_prev = NULL;
	it->f_next = *head;
	if(it->f_next) it->f_next->f_prev = it;
	*head = it;
	if (*tail == 0) *tail = it;
	
	return;
}

static void item_unlink_q(item * it, file_cache_t *file_head)
{
	item **head, **tail;
	
	pthread_mutex_lock(&item_heads_tails_locks[it->slabs_clsid]);
	
	assert(it->slabs_clsid < LARGEST_ID);
	head = &heads[it->slabs_clsid];
	tail = &tails[it->slabs_clsid];

	if(*head == it) {
		assert(it->prev == NULL);
		*head = it->next;
	}
	if(*tail == it) {
		assert(it->next == 0);
		*tail = it->prev;
	}
	assert(it->next != it);
	assert(it->prev != it);

	if(it->next) it->next->prev = it->prev;
	if(it->prev) it->prev->next = it->next;

	pthread_mutex_unlock(&item_heads_tails_locks[it->slabs_clsid]);

	head = &file_head->f_header;
	tail = &file_head->f_tail;

	if(*head == it) {
		assert(it->f_prev == 0);
		*head = it->f_next;
	}
	if(*tail == it) {
		assert(it->f_next == 0);
		*tail = it->f_prev;
	}

	assert(it->f_prev != it);
	assert(it->f_next != it);

	if(it->f_next) it->f_next->f_prev = it->f_prev;
	if(it->f_prev) it->f_prev->f_next = it->f_next;
	return;
}

