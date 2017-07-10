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
#include <assert.h>
#include <pthread.h>
#include "radix-tree.h"

#define page_size_t 4096
#define block_size_t 1024

typedef struct {
	unsigned int	size;			/* size of items in slab */
	unsigned int	perslab;		/* how many items per slab */
	void			*slots; 		/* list of item ptrs */
	unsigned int	slabs;			/* how many slabs were allocated for this class */
	void			**slab_list;	/* array of slab pointers */
	unsigned int	list_size;		/* how many slabs slab_list can hold*/
	unsigned int	sl_curr;		/* total free items in list, or slots size */
	size_t			requested;		/*The number of requested bytes*/
} slabclass_t;

extern item *tails[];
extern pthread_mutex_t *item_heads_tails_locks;
extern pthread_mutex_t cache_lock;

static slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES];
static size_t mem_limit = 0;
static size_t mem_malloced = 0;
static int power_largest;

static void *mem_base = NULL;
static void *mem_current = NULL;
static size_t mem_avail = 0;

/**
 * Access to the slab allocator is protected by this lock
 */
pthread_mutex_t *slabs_lock;
static pthread_mutex_t slabs_rebalance_lock = PTHREAD_MUTEX_INITIALIZER;

static int do_slabs_newslab(const unsigned int id);
static void *do_slabs_alloc(const size_t size, unsigned int id);
static int grow_slab_list (const unsigned int id);
static void *memory_allocate (size_t size);
static void split_slab_page_into_freelist(char *ptr, const unsigned int id);
static void do_slabs_free(void *ptr, const size_t size, unsigned int id);

static void slabs_preallocate (const unsigned int maxslabs) {
     int i;
     unsigned int prealloc = 0;
 
     /* pre-allocate a 1MB slab in every size class so people don't get
        confused by non-intuitive "SERVER_ERROR out of memory"
        messages.  this is the most common question on the mailing
        list.  if you really don't want this, you can rebuild without
        these three lines.  */
 
     for (i = POWER_SMALLEST; i <= POWER_LARGEST; i++) {
         if (++prealloc > maxslabs)
             return;
         if (do_slabs_newslab(i) == 0) {
             fprintf(stderr, "Error while preallocating slab memory!\n"
                 "If using -L or other prealloc options, max memory must be "
                 "at least %d megabytes.\n", power_largest);
             exit(1);
         }
     }
 
}


void slabs_init(const size_t limit, const double factor, const bool prealloc)
{
	int i = POWER_SMALLEST - 1;
	unsigned int size = global_settings.chunk_size;
	unsigned int chunk_size = sizeof(item) + size;
	unsigned int real_size;
	mem_limit = limit;

	if(prealloc){
		mem_base = malloc(mem_limit);
		if(mem_base){
			mem_current = mem_base;
			mem_avail = mem_limit;
		}else {
            fprintf(stderr, "Warning: Failed to allocate requested memory in"
                    " one large chunk.\nWill allocate in smaller chunks\n");
        }
	}
	
	memset(slabclass, 0, sizeof(slabclass));

	while(++i < POWER_LARGEST && chunk_size <= global_settings.item_size_max)
	{
			slabclass[i].size = chunk_size;
			slabclass[i].perslab = global_settings.item_size_max / slabclass[i].size;
			if(global_settings.verbose > 1) {
				fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u \n",
					i, slabclass[i].size, slabclass[i].perslab);
			}
			
			size *= factor;
			real_size = size;
			if(real_size % CHUNK_ALIGN_BYTES){
				real_size += CHUNK_ALIGN_BYTES - (real_size % CHUNK_ALIGN_BYTES);
			}
			chunk_size = sizeof(item) + real_size;
	}

	power_largest = i - 1;

	while(i <= POWER_LARGEST + 1)
	{
		slabclass[i].size = chunk_size;
		slabclass[i].perslab = 1;
		if (global_settings.verbose > 1) {
        	fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                i, slabclass[i].size, slabclass[i].perslab);
    	}
		
		size *= factor;
		real_size = size;
		if(real_size % CHUNK_ALIGN_BYTES){
				real_size += CHUNK_ALIGN_BYTES - (real_size % CHUNK_ALIGN_BYTES);
		}
		chunk_size = sizeof(item) + real_size;
		i++;		
	}
	
	
	
	if (prealloc) {
        slabs_preallocate(power_largest);
    }	
}



/*find a proper slab for every request*/
int slabs_clsid(const size_t size)
{
	int res = POWER_SMALLEST;
	if(size ==0)
		return 0;
	while (size > slabclass[res].size)
		if (res++ == POWER_LARGEST)
		{
			printf("The request is larger than all slabs\n");
			return -1; /*the I/O request is larger than slabs*/
		}
	return res;
}


void *slabs_alloc(size_t size, unsigned int id) {
    void *ret;

    pthread_mutex_lock(&slabs_lock[id]);
    ret = do_slabs_alloc(size, id);
    pthread_mutex_unlock(&slabs_lock[id]);
    return ret;
}


void slabs_free(void *ptr, size_t size, unsigned int id)
{
	pthread_mutex_lock(&slabs_lock[id]);
    do_slabs_free(ptr, size, id);
    pthread_mutex_unlock(&slabs_lock[id]);
}

/*find a slab for current item, else new slabs, otherwise LRU*/
static void *do_slabs_alloc(const size_t size, unsigned int id) {
    slabclass_t *p;
    void *ret = NULL;
    item *it = NULL;

    if (id < POWER_SMALLEST || id > POWER_LARGEST) {
		printf("do_slabs_alloc: id is smaller than POWER_SMALLEST or larger than POWER_LARGEST \n");
		return NULL;
    }

    p = &slabclass[id];
    assert(p->sl_curr == 0 || ((item *)p->slots)->slabs_clsid == 0);

    if (! (p->sl_curr != 0 || do_slabs_newslab(id) != 0)) {
        /* We don't have more memory available */
        ret = NULL;
    } else if (p->sl_curr != 0) {
        /* return off our freelist */
        it = (item *)p->slots;
        p->slots = it->next;
        if (it->next) it->next->prev = 0;
        p->sl_curr--;
        ret = (void *)it;
    }

    if (ret) {
        p->requested += size;
	}

    return ret;
}


static int replace_item(const unsigned int id)
{
	
	slabclass_t *p = &slabclass[id];
	item *search;
	file_cache_t *onwer_file;
	int fd;
	if(p->slabs == 0)
	{
		return -1;
	} else {
		search = tails[id];
		pthread_mutex_lock(&item_heads_tails_locks[id]);
		onwer_file = (file_cache_t *)search->file_owner;
		
		if(search->io_flags == 1)
		{
			fd = open(onwer_file->file_name, O_RDWR);
			write(fd, search->data + search->offset - search->block_offset, search->length);
			close(fd);
		}
		do_item_unlink(search, onwer_file);
		pthread_mutex_unlock(&item_heads_tails_locks[id]);
		return 1;
	}
}


/* allocate space for new slab*/

static int do_slabs_newslab(const unsigned int id)
{
	slabclass_t *p = &slabclass[id];
	int len = global_settings.slab_reassign ? global_settings.item_size_max
		: p->size * p->perslab;

	if(len < p->size * p->perslab)
	{
		len = p->size * p->perslab;
	}
	char *ptr;
	
	if(mem_malloced + len > mem_limit) {
		/*need to replace*/
		if(replace_item(id) == 1) {
			return 1;
		} else {
			return 0;
		}
	}
	
	if((mem_limit && mem_malloced + len > mem_limit) || (grow_slab_list(id) == 0)
		|| ((ptr = memory_allocate((size_t)len)) == 0)) {
		return 0;
	}
	
	memset (ptr, 0, (size_t)len);
	split_slab_page_into_freelist(ptr, id);
	p->slab_list[p->slabs++] = ptr;
	
	mutex_lock(&cache_lock);
	mem_malloced += len;
	mutex_unlock(&cache_lock);
	return 1;
}

static int grow_slab_list (const unsigned int id)
{
	slabclass_t *p = &slabclass[id];
	if (p->slabs == p->list_size)
		{
			size_t new_size = (p->list_size != 0) ? p->list_size * 2 : 16;
			void * new_list = realloc(p->slab_list, new_size * sizeof(void *));
			if(new_list == 0)
				{ return 0;}
			p->slab_list = new_list;
			p->list_size = new_size;
		}
	return 1;
}

static void *memory_allocate (size_t size)
{
	void *ret;
	if(mem_base == NULL)
	{
		ret = malloc(size);
	}
	else
	{
		ret = mem_current;
		if (size > mem_avail)
		{ 
			return NULL; 
		}
		if(size % CHUNK_ALIGN_BYTES) {
			size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
		}
		mem_current = ((char*)mem_current) + size;
		if(size < mem_avail) {
			mem_avail = mem_avail - size;
		}
		else {
			mem_avail = 0;
		}
	}
	return ret;
}

static void split_slab_page_into_freelist(char *ptr, const unsigned int id) {
	slabclass_t *p = &slabclass[id];
	int x;
	for (x = 0; x < p->perslab; x++) {
        	do_slabs_free(ptr, 0, id);
       		ptr += p->size; 
	}
}
static void do_slabs_free(void *ptr, const size_t size, unsigned int id) {
    slabclass_t *p;
    item *it;

    assert(((item *)ptr)->slabs_clsid == 0);
    assert(id >= POWER_SMALLEST && id <= POWER_LARGEST);
    if (id < POWER_SMALLEST || id > POWER_LARGEST)
        return;

    p = &slabclass[id];

    it = (item *)ptr;
    it->it_flags = 0;
	
    it->prev = 0;
	it->next = p->slots;
    if (it->next) it->next->prev = it;
    p->slots = it;

	it->f_prev = 0;
	it->f_next = 0;
	it->radix_next = 0;
	it->file_owner = 0;
	it->io_flags = 0;
    p->sl_curr++;
    p->requested -= size;
    return;
}

