#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <string.h>
#include <sysexits.h>
#include <assert.h>
#include <event2/event.h>
#include <linux/limits.h>
#include "afac.h"
#include "hash.h"
#include "assoc.h"
#include "cache.h"
#include "items.h"
#include "file_table.h"
#include "protocol_binary.h"
#include "thread.h"
#ifndef HAVE_OFF64_T
typedef int64_t off64_t;
#endif

#ifndef __need_IOV_MAX
#define __need_IOV_MAX
#endif

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif


#define hashsize(n) ((unsigned long int)1<<(n))
#define hashmask(n) (hashsize(n)-1)



/*
 * forward declarations
 */
static void drive_machine(conn *c);
static int new_socket(struct addrinfo *ai);
static int try_read_command(conn *c);
static int server_socket(const char *interface, int port, enum network_transport transport, FILE *portnumber_file);
conn *conn_new(const int sfd, enum conn_states init_state,
                const int event_flags,
                const int read_buffer_size, enum network_transport transport,
                struct event_base *base);
void event_handler(const int fd, const short which, void *arg);
static void maximize_sndbuf(const int sfd);
static int add_msghdr(conn *c);
static void out_of_memory(conn *c, char *ascii_error);
static void dispatch_bin_command(conn * c);
static void write_io_trace_to_file(LIBEVENT_THREAD * curr_thread, FILE *t_file);
static void write_io_trace(conn *c);



enum try_read_result {
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,            /** an error occured (on the socket) (or client closed connection) */
    READ_MEMORY_ERROR      /** failed to allocate more memory */
};


struct settings global_settings;
struct stats_t stats;
FILE *trace_file = NULL;

struct file_mapping *mapping;

enum hashfunc_type hash_type = JENKINS_HASH;

struct item_list{
	unsigned long start_offset;
	unsigned long end_offset;
	item* it;
};

/*communication*/
static conn *listen_conn = NULL;
static int max_fds;
static struct event_base *main_base;

enum transmit_result {
    TRANSMIT_COMPLETE,   /** All done writing. */
    TRANSMIT_INCOMPLETE, /** More data remaining to write. */
    TRANSMIT_SOFT_ERROR, /** Can't write any more right now. */
    TRANSMIT_HARD_ERROR  /** Can't write (c->state is set to conn_closing) */
};


conn **conns;


/* This reduces the latency without adding lots of extra wiring to be able to
 * notify the listener thread of when to listen again.
 * Also, the clock timer could be broken out into its own thread and we
 * can block the listener via a condition.
 */
static volatile bool allow_new_conns = true;
extern pthread_mutex_t *file_find_open_clean_locks;


int file_open(unsigned long fd, int cache_flags)
{
	uint64_t hv2;
	/*This hash is used for locate file in file_table, so the hashmask must be compliance with hashtable size*/
	uint64_t hv = hash(mapping[fd].file_name,mapping[fd].path_length);
	hv2 = hv & hashmask(hashpower + 1);
	/*hash value can not be larger than hashsize*/
	hv = hv & hashmask(hashpower);
	printf("hv is %ld\n",hv);

	
	pthread_mutex_lock(&file_find_open_clean_locks[hv2]);
	
	file_cache_t * file_head = assoc_find(mapping[fd].file_name, mapping[fd].path_length, hv);
	if(!file_head)
	{
		file_head = file_item_alloc_with_lock(fd);
		gettimeofday(&(file_head->l_time), NULL);
		if(file_head == NULL)
			return -1;
		else {
			file_head->fd = fd;
			file_head->file_cache_flags = cache_flags;
			file_head->file_opened++;
			file_head->path_length = mapping[fd].path_length;
			snprintf(file_head->file_name, file_head->path_length, mapping[fd].file_name);
			assoc_insert_with_lock(file_head, hv);
			printf("insert new file succeed\n");
		}
	} else {
		gettimeofday(&(file_head->l_time), NULL);
		pthread_mutex_lock(&(file_head->file_object_lock));
		file_head->file_opened++;
		pthread_mutex_unlock(&(file_head->file_object_lock));
	}
	
	pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
	
	return 0;
}

int file_close(unsigned long fd, int cache_flags)
{
	/*This hash is used for locate file in file_table, so the hashmask must be compliance with hashtable size*/
	uint64_t hv = hash(mapping[fd].file_name,mapping[fd].path_length);
	/*hash value can not be larger than hashsize*/
	hv = hv & hashmask(hashpower);
	printf("hv is %ld\n",hv);

	
	file_cache_t * file_head = assoc_find(mapping[fd].file_name, mapping[fd].path_length, hv);
	if(file_head) {
		gettimeofday(&(file_head->l_time), NULL);
		pthread_mutex_lock(&(file_head->file_object_lock));
		file_head->file_opened--;
		pthread_mutex_unlock(&(file_head->file_object_lock));
	}
}

int item_read(unsigned long fd, unsigned long offset, uint64_t length, void *data_buffer)
{
	int i = 0;
	int j = 0;
	/*temporary variable used for current read request*/
	unsigned long start_per_page = offset;
	unsigned long end_per_page = offset + length;
	unsigned long first_page = offset/RADIX_GRA;
	unsigned long last_page = (offset+length-1)/RADIX_GRA;
	int page_num = last_page - first_page + 1;
	printf("page_num is %d\n",page_num);
	first_page = first_page*RADIX_GRA;
	last_page = last_page*RADIX_GRA;
	unsigned long cur_page = first_page;


	
	/*item that already in the cache*/
	unsigned long item_valid_start, item_valid_end;

	/*for item*/
	item *it = NULL;
	item *ptr = NULL;
	item *cur_ptr = NULL;

	

	/*logic*/
	uint64_t hv2;
	uint64_t hv = hash(mapping[fd].file_name, mapping[fd].path_length);
	
	hv2 = hv & hashmask(hashpower + 1);
	hv = hv & hashmask(hashpower);
	printf("hv is %ld\n",hv);
	
	pthread_mutex_lock(&file_find_open_clean_locks[hv2]);
	
	file_cache_t * file_head = assoc_find(mapping[fd].file_name, mapping[fd].path_length, hv);
	printf("file_head find is ok\n");
	
	if(file_head == NULL)
	{
		/*This file is not be cached in memory*/
		file_head = file_item_alloc_with_lock(fd);
		if(file_head == NULL)
		{
			/*there is no space for file to be stored in file_table, then replace or fwrite*/
			lseek(fd,offset,SEEK_SET);
			read(fd, data_buffer,length);
			
			pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
			return 0;
		}
		
		gettimeofday(&(file_head->l_time), NULL);
		assoc_insert_with_lock(file_head, hv);
		printf("insert new file succeed\n");
		
		it = do_item_alloc(first_page, page_num * RADIX_GRA, page_num * RADIX_GRA);
		
		if(it == NULL)
		{
			lseek(fd,offset,SEEK_SET);
			
			pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
			return read(fd, data_buffer,length);
		}else {
			printf("item alloc succeed\n");
			lseek(fd,first_page,SEEK_SET);
			/*read all these request data in memory*/
			read(fd,it->data,page_num*RADIX_GRA);
			printf("the last num is %d\n",*((int*)(it->data + page_num*RADIX_GRA -4)));
			
			printf("read from file is ok\n");
			printf("it->data point to data %d\n",*((int*)(it->data)));
			printf("it->data + offset - first_page point to data %d\n",*((int*)(it->data + offset - first_page)));
			memcpy((char*)data_buffer,((char*)it->data) + offset - first_page,length);
			printf("memcpy is ok \n");
			do_item_link(it,file_head);
			cur_page = first_page;
			for(i=0;i<page_num;i++)
			{
				cur_page = cur_page + RADIX_GRA;
				radix_tree_insert(&(file_head->root),cur_page/RADIX_GRA,it);
			}
			
			pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
			return 1;
		}
	}
	
	/*This file is been cached in memory*/
	
	gettimeofday(&(file_head->l_time), NULL);

	i = 0;
	int k = 0;
	int w = 0;
	void *page_buf;
	page_buf = malloc(RADIX_GRA);
	unsigned long item_valid_length;
	unsigned long item_start_page; /*logical page of into file*/
	unsigned long start_page;
	unsigned long end_page;
	unsigned long start_per_page_start;
	do{
		cur_page = first_page + RADIX_GRA*i;
		start_per_page = (i==0)? offset:cur_page;
		end_per_page = (i == page_num - 1)?(offset + length-1):(start_per_page + RADIX_GRA - 1);
		it = do_item_find(fd, start_per_page, hv);
		if(it == NULL)
		{
			start_page = cur_page;
			start_per_page_start = start_per_page;
			k = k + 1;
			while(((it = do_item_find(fd, start_per_page + RADIX_GRA, hv)) == NULL)&&(i + 1 < page_num))
			{
				k++;
				i++;
				cur_page = first_page + RADIX_GRA*i;
				start_per_page = (i==0)? offset:cur_page;
				end_per_page = (i == page_num - 1)?(offset + length-1):(start_per_page + RADIX_GRA - 1);
			}
			
			/*These pages is not in the cache,we need to read this page from filesystem */
			it = do_item_alloc(start_page, RADIX_GRA * k, RADIX_GRA * k);
			if(it != NULL)
			{
				lseek(fd,start_page,SEEK_SET);
				read(fd,it->data,RADIX_GRA * k);
				for(w = 0; w < k; w++)
				{
					radix_tree_insert(&(file_head->root),cur_page/RADIX_GRA + w,it);
				}
				do_item_link(it, file_head);
				memcpy((char*)data_buffer + start_per_page_start - offset,
					it->data + start_per_page_start - start_page, end_per_page - start_per_page_start + 1);
			}else{
				/*item allocate failure*/
				lseek(fd,start_per_page_start,SEEK_SET);
				read(fd,data_buffer + start_per_page_start - offset,
					end_per_page - start_per_page_start + 1);
			}
			i++;
		} else {
			/*This page is in the cache*/
			ptr = it;
			/*This item is large or equal to RADIX_GRA*/
			item_valid_start = (start_per_page/RADIX_GRA == it->offset/RADIX_GRA)? it->offset : cur_page;
			item_valid_end = it->offset + it->length - 1;
			item_start_page = it->offset/RADIX_GRA;
			item_start_page = item_start_page*RADIX_GRA;
			
			
			if((page_num == 1)&&(i==0))
			{
				/*read request is smaller than one page*/
				if((item_valid_start <= start_per_page)&&(item_valid_end >= end_per_page))
				{
					memcpy((char*)data_buffer, it->data + start_per_page - item_start_page, offset + length - start_per_page);
					
					pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
					return offset + length - start_per_page;
				}
				else
				{
					/*data this item cached do not contain all the data this request needed*/
					if(page_buf == NULL)
						page_buf = malloc(RADIX_GRA);
					if(page_buf == NULL)
					{
						printf("Memory allocate failed for page_buf in item_read fuction\n");
						
						pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
						return -1;
					}
					lseek(fd, first_page,SEEK_SET);
					read(fd, page_buf, RADIX_GRA);
					item_valid_length = (item_valid_end > cur_page + RADIX_GRA-1)?
						(cur_page + RADIX_GRA - item_valid_start):(item_valid_end - item_valid_start + 1);
					memcpy((char*)page_buf + item_valid_start - cur_page,
						it->data+item_valid_start - item_start_page, item_valid_length);
					memcpy(it->data + cur_page - item_start_page,page_buf,RADIX_GRA);
					/*modify offset and length*/
					if(item_start_page == cur_page)
						it->offset = cur_page;
					it->length = it->length + RADIX_GRA - item_valid_length;
					/*copy data to data_buffer and complete the read request*/
					memcpy((char*)data_buffer,it->data+start_per_page-item_start_page,length);
					
					pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
					return length;
				}
			}
			/*page_num is more than 1*/
			if((item_valid_start <= start_per_page)&&(item_valid_end >= end_per_page))
			{
				/*the first page is in cache, do not need to read it from filesystem*/
				if((it->offset + it->length) >= (offset + length))
				{
					memcpy((char*)data_buffer+start_per_page-offset, it->data + start_per_page - it->offset, offset + length - start_per_page);
					i = page_num;
				}else {
					/* how many pages this item cached*/
					j = i;
					i = i + (it->offset + it->length -1)/RADIX_GRA - cur_page/RADIX_GRA + 1;
					if((it->offset+it->length)%RADIX_GRA != 0)
						i=i-1;
					
					/*copy i -j pages*/
					memcpy((char*)data_buffer+start_per_page - first_page,it->data + start_per_page - item_start_page,
							(i-j-1)*RADIX_GRA + end_per_page - start_per_page + 1);
				}
				
			}
			else {
				/* need to read this page from file system*/
				if(page_buf==NULL)
					page_buf = malloc(RADIX_GRA);
				if(page_buf==NULL)
				{
					printf("Memory allocation failed for item_read\n");
					
					pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
					return -1;
				}
				lseek(fd, cur_page,SEEK_SET);
				read(fd, page_buf, RADIX_GRA);
				item_valid_length = (item_valid_end > cur_page + RADIX_GRA-1)?
					(cur_page + RADIX_GRA - item_valid_start):(item_valid_end - item_valid_start + 1);
				memcpy((char*)page_buf + item_valid_start - cur_page,it->data+item_valid_start - item_start_page, item_valid_length);
				memcpy(it->data + cur_page - item_start_page,page_buf,RADIX_GRA);
				/*modify offset and length*/
				if(item_start_page == cur_page)
					it->offset = cur_page;
				it->length = it->length + RADIX_GRA - item_valid_length;
				/*copy data to data_buffer*/
				memcpy((char*)data_buffer,it->data+start_per_page-item_start_page,end_per_page - start_per_page + 1);
				i++;
			}
		}	
	}while(i < page_num);

	pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
	return length;	
}
int item_write(unsigned long fd, unsigned long offset, uint64_t length, void *data_buffer)
{
	/* do we needed to check the data*/
	{}
	/*file fd is in cache?*/
	int i = 0;
	int j = -1;
	unsigned long first_page = offset/RADIX_GRA;
	unsigned long last_page = (offset+length-1)/RADIX_GRA;
	int page_num = last_page - first_page + 1;

	first_page = first_page*RADIX_GRA;
	last_page = last_page*RADIX_GRA;
	unsigned long cur_page = first_page;

	unsigned long start_per_page = offset;
	unsigned long end_per_page = offset + length - 1;
	unsigned long item_valid_start, item_valid_end;

	struct item_list *item_list_w;
	item_list_w = calloc(page_num,sizeof(struct item_list));
	if(item_list_w == NULL)
	{
		printf("Memory calloc failed for item_list_w in item_write\n");
		return -1;
	}
	uint64_t hv2;
	uint64_t hv = hash(mapping[fd].file_name, mapping[fd].path_length);
	hv2 = hv & hashmask(hashpower + 1);
	hv = hv & hashmask(hashpower);
	
	pthread_mutex_lock(&file_find_open_clean_locks[hv2]);
	file_cache_t * fi_it = assoc_find(mapping[fd].file_name, mapping[fd].path_length, hv);

	item *it;
	if(fi_it == NULL)
	{
		/*This file is not be cached in memory*/
		fi_it = file_item_alloc_with_lock(fd);
		if(fi_it == NULL)
		{
			/*there is no space for file to be stored in file_table, then replace or fwrite*/
			lseek(fd,offset,SEEK_SET);
			pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
			return write(fd, data_buffer, length);
		}
		
		gettimeofday(&(fi_it->l_time), NULL);
		
		assoc_insert_with_lock(fi_it, hv);
		if(page_num > 1)
		{
			it = do_item_alloc(offset,length,length);
			memcpy((it->data + offset%RADIX_GRA), data_buffer,length);
			do {
				radix_tree_insert(&(fi_it->root),cur_page/RADIX_GRA,it);
				cur_page += RADIX_GRA;
				i++;
			}while(i < page_num);
			do_item_link(it,fi_it);
			pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
			return length;
		} else {
		/*we need to handle the write to 1K or 2K size,? no!*/
			it = do_item_alloc(offset,length,RADIX_GRA);
			radix_tree_insert(&(fi_it->root),offset/RADIX_GRA,it);
			do_item_link(it,fi_it);
			memcpy((it->data + offset%RADIX_GRA), data_buffer,length);
			pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
			return length;
		}
	}else 
	{
		gettimeofday(&(fi_it->l_time), NULL);
		/*look up data cached item*/
		do
		{
			if(i == 0)
			{
				start_per_page = offset;
				cur_page = first_page;
				end_per_page = (page_num == 1)? (offset+length-1):(cur_page+RADIX_GRA-1);
			}else
			{	
				cur_page = first_page + RADIX_GRA*i;
				start_per_page = cur_page;
				end_per_page = (i != page_num -1)?(cur_page + RADIX_GRA - 1):(offset + length - 1);
			}
			
			it = do_item_find(fd, start_per_page,hv);
			if(it == NULL)
			{	
				i++;
			} else {
				j++;
				item_valid_start = (start_per_page/RADIX_GRA == it->offset/RADIX_GRA)? it->offset : cur_page;
				item_valid_end = it->offset + it->length - 1;
				cur_page = first_page + RADIX_GRA*i;
				
				unsigned long item_start_page;
				item_start_page = it->offset/RADIX_GRA;
				item_start_page = item_start_page*RADIX_GRA;
				if(i == 0){
					/*first judge whether the data need to be write is all in cache, no need to c-o-w or read pages from filesystem*/
					if((it->offset<=offset)&&(it->offset + it->length >= offset+length)){
						memcpy(it->data+offset-item_start_page,data_buffer,length);
						pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
						return length;
					}else if((item_valid_start > offset+length-1)||(item_valid_end < offset)){
						/*there is no overlap between cache data and needed write data*/
						void *page_buf;
						page_buf = malloc(RADIX_GRA);
						if(page_buf != NULL)
						{
							lseek(fd,cur_page,SEEK_SET);
							read(fd, page_buf, RADIX_GRA);
							unsigned long valid_bytes = ((item_valid_end > cur_page + RADIX_GRA - 1)?
								 cur_page + RADIX_GRA -1: item_valid_end) - item_valid_start + 1;
							memcpy(page_buf,it->data + it->offset - item_start_page,valid_bytes);
							memcpy(it->data + item_valid_start - item_start_page,page_buf,RADIX_GRA);
							/*adjust the offset and length of this item*/
							if((it->offset/RADIX_GRA) == (it->offset + it->length - 1 )/RADIX_GRA)
							{
								/*This item only contain one page*/
								it->offset = cur_page;
								it->length = RADIX_GRA;
							} else if(item_valid_end < offset)
							{
								it->length = it->length + (RADIX_GRA - item_valid_end%RADIX_GRA);
							} else if((item_valid_start > offset+length-1)&&(item_valid_start == it->offset))
							{
								it->offset = cur_page;
								it->length = it->length + it->offset%RADIX_GRA;
							}
							free(page_buf);
							if(page_num == 1)
							{
								it->io_flags = 1;
								memcpy(it->data + offset%RADIX_GRA,data_buffer,length);
								pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
								return length;
							}
						}else {
							pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
							return -1;
						}
					}
				}
				if((i == page_num - 1)&&(i > 0))
				{
					if((item_valid_start > offset+length-1)||(item_valid_end < offset)){
						/*there is no overlap between cache data and needed write data*/
						void *page_buf;
						page_buf = malloc(RADIX_GRA);
						if(page_buf != NULL)
						{
							lseek(fd,cur_page,SEEK_SET);
							read(fd, page_buf, RADIX_GRA);
							unsigned long valid_bytes = ((item_valid_end > cur_page + RADIX_GRA - 1)?
								 cur_page + RADIX_GRA -1: item_valid_end) - item_valid_start + 1;
							memcpy((char*)page_buf,it->data + item_valid_start - item_start_page,valid_bytes);
							memcpy(it->data + cur_page - item_start_page,page_buf,RADIX_GRA);
							/*adjust the offset and length of this item*/
							if((it->offset/RADIX_GRA) == (it->offset + it->length - 1 )/RADIX_GRA)
							{
								/*This item only contain one page*/
								it->offset = cur_page;
								it->length = RADIX_GRA;
							} else if(item_valid_end < offset)
							{
								it->length = it->length + (RADIX_GRA - item_valid_end%RADIX_GRA);
							} else if((item_valid_start > offset+length-1)&&(item_valid_start == it->offset))
							{
								it->offset = cur_page;
								it->length = it->length + it->offset%RADIX_GRA;
							}
							free(page_buf);
						}else {
							pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
							return -1;
						}
					}
				}
				item_list_w[j].start_offset = it->offset;
				item_list_w[j].end_offset = (it->offset + it->length - 1);
				item_list_w[j].it = it;
				i = i + (item_valid_end/RADIX_GRA - item_valid_start/RADIX_GRA) + 1;
			}
				
		}while(i < page_num)
		/*allocate a item to store data or write data to item already cached*/;
		if(j == -1)
		{
			/*no pages this write request involve is cached in*/
			it = do_item_alloc(offset,length,page_num*RADIX_GRA);
			memcpy((it->data+offset-first_page),data_buffer,length);
			it->io_flags = 1;
			do_item_link(it, fi_it);
			i = 0;
			do{
				radix_tree_insert(&(fi_it->root),first_page/RADIX_GRA +i,it);
				i++;
			}while (i < page_num);
		}else{
			
			/* decide the item size of this write and allocate item and copy data in this item and 
			delete the radix-tree node and insert new node and unlink old item and link new item */
			
			unsigned long min_start_offset, max_end_offset;
			min_start_offset = offset;
			max_end_offset = offset+length-1;
			for(i=0;i<=j;i++)
			{
				if(item_list_w[i].start_offset < min_start_offset)
					min_start_offset = item_list_w[i].start_offset;
				if(item_list_w[i].end_offset > max_end_offset)
					max_end_offset = item_list_w[i].end_offset;
			}
			it = do_item_alloc(min_start_offset,max_end_offset - min_start_offset + 1,
				(max_end_offset/RADIX_GRA - min_start_offset/RADIX_GRA + 1)*RADIX_GRA);
			if(it == NULL)
			{
				printf("do_item_alloc failed for item_write\n");
				pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
				return -1;
			}
			unsigned long item_start_page, item_start_page1,item_end_page, item_end_page1;
			int item_page_num, item_page_num1;
			item_start_page = min_start_offset/RADIX_GRA;
			item_end_page = max_end_offset/RADIX_GRA;
			item_page_num = item_end_page - item_start_page + 1;
			item_start_page = item_start_page*RADIX_GRA;
			int k = 0;
			for(i=0;i<=j;i++)
			{
				item_start_page1 = item_list_w[i].start_offset/RADIX_GRA;
				item_end_page1= item_list_w[i].end_offset/RADIX_GRA;
				item_page_num1= item_end_page1- item_start_page1 + 1;
				item_start_page1 = item_start_page1*RADIX_GRA;
				
				memcpy(it->data + item_list_w[i].start_offset - item_start_page,
					item_list_w[i].it->data + item_list_w[i].start_offset - item_start_page1,
					item_list_w[i].end_offset - item_list_w[i].start_offset + 1);
				do{
					radix_tree_delete(&(fi_it->root), item_start_page1/RADIX_GRA + k);
					k++;
				}while (k < item_page_num1);
				item_list_w[i].it->io_flags = 0;
				do_item_unlink(item_list_w[i].it,fi_it);
			}
			k = 0;
			it->io_flags = 1;
			memcpy(it->data + offset - item_start_page, data_buffer, offset);
			do{
				radix_tree_insert(&(fi_it->root),item_start_page/RADIX_GRA + k,it);
				k++;
			}while (k < item_page_num);
			do_item_link(it,fi_it);
		}
		pthread_mutex_unlock(&file_find_open_clean_locks[hv2]);
		return length;
	}
}


static struct event maxconnsevent;


static void file_mapping_init()
{
	int i = 0;
    int next_fd = dup(1);
    int headroom = 10;      /* account for extra unexpected open FDs */
    struct rlimit rl;

    max_fds = global_settings.max_open_files + headroom + next_fd;

    /* But if possible, get the actual highest FD we can possibly ever see. */
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        max_fds = rl.rlim_max;
    } else {
        fprintf(stderr, "Failed to query maximum file descriptor; "
                        "falling back to maxfiles\n");
    }

    close(next_fd);

    if ((mapping = calloc(max_fds, sizeof(struct file_mapping))) == NULL) {
        fprintf(stderr, "Failed to allocate file mappings structures\n");
        /* This is unrecoverable so bail out early. */
        exit(1);
    }
	for(i = 0; i < max_fds; i++)
	{
		pthread_mutex_init(&(mapping[i].file_unit_lock), NULL);
	}
}

static void maxconns_handler(const int fd, const short which, void *arg) {
    struct timeval t = {.tv_sec = 0, .tv_usec = 10000};

    if (fd == -42 || allow_new_conns == false) {
        /* reschedule in 10ms if we need to keep polling */
        evtimer_set(&maxconnsevent, maxconns_handler, 0);
        event_base_set(main_base, &maxconnsevent);
        evtimer_add(&maxconnsevent, &t);
    } else {
        evtimer_del(&maxconnsevent);
        accept_new_conns(true);
    }
}


static const char *prot_text(enum protocol prot) {
    char *rv = "unknown";
    switch(prot) {
        case ascii_prot:
            rv = "ascii";
            break;
        case binary_prot:
            rv = "binary";
            break;
        case negotiating_prot:
            rv = "auto-negotiate";
            break;
    }
    return rv;
}


/*
 * Initializes the connections array. We don't actually allocate connection
 * structures until they're needed, so as to avoid wasting memory when the
 * maximum connection count is much higher than the actual number of
 * connections.
 *
 * This does end up wasting a few pointers' worth of memory for FDs that are
 * used for things other than connections, but that's worth it in exchange for
 * being able to directly index the conns array by FD.
 */
static void conn_init(void) {
    /* We're unlikely to see an FD much higher than maxconns. */
    int next_fd = dup(1);
    int headroom = 10;      /* account for extra unexpected open FDs */
    struct rlimit rl;

    max_fds = global_settings.maxconns + headroom + next_fd;

    /* But if possible, get the actual highest FD we can possibly ever see. */
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        max_fds = rl.rlim_max;
    } else {
        fprintf(stderr, "Failed to query maximum file descriptor; "
                        "falling back to maxconns\n");
    }

    close(next_fd);

    if ((conns = calloc(max_fds, sizeof(conn *))) == NULL) {
        fprintf(stderr, "Failed to allocate connection structures\n");
        /* This is unrecoverable so bail out early. */
        exit(1);
    }
}


static int new_socket(struct addrinfo *ai) {
    int sfd;
    int flags;

    if ((sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
        return -1;
    }
    
    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
		fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("setting O_NONBLOCK");
        close(sfd);
        return -1;
    }
    return sfd;
}



static int server_sockets(int port, enum network_transport transport,
                          FILE *portnumber_file)
{
    if (global_settings.inter == NULL) {
        return server_socket(global_settings.inter, port, transport, portnumber_file);
    } else /*if a server has multiple ip address, then bind a afac instance to each (ip, port) pair*/
    {
        /* tokenize them and bind to each one of them.. */
        char *b;
        int ret = 0;
        char *list = strdup(global_settings.inter);

        if (list == NULL) {
            fprintf(stderr, "Failed to allocate memory for parsing server interface string\n");
            return 1;
        }
	char *p;
        for (p = strtok_r(list, ";,", &b); p != NULL; p = strtok_r(NULL, ";,", &b)) {
            int the_port = port;
            char *s = strchr(p, ':');
            if (s != NULL) {
                *s = '\0';
                ++s;
                if (!safe_strtol(s, &the_port)) {
                    fprintf(stderr, "Invalid port number: \"%s\"", s);
                    return 1;
                }
            }
            if (strcmp(p, "*") == 0) {
                p = NULL;
            }
            ret |= server_socket(p, the_port, transport, portnumber_file);
        }
        free(list);
        return ret;
    }
}


/**
 * Create a socket and bind it to a specific port number
 * @param interface the interface to bind to
 * @param port the port number to bind to
 * @param transport the transport protocol (TCP / UDP)
 * @param portnumber_file A filepointer to write the port numbers to
 *        when they are successfully added to the list of ports we
 *        listen on.
 */
static int server_socket(const char *interface, int port,
                         enum network_transport transport, FILE *portnumber_file)
{
    int sfd;
    struct linger ling = {0, 0};
    struct addrinfo *ai;
    struct addrinfo *next;
    struct addrinfo hints = { .ai_flags = AI_PASSIVE,
                              .ai_family = AF_UNSPEC };
    char port_buf[NI_MAXSERV];
    int error;
    int success = 0;
    int flags =1;

    hints.ai_socktype = IS_UDP(transport) ? SOCK_DGRAM : SOCK_STREAM;

    if (port == -1) {
        port = 0;
    }
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    error= getaddrinfo(interface, port_buf, &hints, &ai);
    if (error != 0) {
        if (error != EAI_SYSTEM)
          fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
        else
          perror("getaddrinfo()");
        return 1;
    }

    for (next= ai; next; next= next->ai_next) {
        conn *listen_conn_add;
        if ((sfd = new_socket(next)) == -1) {
            /* getaddrinfo can return "junk" addresses,
             * we make sure at least one works before erroring.
             */
            if (errno == EMFILE) {
                /* ...unless we're out of fds */
                perror("server_socket");
                exit(EX_OSERR);
            }
            continue;
        }

#ifdef IPV6_V6ONLY
        if (next->ai_family == AF_INET6) {
            error = setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, 
				(char *) &flags, sizeof(flags));
            if (error != 0) {
                perror("setsockopt");
                close(sfd);
                continue;
            }
        }
#endif

        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
        if (IS_UDP(transport)) {
            maximize_sndbuf(sfd);
        } else {
            error = setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
            if (error != 0)
                perror("setsockopt");

            error = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
            if (error != 0)
                perror("setsockopt");

            error = setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
            if (error != 0)
                perror("setsockopt");
		maximize_sndbuf(sfd);
        }

        if (bind(sfd, next->ai_addr, next->ai_addrlen) == -1) {
            if (errno != EADDRINUSE) {
                perror("bind()");
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            close(sfd);
            continue;
        } else {
            success++;
            if (!IS_UDP(transport) && listen(sfd, global_settings.backlog) == -1) {
                perror("listen()");
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            if (portnumber_file != NULL &&
                (next->ai_addr->sa_family == AF_INET ||
                 next->ai_addr->sa_family == AF_INET6)) {
                union {
                    struct sockaddr_in in;
                    struct sockaddr_in6 in6;
                } my_sockaddr;
                socklen_t len = sizeof(my_sockaddr);
                if (getsockname(sfd, (struct sockaddr*)&my_sockaddr, &len)==0) {
                    if (next->ai_addr->sa_family == AF_INET) {
                        fprintf(portnumber_file, "%s INET: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in.sin_port));
                    } else {
                        fprintf(portnumber_file, "%s INET6: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in6.sin6_port));
                    }
                }
            }
        }

        if (IS_UDP(transport)) {
            int c;

            for (c = 0; c < global_settings.num_threads_per_udp; c++) {

		  /* Allocate one UDP file descriptor per worker thread;
                 * this allows "stats conns" to separately list multiple
                 * parallel UDP requests in progress.
                 *
                 * The dispatch code round-robins new connection requests
                 * among threads, so this is guaranteed to assign one
                 * FD to each thread.
                 */
                int per_thread_fd = c ? dup(sfd) : sfd;
                dispatch_conn_new(per_thread_fd, conn_read,
                                  EV_READ | EV_PERSIST,
                                  UDP_READ_BUFFER_SIZE, transport);
            }
        } else {
            if (!(listen_conn_add = conn_new(sfd, conn_listening,
                                             EV_READ | EV_PERSIST, 1,
                                             transport, main_base))) {
                fprintf(stderr, "failed to create listening connection\n");
                exit(EXIT_FAILURE);
            }
            listen_conn_add->next = listen_conn;
            listen_conn = listen_conn_add;
        }
    }

    freeaddrinfo(ai);

    /* Return zero iff we detected no errors in starting up connections */
    return success == 0;
}

/*
 * Frees a connection.
 */
void conn_free(conn *c) {
    if (c) {
        assert(c != NULL);
        assert(c->sfd >= 0 && c->sfd < max_fds);

        /* MEMCACHED_CONN_DESTROY(c); */
        conns[c->sfd] = NULL;
        if (c->hdrbuf)
            free(c->hdrbuf);
        if (c->msglist)
            free(c->msglist);
        if (c->rbuf)
            free(c->rbuf);
        if (c->wbuf)
            free(c->wbuf);
        if (c->ilist)
            free(c->ilist);
        if (c->suffixlist)
            free(c->suffixlist);
        if (c->iov)
            free(c->iov);
        free(c);
    }
}


conn *conn_new(const int sfd, enum conn_states init_state,
                const int event_flags,
                const int read_buffer_size, enum network_transport transport,
                struct event_base *base) {
    conn *c;

    assert(sfd >= 0 && sfd < max_fds);
    c = conns[sfd];

    if (NULL == c) {
        if (!(c = (conn *)calloc(1, sizeof(conn)))) {
            fprintf(stderr, "Failed to allocate connection object\n");
            return NULL;
        }

        c->rbuf = c->wbuf = 0;
        c->ilist = 0;
        c->suffixlist = 0;
        c->iov = 0;
        c->msglist = 0;
        c->hdrbuf = 0;

        c->rsize = read_buffer_size;
        c->wsize = DATA_BUFFER_SIZE;
        c->isize = ITEM_LIST_INITIAL;
        c->suffixsize = SUFFIX_LIST_INITIAL;
        c->iovsize = IOV_LIST_INITIAL;
        c->msgsize = MSG_LIST_INITIAL;
        c->hdrsize = 0;
	c->need_ntoh = 1;

        c->rbuf = (char *)malloc((size_t)c->rsize);
        c->wbuf = (char *)malloc((size_t)c->wsize);
        c->ilist = (item **)malloc(sizeof(item *) * c->isize);
        c->suffixlist = (char **)malloc(sizeof(char *) * c->suffixsize);
        c->iov = (struct iovec *)malloc(sizeof(struct iovec) * c->iovsize);
        c->msglist = (struct msghdr *)malloc(sizeof(struct msghdr) * c->msgsize);

		c->assemb_buf = NULL;
		c->assemb_curr = NULL;
		c->assemb_size = 0;

		c->frag_num = 0;
		c->frag_lack = -1;

        if (c->rbuf == 0 || c->wbuf == 0 || c->ilist == 0 || c->iov == 0 ||
                c->msglist == 0 || c->suffixlist == 0) {
            conn_free(c);
            fprintf(stderr, "Failed to allocate buffers for connection\n");
            return NULL;
        }


        c->sfd = sfd;
        conns[sfd] = c;
    }

    c->transport = transport;
    c->protocol = global_settings.binding_protocol;

  /* unix socket mode doesn't need this, so zeroed out.  but why
     * is this done for every command?  presumably for UDP
     * mode. 
     */
    if (!global_settings.socketpath) {
        c->request_addr_size = sizeof(c->request_addr);
    } else {
        c->request_addr_size = 0;
    }

    if (transport == tcp_transport && init_state == conn_new_cmd) {
        if (getpeername(sfd, (struct sockaddr *) &c->request_addr,
                        &c->request_addr_size)) {
            perror("getpeername");
            memset(&c->request_addr, 0, sizeof(c->request_addr));
        }
    }

    if (global_settings.verbose > 1) {
        if (init_state == conn_listening) {
            fprintf(stderr, "<%d server listening (%s)\n", sfd,
                prot_text(c->protocol));
        } else if (IS_UDP(transport)) {
            fprintf(stderr, "<%d server listening (udp)\n", sfd);
        } else if (c->protocol == negotiating_prot) {
            fprintf(stderr, "<%d new auto-negotiating client connection\n",
                    sfd);
        } else if (c->protocol == ascii_prot) {
            fprintf(stderr, "<%d new ascii client connection.\n", sfd);
        } else if (c->protocol == binary_prot) {
            fprintf(stderr, "<%d new binary client connection.\n", sfd);
        } else {
            fprintf(stderr, "<%d new unknown (%d) client connection\n",
                sfd, c->protocol);
            assert(false);
        }
    }

    c->state = init_state;
    c->rlbytes = 0;
    c->cmd = -1;
    c->rbytes = c->wbytes = 0;
    c->wcurr = c->wbuf;
    c->rcurr = c->rbuf;
    c->ritem = 0;
    c->icurr = c->ilist;
    c->suffixcurr = c->suffixlist;
    c->ileft = 0;
    c->suffixleft = 0;
    c->iovused = 0;
    c->msgcurr = 0;
    c->msgused = 0;
    c->authenticated = false;

    c->write_and_go = init_state;
    c->write_and_free = 0;
    c->item = 0;

    c->noreply = false;

    event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = event_flags;

    if (event_add(&c->event, 0) == -1) {
        perror("event_add");
        return NULL;
    }


    return c;
}

/**
 * Convert a state name to a human readable form.
 */
static const char *state_text(enum conn_states state) {
    const char* const statenames[] = { "conn_listening",
                                       "conn_new_cmd",
                                       "conn_waiting",
                                       "conn_read",
                                       "conn_parse_cmd",
                                       "conn_write",
                                       "conn_nread",
                                       "conn_swallow",
                                       "conn_closing",
                                       "conn_mwrite",
                                       "conn_closed" };
    return statenames[state];
}



/*
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
static void conn_set_state(conn *c, enum conn_states state) {
    assert(c != NULL);
    assert(state >= conn_listening && state < conn_max_state);

    if (state != c->state) {
        if (global_settings.verbose > 2) {
            fprintf(stderr, "%d: going from %s to %s\n",
                    c->sfd, state_text(c->state),
                    state_text(state));
        }

        c->state = state;
    }
}


/*
 * Shrinks a connection's buffers if they're too big.  This prevents
 * periodic large "get" requests from permanently chewing lots of server
 * memory.
 *
 * This should only be called in between requests since it can wipe output
 * buffers!
 */
static void conn_shrink(conn *c) {
    assert(c != NULL);

    if (IS_UDP(c->transport))
        return;

    if (c->rsize > READ_BUFFER_HIGHWAT && c->rbytes < DATA_BUFFER_SIZE) {
        char *newbuf;

        if (c->rcurr != c->rbuf)
            memmove(c->rbuf, c->rcurr, (size_t)c->rbytes);

        newbuf = (char *)realloc((void *)c->rbuf, DATA_BUFFER_SIZE);

        if (newbuf) {
            c->rbuf = newbuf;
            c->rsize = DATA_BUFFER_SIZE;
        }
        /* TODO check other branch... */
        c->rcurr = c->rbuf;
    }

    if (c->isize > ITEM_LIST_HIGHWAT) {
        item **newbuf = (item**) realloc((void *)c->ilist, ITEM_LIST_INITIAL * sizeof(c->ilist[0]));
        if (newbuf) {
            c->ilist = newbuf;
            c->isize = ITEM_LIST_INITIAL;
        }
    /* TODO check error condition? */
    }

    if (c->msgsize > MSG_LIST_HIGHWAT) {
        struct msghdr *newbuf = (struct msghdr *) realloc((void *)c->msglist, MSG_LIST_INITIAL * sizeof(c->msglist[0]));
        if (newbuf) {
            c->msglist = newbuf;
            c->msgsize = MSG_LIST_INITIAL;
        }
    /* TODO check error condition? */
    }

    if (c->iovsize > IOV_LIST_HIGHWAT) {
        struct iovec *newbuf = (struct iovec *) realloc((void *)c->iov, IOV_LIST_INITIAL * sizeof(c->iov[0]));
        if (newbuf) {
            c->iov = newbuf;
            c->iovsize = IOV_LIST_INITIAL;
        }
    /* TODO check return value */
    }
}

static void conn_release_items(conn *c) {
    assert(c != NULL);

    if (c->item) {
        item_remove(c->item);
        c->item = 0;
    }

    while (c->ileft > 0) {
        item *it = *(c->icurr);
        assert((it->it_flags & ITEM_SLABBED) == 0);
        item_remove(it);
        c->icurr++;
        c->ileft--;
    }

    if (c->suffixleft != 0) {
        /*
	for (; c->suffixleft > 0; c->suffixleft--, c->suffixcurr++) {
            cache_free(c->thread->suffix_cache, *(c->suffixcurr));
        }
	*/
	printf("c->suffixleft != 0\n");
    }

    c->icurr = c->ilist;
    c->suffixcurr = c->suffixlist;
}


static void conn_cleanup(conn *c) {
    assert(c != NULL);

    conn_release_items(c);

    if (c->write_and_free) {
        free(c->write_and_free);
        c->write_and_free = 0;
    }


    if (IS_UDP(c->transport)) {
        conn_set_state(c, conn_read);
    }
}

static void conn_close(conn *c) {
    assert(c != NULL);

    /* delete the event, the socket and the conn */
    event_del(&c->event);

    if (global_settings.verbose > 1)
        fprintf(stderr, "<%d connection closed.\n", c->sfd);

    conn_cleanup(c);

/*    MEMCACHED_CONN_RELEASE(c->sfd); */
    close(c->sfd);
    conn_set_state(c, conn_closed);

    pthread_mutex_lock(&conn_lock);
    allow_new_conns = true;
    pthread_mutex_unlock(&conn_lock);

    STATS_LOCK();
    stats.curr_conns--;
    STATS_UNLOCK();

    return;
}


static void reset_cmd_handler(conn *c) {
    c->cmd = -1;
    c->substate = bin_no_state;
    if(c->item != NULL) {
        item_remove(c->item);
        c->item = NULL;
    }
    conn_shrink(c);
    if (c->rbytes > 0) {
        conn_set_state(c, conn_parse_cmd);
    } else {
        conn_set_state(c, conn_waiting);
    }
}


/*
 * Sets a socket's send buffer size to the maximum allowed by the system.
 */
static void maximize_sndbuf(const int sfd) {
    socklen_t intsize = sizeof(int);
    int last_good = 0;
    int min, max, avg;
    int old_size;

    /* Start with the default size. */
    if (getsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &old_size, &intsize) != 0) {
        if (global_settings.verbose > 0)
            perror("getsockopt(SO_SNDBUF)");
        return;
    }

    /* Binary-search for the real maximum. */
    min = old_size;
    max = MAX_SENDBUF_SIZE;

    while (min <= max) {
        avg = ((unsigned int)(min + max)) / 2;
        if (setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&avg, intsize) == 0) {
            last_good = avg;
            min = avg + 1;
        } else {
            max = avg - 1;
        }
    }

    if (global_settings.verbose > 1)
        fprintf(stderr, "<%d send buffer was %d, now %d\n", sfd, old_size, last_good);
}

static bool update_event(conn *c, const int new_flags) {
    assert(c != NULL);

    struct event_base *base = c->event.ev_base;
    if (c->ev_flags == new_flags)
        return true;
    if (event_del(&c->event) == -1) return false;
    event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;
    if (event_add(&c->event, 0) == -1) return false;
    return true;
}


void event_handler(const int fd, const short which, void *arg) {
    conn *c;

    c = (conn *)arg;
    assert(c != NULL);

    c->which = which;

    /* sanity */
    if (fd != c->sfd) {
        if (global_settings.verbose > 0)
            fprintf(stderr, "Catastrophic: event fd doesn't match conn fd!\n");
        conn_close(c);
        return;
    }

    drive_machine(c);

    /* wait for next event */
    return;
}


/*
 * Sets whether we are listening for new connections or not.
 */
void do_accept_new_conns(const bool do_accept) {
    conn *next;

    for (next = listen_conn; next; next = next->next) {
        if (do_accept) {
            update_event(next, EV_READ | EV_PERSIST);
            if (listen(next->sfd, global_settings.backlog) != 0) {
                perror("listen");
            }
        }
        else {
            update_event(next, 0);
            if (listen(next->sfd, 0) != 0) {
                perror("listen");
            }
        }
    }

    if (do_accept) {
        STATS_LOCK();
        stats.accepting_conns = true;
        STATS_UNLOCK();
    } else {
        STATS_LOCK();
        stats.accepting_conns = false;
        stats.listen_disabled_num++;
        STATS_UNLOCK();
        allow_new_conns = false;
        maxconns_handler(-42, 0, 0);
    }
}


/*
 * Constructs a set of UDP headers and attaches them to the outgoing messages.
 */
static int build_udp_headers(conn *c) {
    int i;
    unsigned char *hdr;

    assert(c != NULL);

    if (c->msgused > c->hdrsize) {
        void *new_hdrbuf;
        if (c->hdrbuf) {
            new_hdrbuf = realloc(c->hdrbuf, c->msgused * 2 * UDP_HEADER_SIZE);
        } else {
            new_hdrbuf = malloc(c->msgused * 2 * UDP_HEADER_SIZE);
        }

        if (! new_hdrbuf) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
        c->hdrbuf = (unsigned char *)new_hdrbuf;
        c->hdrsize = c->msgused * 2;
    }

    hdr = c->hdrbuf;
    for (i = 0; i < c->msgused; i++) {
        c->msglist[i].msg_iov[0].iov_base = (void*)hdr;
        c->msglist[i].msg_iov[0].iov_len = UDP_HEADER_SIZE;
        *hdr++ = c->request_id / 256;
        *hdr++ = c->request_id % 256;
        *hdr++ = i / 256;
        *hdr++ = i % 256;
        *hdr++ = c->msgused / 256;
        *hdr++ = c->msgused % 256;
        *hdr++ = 0;
        *hdr++ = 0;
        assert((void *) hdr == (caddr_t)c->msglist[i].msg_iov[0].iov_base + UDP_HEADER_SIZE);
    }

    return 0;
}


/*
 * Ensures that there is room for another struct iovec in a connection's
 * iov list.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int ensure_iov_space(conn *c) {
    assert(c != NULL);

    if (c->iovused >= c->iovsize) {
        int i, iovnum;
        struct iovec *new_iov = (struct iovec *)realloc(c->iov,
                                (c->iovsize * 2) * sizeof(struct iovec));
        if (! new_iov) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
        c->iov = new_iov;
        c->iovsize *= 2;

        /* Point all the msghdr structures at the new list. */
        for (i = 0, iovnum = 0; i < c->msgused; i++) {
            c->msglist[i].msg_iov = &c->iov[iovnum];
            iovnum += c->msglist[i].msg_iovlen;
        }
    }

    return 0;
}


/*
* Adds data to the list of pending data that will be be written out to a connection.
*
* Returns 0 on success, -1 on out-of-memory
*/
static int add_iov(conn *c, const void *buf, int len)
{
	assert(c != NULL);
	struct msghdr *m;
	int leftover;
	bool limit_to_mtu;

	do {
		m = &c->msglist[c->msgused - 1];

		/*
        	*  Limit UDP packets, and the first payloads of TCP replies, to
         	*  UDP_MAX_PAYLOAD_SIZE bytes.
        	*/
		limit_to_mtu = IS_UDP(c->transport) || (1 == c->msgused);

		if(m->msg_iovlen == IOV_MAX || 
			(limit_to_mtu && c->msgbytes >= UDP_MAX_PAYLOAD_SIZE))
		{
			add_msghdr(c);
			m = &c->msglist[c->msgused - 1];
		}

		if(ensure_iov_space(c) != 0)
			return -1;

		if(limit_to_mtu && len + c->msgbytes > UDP_MAX_PAYLOAD_SIZE)
		{
			leftover = len + c->msgbytes - UDP_MAX_PAYLOAD_SIZE;
			len -= leftover;
		} else {
			leftover = 0;
		}

		m = &c->msglist[c->msgused - 1];
		m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
		m->msg_iov[m->msg_iovlen].iov_len = len;

		m->msg_iovlen++;
		c->msgbytes += len;
		c->iovused++;

		buf = ((char*)buf) + len;
		len = leftover;
		
	}while(leftover != 0);
}


/* Add a message header to a connection
 *
 * Returns 0 on success, -1 on out-of-memory
 *
 */
static int add_msghdr(conn *c)
{
	assert(c != NULL);
	struct msghdr *msg;
	if(c->msgsize == c->msgused)
	{
		msg = realloc(c->msglist, c->msgsize * 2 * sizeof(struct msghdr));
		if(!msg)
		{
			STATS_LOCK();
			stats.malloc_fails++;
			STATS_UNLOCK();
			return -1;
		}
		c->msglist = msg;
		c->msgsize *= 2;
		
	}

	msg = c->msglist + c->msgused;
	memset(msg, 0, sizeof(struct msghdr));
	msg->msg_iov = &c->iov[c->iovused];

	if(IS_UDP(c->transport) && c->request_addr_size > 0)
	{
		msg->msg_name = &c->request_addr;
		msg->msg_namelen = c->request_addr_size;
	}
	c->msgbytes = 0;
	c->msgused++;
	
	if(IS_UDP(c->transport))
	{
		/* Leave room for the UDP header, which we'll fill in later. */
		return add_iov(c, NULL, UDP_HEADER_SIZE);
	}

	return 0;

}


static void add_bin_header(conn *c, uint16_t err, uint8_t para_num, uint64_t body_len,
	uint8_t response_id,	uint8_t is_frag, uint8_t frag_id, uint64_t response_len, uint64_t para_len,
	uint64_t para1_len, uint64_t para2_len, uint64_t para3_len, uint64_t para4_len, uint64_t para5_len, 
	uint64_t para6_len, uint64_t frag_offset) {
	
    protocol_binary_response_header* header;
    assert(c);

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0) {
     /* This should never run out of memory because iov and msg lists
         * have minimum sizes big enough to hold an error response.
         */
        out_of_memory(c, "SERVER_ERROR out of memory adding binary header");
        return;
    }

    header = (protocol_binary_response_header *)c->wbuf;

    header->response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    header->response.opcode = c->binary_header.request.opcode;
	header->response.isfrag = is_frag;
	header->response.frag_id = frag_id;
	header->response.response_id = response_id;
	header->response.para_num = para_num;
	header->response.reserved = 0;
	header->response.body_len = htonll(body_len);
	header->response.response_len = htonll(response_len);
	header->response.para_len = htonll(para_len);
	header->response.para1_len = htonll(para1_len);
	header->response.para2_len = htonll(para2_len);
	header->response.para3_len = htonll(para3_len);
	header->response.para4_len = htonll(para4_len);
	header->response.para5_len = htonll(para5_len);
	header->response.para6_len = htonll(para6_len);
	header->response.frag_offset = htonll(frag_offset);


    if (global_settings.verbose > 1) {
        int ii;
        fprintf(stderr, ">%d Writing bin response:", c->sfd);
        for (ii = 0; ii < sizeof(header->bytes); ++ii) {
            if (ii % 4 == 0) {
                fprintf(stderr, "\n>%d  ", c->sfd);
            }
            fprintf(stderr, " 0x%02x", header->bytes[ii]);
        }
        fprintf(stderr, "\n");
    }

    add_iov(c, c->wbuf, sizeof(header->response));
}




/**
 * Writes a binary error response. If errstr is supplied, it is used as the
 * error text; otherwise a generic description of the error status code is
 * included.
 */
static void write_bin_error(conn *c, protocol_binary_response_status err,
                            const char *errstr, int swallow) {
    size_t len;

    if (!errstr) {
        switch (err) {
        case PROTOCOL_BINARY_RESPONSE_ENOMEM:
            errstr = "Out of memory";
            break;
        case PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND:
            errstr = "Unknown command";
            break;
        case PROTOCOL_BINARY_RESPONSE_EINVAL:
            errstr = "Invalid arguments";
            break;
        default:
            assert(false);
            errstr = "UNHANDLED ERROR";
            fprintf(stderr, ">%d UNHANDLED ERROR: %d\n", c->sfd, err);
        }
    }

    if (global_settings.verbose > 1) {
        fprintf(stderr, ">%d Writing an error: %s\n", c->sfd, errstr);
    }

    len = strlen(errstr);
    add_bin_header(c, err, 0, len, (len + sizeof(protocol_binary_response_header))%256, 0, 1,
		len + sizeof(protocol_binary_response_header), 0, 0, 0, 0, 0, 0, 0, 0);
    if (len > 0) {
        add_iov(c, errstr, len);
    }
    conn_set_state(c, conn_mwrite);
    if(swallow > 0) {
        c->sbytes = swallow;
        c->write_and_go = conn_swallow;
    } else {
        c->write_and_go = conn_new_cmd;
    }
}


/* Form and send a response to a command over the binary protocol */
static void write_bin_response(conn *c, void *d, uint16_t err, uint8_t para_num, uint64_t body_len,
		uint8_t response_id, uint8_t is_frag, uint8_t frag_id, uint64_t response_len, uint64_t para_len,
		uint64_t para1_len, uint64_t para2_len, uint64_t para3_len, uint64_t para4_len, uint64_t para5_len, 
		uint64_t para6_len, uint64_t frag_offset){

	add_bin_header(c, err, para_num, body_len, response_id, is_frag, frag_id, response_len, para_len,
		para1_len, para2_len, para3_len, para4_len, para5_len, para6_len, frag_offset);
    if(body_len > 0) {
    	add_iov(c, d, body_len);
    }
    conn_set_state(c, conn_mwrite);
    c->write_and_go = conn_new_cmd;
}




static void out_string(conn *c, const char *str) {
    size_t len;

    assert(c != NULL);

    if (c->noreply) {
        if (global_settings.verbose > 1)
            fprintf(stderr, ">%d NOREPLY %s\n", c->sfd, str);
        c->noreply = false;
        conn_set_state(c, conn_new_cmd);
        return;
    }

    if (global_settings.verbose > 1)
        fprintf(stderr, ">%d %s\n", c->sfd, str);

    /* Nuke a partial output... */
    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    add_msghdr(c);

    len = strlen(str);
    if ((len + 2) > c->wsize) {
        /* ought to be always enough. just fail for simplicity */
        str = "SERVER_ERROR output line too long";
        len = strlen(str);
    }

    memcpy(c->wbuf, str, len);
    memcpy(c->wbuf + len, "\r\n", 2);
    c->wbytes = len + 2;
    c->wcurr = c->wbuf;

    conn_set_state(c, conn_write);
    c->write_and_go = conn_new_cmd;
    return;
}



/*
 * Outputs a protocol-specific "out of memory" error. For ASCII clients,
 * this is equivalent to out_string().
 */
static void out_of_memory(conn *c, char *ascii_error) {

	const static char error_prefix[] = "SERVER_ERROR ";
    const static int error_prefix_len = sizeof(error_prefix) - 1;

    if (c->protocol == binary_prot) {
        /* Strip off the generic error prefix; it's irrelevant in binary */
        if (!strncmp(ascii_error, error_prefix, error_prefix_len)) {
            ascii_error += error_prefix_len;
        }
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, ascii_error, 0);
    } else {
        out_string(c, ascii_error);
    }
}



static void write_io_trace(conn *c)
{
	assert(c != NULL);
	if(c->thread->file_trace_m.count < c->thread->file_trace_m.max) {
		memcpy(&(c->thread->file_trace_m.file_trace[c->thread->file_trace_m.count]), 
			&(c->request_trace),sizeof(c->request_trace));
		c->thread->file_trace_m.count++;
	} else {
		write_io_trace_to_file(c->thread, trace_file);
		write_io_trace(c);
	}
}

static void write_io_trace_to_file(LIBEVENT_THREAD * curr_thread, FILE *t_file)
{
	assert(curr_thread != NULL);
	if(t_file == NULL)
	{
		printf("File for trace did not opened success\n");
		return;
	} else {
		fwrite(curr_thread->file_trace_m.file_trace, sizeof(struct io_trace),
			curr_thread->file_trace_m.count, t_file);
		memset(curr_thread->file_trace_m.file_trace, 0, 
			curr_thread->file_trace_m.count * sizeof(struct io_trace));
		curr_thread->file_trace_m.count = 0;
	}
	
}

// RDMA

typedef struct
{
	pthread_mutex_t trans_mutex;
	glex_ep_t *ep;
	char* ep_mem;
	struct glex_ep_attr ep_attr;
	glex_ep_addr_t ep_addr;
	glex_mh_t mh;
} rdma_endpoint;

typedef struct
{
	glex_mh_t mh;
	glex_ep_addr_t ep_addr;
	uint32_t offset;
	uint32_t len;

	int src_srv_id;
	int mem_blk_id;		// the mem_blk_id of src endpoint of rdma_pull
} rdma_data_req;

static rdma_endpoint *srv_rdmas;
pthread_cond_t srv_mem_blocks_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t srv_mem_blocks_mutex = PTHREAD_MUTEX_INITIALIZER;
static rdma_mem_block *srv_mem_blocks;
static int MAX_NUM_SRV_RDMA_MEM_BLOCK;
static int num_free_srv_rdma_mem_block;

static int SRV_RDMA_EP_NUM = 8;
static int SRV_MEM_BLK_PER_EP = 8;
static int SIZE_RDMA_MEM_BLK = 8;


static void rdma_init()
{
    srv_rdmas = calloc(1, sizeof(rdma_endpoint) * config_param.SRV_RDMA_EP_NUM);

    glex_ret_t ret;
    size_t page_size = sysconf(_SC_PAGESIZE);
    uint32_t ep_mem_size = ((uint32_t) SIZE_RDMA_MEM_BLK) * config_param.SRV_MEM_BLK_PER_EP;

    int i;
    for(i=0; i<config_param.SRV_RDMA_EP_NUM; i++)
    {
        if (posix_memalign((void **)&srv_rdmas[i].ep_mem, page_size, ep_mem_size))
        {
            printf("error: cannot allocate memory\n");
            exit(1);
        }
        memset(srv_rdmas[i].ep_mem, 0, ep_mem_size);
        if(DEBUG) fprintf(stderr, "Init RDMA Memory: [%d Bytes]\n", ep_mem_size);

        ret = glex_init();
        if (ret != GLEX_SUCCESS)
        {
            printf("error: rdma_init(), return: %d\n", ret);
            exit(1);
        }

        srv_rdmas[i].ep_attr.key				= 0x66;
        srv_rdmas[i].ep_attr.cap.dq_capacity	= GLEX_EP_CAP_DQ_CAPACITY_DEFAULT;
        srv_rdmas[i].ep_attr.cap.cq_capacity	= GLEX_EP_CAP_CQ_CAPACITY_DEFAULT;
        srv_rdmas[i].ep_attr.cap.mpq_capacity	= GLEX_EP_CAP_MPQ_CAPACITY_DEFAULT;

        uint32_t local_ep_num = GLEX_ANY_EP_NUM;

        ret = glex_create_ep(0, local_ep_num, &srv_rdmas[i].ep_attr, &srv_rdmas[i].ep);
        if (ret != GLEX_SUCCESS)
        {
            printf("error: _create_ep(), return: %d\n", ret);
            exit(1);
        }

        glex_get_ep_addr(srv_rdmas[i].ep, &srv_rdmas[i].ep_addr);

        ret = glex_register_mem(srv_rdmas[i].ep, srv_rdmas[i].ep_mem, ep_mem_size, &srv_rdmas[i].mh);
        if (ret != GLEX_SUCCESS)
        {
            printf("error: _register_mem(), return: %d\n", ret);
            exit(1);
        }

        pthread_mutex_init(&srv_rdmas[i].trans_mutex, NULL);
    }

    MAX_NUM_SRV_RDMA_MEM_BLOCK = config_param.SRV_MEM_BLK_PER_EP * config_param.SRV_RDMA_EP_NUM;
    srv_mem_blocks = calloc(1, sizeof(rdma_mem_block) * MAX_NUM_SRV_RDMA_MEM_BLOCK);
    for(i=0; i<MAX_NUM_SRV_RDMA_MEM_BLOCK; i++)
    {
        srv_mem_blocks[i].is_free = 1;
        srv_mem_blocks[i].id = i;
        srv_mem_blocks[i].endpoint = &srv_rdmas[i / config_param.SRV_MEM_BLK_PER_EP];
        srv_mem_blocks[i].mem = srv_mem_blocks[i].endpoint->ep_mem + (i % config_param.SRV_MEM_BLK_PER_EP) * SIZE_RDMA_MEM_BLK;
        assert(srv_mem_blocks[i].mem);
    }
    num_free_srv_rdma_mem_block = MAX_NUM_SRV_RDMA_MEM_BLOCK;
}

static void rdma_finalize()
{
    int i;
    for(i=0; i<config_param.SRV_RDMA_EP_NUM; i++)
    {
        free(srv_rdmas[i].ep_mem);
    }
    free(srv_rdmas);
    free(srv_mem_blocks);
}

static int last_assign_mem_blk = -1;
static rdma_mem_block* get_mem_block()
{
    rdma_mem_block* ret = NULL;
    pthread_mutex_lock(&srv_mem_blocks_mutex);

    if(num_free_srv_rdma_mem_block == 0)
    {
        ret = NULL;
    }
    else
    {
        int count, cur_blk;
        for(count=0; count<MAX_NUM_SRV_RDMA_MEM_BLOCK; count++)
        {
            cur_blk = (last_assign_mem_blk + 1 + count) % MAX_NUM_SRV_RDMA_MEM_BLOCK;
            if(srv_mem_blocks[cur_blk].is_free)
            {
                ret = &srv_mem_blocks[cur_blk];
                srv_mem_blocks[cur_blk].is_free = 0;
                num_free_srv_rdma_mem_block --;
                last_assign_mem_blk = cur_blk;
                break;
            }
        }
    }
    pthread_mutex_unlock(&srv_mem_blocks_mutex);

    return ret;
}

static void release_mem_block(int mem_block_id)
{
    assert(mem_block_id >= 0 && mem_block_id < MAX_NUM_SRV_RDMA_MEM_BLOCK);

    pthread_mutex_lock(&srv_mem_blocks_mutex);
    assert(srv_mem_blocks[mem_block_id].is_free == 0);
    srv_mem_blocks[mem_block_id].is_free = 1;
    num_free_srv_rdma_mem_block ++;
    if(num_free_srv_rdma_mem_block == 1)
    {
        //pthread_cond_signal(&srv_mem_blocks_cond);
    }
    pthread_mutex_unlock(&srv_mem_blocks_mutex);
}


static void complete_nread(conn *c) 
{
	assert(c != NULL);
	assert(c->protocol == binary_prot);

	FILE *ret;
	int fd;
	char path[PATH_MAX];
	char mode[8];
	int open_flags = 0;
	int open_mode = 0;
	uint32_t hv;
	struct timeval start, finish;
	uint64_t offset, length, s_offset;
	off_t seek_offset;
	int whence = 0;
	size_t size, nmemb, return_v;
	uint64_t buf_offset = 0;
	uint64_t data_length = 0;
	int process_id;
	void *data;
	file_cache_t* file_head;
	
	if(c->frag_num == 1)
	{
		protocol_binary_request_header *req;
		req = (protocol_binary_request_header *)c->ritem;
		assert(req->request.opcode == c->cmd);
		switch(c->cmd){
			case PROTOCOL_BINARY_CMD_CREAT:
				break;
			case PROTOCOL_BINARY_CMD_CREAT64:
				break;
			case PROTOCOL_BINARY_CMD_OPEN64:
				assert(req->request.para_num == 2 || req->request.para_num == 3);
				/*client and server may be on the same node, so pay attention to the dynamic and static
				style to intercept I/O, else may cause to crack*/
				buf_offset += sizeof(protocol_binary_request_header);
				snprintf(path, req->request.para1_len, (char*)req + buf_offset);
				buf_offset += req->request.para1_len;
				
				memcpy(&open_flags, (char*)req + buf_offset, req->request.para2_len);
				buf_offset += req->request.para2_len;

				if(req->request.para_num == 3)
				{
					memcpy(&open_mode, (char*)req + buf_offset, req->request.para3_len);
				}
				
				gettimeofday(&start,NULL);
				if(req->request.para_num == 3)
					fd = open64(path, open_flags, open_mode);
				else
					fd = open64(path, open_flags);
				gettimeofday(&finish, NULL);
				c->write_and_free = malloc(sizeof(int));
				memcpy(c->write_and_free, &fd, sizeof(int));
				write_bin_response(c, c->write_and_free, 0, 1, sizeof(fd), 0, 1, 1, sizeof(fd), sizeof(fd), sizeof(fd), 0, 0, 0, 0, 0, 0);

				mapping[fd].path_length = req->request.para1_len;
				snprintf(mapping[fd].file_name, req->request.para1_len,path);
				mapping[fd].fptr = NULL;
				mapping[fd].count += 1;
				mapping[fd].flag = 1;
				fprintf(stderr, "File handle is after open64 %d\n", fd);
				file_open(fd,((int)1 << 16) - 1);
				
				/*for trace: file_name, client name exc_time*/
				c->request_trace.exc_time = (finish.tv_sec - start.tv_sec) * 1000000 +
					finish.tv_usec - start.tv_usec;
				snprintf(c->request_trace.client_name, sizeof(c->request_trace.client_name), "%d", inet_ntoa(c->request_addr.sin_addr));
				if(strlen(path) < sizeof(c->request_trace.file_name))
				{	
					snprintf(c->request_trace.file_name, sizeof(c->request_trace.file_name), "%s", path);
				} else {
					snprintf(c->request_trace.file_name, sizeof(c->request_trace.file_name), "%s", path + strlen(path) - sizeof(c->request_trace.file_name));
				}
				c->request_trace.length = c->request_trace.offset = 0;
				write_io_trace(c);
				write_io_trace_to_file(c->thread, trace_file);
				break;
			case PROTOCOL_BINARY_CMD_OPEN:
				
				assert(req->request.para_num == 2 || req->request.para_num == 3);
				/*client and server may be on the same node, so pay attention to the dynamic and static
				style to intercept I/O, else may cause to crack*/
				buf_offset += sizeof(protocol_binary_request_header);
				snprintf(path, req->request.para1_len, (char*)req + buf_offset);
				buf_offset += req->request.para1_len;
				
				memcpy(&open_flags, (char*)req + buf_offset, req->request.para2_len);
				buf_offset += req->request.para2_len;

				if(req->request.para_num == 3)
				{
					memcpy(&open_mode, (char*)req + buf_offset, req->request.para3_len);
				}
				
				gettimeofday(&start,NULL);
				if(req->request.para_num == 3)
					fd = open(path, open_flags, open_mode);
				else
					fd = open(path, open_flags);
				gettimeofday(&finish, NULL);
				c->write_and_free = malloc(sizeof(int));
				memcpy(c->write_and_free, &fd, sizeof(int));
				write_bin_response(c, c->write_and_free, 0, 1, sizeof(fd), 0, 1, 1, sizeof(fd), sizeof(fd), sizeof(fd), 0, 0, 0, 0, 0, 0);

				mapping[fd].path_length = req->request.para1_len;
				snprintf(mapping[fd].file_name, req->request.para1_len,path);
				mapping[fd].fptr = NULL;
				mapping[fd].count += 1;
				mapping[fd].flag = 1;
				fprintf(stderr, "The fd is %d\n", fd);
				file_open(fd,((int)1 << 16) - 1);
				
				/*for trace: file_name, client name exc_time*/
				c->request_trace.exc_time = (finish.tv_sec - start.tv_sec) * 1000000 +
					finish.tv_usec - start.tv_usec;
				snprintf(c->request_trace.client_name, sizeof(c->request_trace.client_name), "%d", inet_ntoa(c->request_addr.sin_addr));
				if(strlen(path) < sizeof(c->request_trace.file_name))
				{	
					snprintf(c->request_trace.file_name, sizeof(c->request_trace.file_name), "%s", path);
				} else {
					snprintf(c->request_trace.file_name, sizeof(c->request_trace.file_name), "%s", path + strlen(path) - sizeof(c->request_trace.file_name));
				}
				c->request_trace.length = c->request_trace.offset = 0;
				write_io_trace(c);
				write_io_trace_to_file(c->thread, trace_file);
				break;
			case PROTOCOL_BINARY_CMD_CLOSE:
				assert(req->request.para_num == 1);
				buf_offset += sizeof(protocol_binary_request_header);
				fd = *(int *)((char *)req + buf_offset);

				return_v = close(fd);
				c->write_and_free = malloc(sizeof(return_v));
				memcpy(c->write_and_free, &return_v, sizeof(return_v));
				write_bin_response(c, c->write_and_free, 0, 1, sizeof(return_v), 0, 1, 1, 
					sizeof(return_v), sizeof(return_v), sizeof(return_v), 0, 0, 0, 0, 0, 0);
				file_close(fd,0);
				memset(mapping[fd].file_name, 0, mapping[fd].path_length);
				mapping[fd].path_length = 0;
				mapping[fd].count -= 1;
				mapping[fd].flag = -1;
				mapping[fd].fptr = NULL;
				break;
			case PROTOCOL_BINARY_CMD_WRITE:
				assert(req->request.para_num == 5);
				buf_offset += sizeof(protocol_binary_request_header);

				fd = *((int *)((char *)req + buf_offset));
				assert(req->request.para1_len == sizeof(int));
				buf_offset += req->request.para1_len;
				
				data = (char *)req + buf_offset;
				data_length = req->request.para2_len;
				buf_offset += req->request.para2_len;

				size = *((size_t *)((char *)req + buf_offset));
				assert(req->request.para3_len == sizeof(size_t));
				buf_offset += req->request.para3_len;

				offset = *((long *)((char *)req + buf_offset));
				assert(sizeof(long) == req->request.para4_len);
				buf_offset += req->request.para4_len;

				process_id = *((int *)((char *)req + buf_offset));
				assert(sizeof(process_id) == req->request.para5_len);
				fprintf(stderr, "File handle is %d, and mapping[fd].flag is %d\n", fd, mapping[fd].flag);
				assert(mapping[fd].flag == 1);
				

				lseek(fd, offset, SEEK_SET);
				gettimeofday(&start,NULL);
				/*return_v = fwrite(data, size, nmemb, ret); */
				return_v = item_write(fd, offset, size, data);
				gettimeofday(&finish,NULL);
				c->write_and_free = malloc(sizeof(return_v));
				memcpy(c->write_and_free, &return_v, sizeof(return_v));
				printf("Data_size written is %d\n", return_v);
				write_bin_response(c, c->write_and_free, 0, 1, sizeof(return_v), 0, 1, 1, 
					sizeof(return_v), sizeof(return_v), sizeof(return_v), 0, 0, 0, 0, 0, 0);

				/*for trace: file_name, client name exc_time*/
				c->request_trace.exc_time = (finish.tv_sec - start.tv_sec) * 1000000 +
					finish.tv_usec - start.tv_usec;
				snprintf(c->request_trace.client_name, sizeof(c->request_trace.client_name), "%s", inet_ntoa(c->request_addr.sin_addr));
				c->request_trace.pid = process_id;
				c->request_trace.length = length;
				c->request_trace.offset = offset;
				write_io_trace(c);
				write_io_trace_to_file(c->thread, trace_file);
				break;
			case PROTOCOL_BINARY_CMD_READ:
				assert(req->request.para_num == 3);
				buf_offset += sizeof(protocol_binary_request_header);

				fd = *((int *)((char *)req + buf_offset));
				assert(req->request.para1_len == sizeof(int));
				buf_offset += req->request.para1_len;

				size = *((size_t *)((char *)req + buf_offset));
				assert(req->request.para2_len == sizeof(size_t));
				buf_offset += req->request.para2_len;

				offset = *((long *)((char *)req + buf_offset));
				assert(req->request.para3_len == sizeof(long));
				buf_offset += req->request.para3_len;

			

				assert(mapping[fd].flag == 1);
				
				c->write_and_free = malloc(size + sizeof(return_v));
				lseek(fd, offset, SEEK_SET);
				
				gettimeofday(&start,NULL);
				/* return_v = fread(c->write_and_free, size, nmemb, ret);*/
				return_v = item_read(fd, offset, size, c->write_and_free);
				gettimeofday(&finish,NULL);
		
				memcpy((char *)c->write_and_free + return_v, &return_v, sizeof(return_v));
				
				write_bin_response(c, c->write_and_free, 0, 1, return_v + sizeof(return_v), 0, 1, 1, 
					return_v + sizeof(return_v), return_v + sizeof(return_v), 
					return_v, sizeof(return_v), 0, 0, 0, 0, 0);

				/*for trace: file_name, client name exc_time*/
				c->request_trace.exc_time = (finish.tv_sec - start.tv_sec) * 1000000 +
					finish.tv_usec - start.tv_usec;
				snprintf(c->request_trace.client_name, sizeof(c->request_trace.client_name), "%d", inet_ntoa(c->request_addr.sin_addr));
				c->request_trace.length = length;
				c->request_trace.offset = offset;
				break;
			case PROTOCOL_BINARY_CMD_LSEEK:
				assert(req->request.para_num == 4);
				buf_offset += sizeof(protocol_binary_request_header);

				fd = *((int *)((char *)req + buf_offset));
				assert(req->request.para1_len == sizeof(int));
				buf_offset += req->request.para1_len;

				seek_offset = *((off_t *)((char *)req + buf_offset));
				assert(req->request.para2_len == sizeof(off_t));
				buf_offset += req->request.para2_len;

				whence = *((int *)((char *)req + buf_offset));
				assert(req->request.para3_len == sizeof(int));
				buf_offset += req->request.para3_len;

				
				offset = *((long *)((char *)req + buf_offset));
				assert(req->request.para4_len == sizeof(long));
				
				switch(whence)
				{
					case SEEK_END:
						gettimeofday(&start,NULL);
						seek_offset = lseek(fd, s_offset, SEEK_END);
						gettimeofday(&finish,NULL);
						break;
					case SEEK_CUR:
					case SEEK_SET:
						printf("SEEK_CUR and SEEK_SET should be processed in local.\n");
						break;
					default:
						printf("illegal whence\n");
				}
				

				assert(mapping[fd].flag == 1);
				c->write_and_free = malloc(sizeof(seek_offset));
				memcpy(c->write_and_free, &seek_offset, sizeof(seek_offset));
				write_bin_response(c, c->write_and_free, 0, 1, sizeof(seek_offset), 0, 1, 1, 
					sizeof(seek_offset), sizeof(seek_offset), sizeof(seek_offset), 0, 0, 0, 0, 0, 0);
				
				break;
			case PROTOCOL_BINARY_CMD_LSEEK64:
				assert(req->request.para_num == 4);
				buf_offset += sizeof(protocol_binary_request_header);

				fd = *((int *)((char *)req + buf_offset));
				assert(req->request.para1_len == sizeof(int));
				buf_offset += req->request.para1_len;

				s_offset = *((off64_t *)((char *)req + buf_offset));
				assert(req->request.para2_len == sizeof(off64_t));
				buf_offset += req->request.para2_len;

				whence = *((int *)((char *)req + buf_offset));
				assert(req->request.para3_len == sizeof(int));
				buf_offset += req->request.para3_len;

				
				offset = *((long *)((char *)req + buf_offset));
				assert(req->request.para4_len == sizeof(long));
				
				switch(whence)
				{
					case SEEK_END:
						offset = lseek64(fd, s_offset, SEEK_END);
						break;
					case SEEK_CUR:
					case SEEK_SET:
						printf("SEEK_CUR and SEEK_SET should be processed in local.\n");
						break;
					default:
						printf("illegal whence\n");
				}

				assert(mapping[fd].flag == 1);
				c->write_and_free = malloc(sizeof(offset));
				memcpy(c->write_and_free, &offset, sizeof(offset));
				write_bin_response(c, c->write_and_free, 0, 1, sizeof(offset), 0, 1, 1, 
					sizeof(offset), sizeof(offset), sizeof(offset), 0, 0, 0, 0, 0, 0);

				break;
			case PROTOCOL_BINARY_CMD_PREAD:
				break;
			case PROTOCOL_BINARY_CMD_PREAD64:
				break;
			case PROTOCOL_BINARY_CMD_PWRITE:
				break;
			case PROTOCOL_BINARY_CMD_PWRITE64:
				break;
			case PROTOCOL_BINARY_CMD_READV:
				break;
			case PROTOCOL_BINARY_CMD_WRITEV:
				break;
			case PROTOCOL_BINARY_CMD___FXSTAT:
				break;
			case PROTOCOL_BINARY_CMD___FXSTAT64:
				break;
			case PROTOCOL_BINARY_CMD___LXSTAT:
				break;
			case PROTOCOL_BINARY_CMD___LXSTAT64:
				break;
			case PROTOCOL_BINARY_CMD___XSTAT:
				break;
			case PROTOCOL_BINARY_CMD___XSTAT64:
				break;
			case PROTOCOL_BINARY_CMD_MMAP:
				break;
			case PROTOCOL_BINARY_CMD_MMAP64:
				break;
			case PROTOCOL_BINARY_CMD_FOPEN:
				assert(req->request.para_num == 2);
				/*client and server may be on the same node, so pay attention to the dynamic and static
				style to intercept I/O, else may cause to crack*/
				snprintf(path, req->request.para1_len, (char*)req + sizeof(protocol_binary_request_header));
				snprintf(mode,req->request.para2_len,(char*)req +
					sizeof(protocol_binary_request_header) + req->request.para1_len);
				gettimeofday(&start,NULL);
				ret = fopen(path,mode);
				gettimeofday(&finish, NULL);
				
				write_bin_response(c, ret, 0, 2, sizeof(*ret) + sizeof(errno), 0, 1, 1, 
					sizeof(*ret) + sizeof(errno), sizeof(*ret) + sizeof(errno), sizeof(*ret), 
					sizeof(errno), 0, 0, 0, 0, 0);

				mapping[fileno(ret)].path_length = req->request.para1_len;
				snprintf(mapping[fileno(ret)].file_name, req->request.para1_len,path);
				mapping[fileno(ret)].fptr = ret;
				mapping[fileno(ret)].count += 1;
				mapping[fileno(ret)].flag = 1;

				file_open(fileno(ret),((int)1 << 16) - 1);
				
				/*for trace: file_name, client name exc_time*/
				c->request_trace.exc_time = (finish.tv_sec - start.tv_sec) * 1000000 +
					finish.tv_usec - start.tv_usec;
				snprintf(c->request_trace.client_name, sizeof(c->request_trace.client_name), "%d", inet_ntoa(c->request_addr.sin_addr));
				if(strlen(path) < sizeof(c->request_trace.file_name))
				{	
					snprintf(c->request_trace.file_name, sizeof(c->request_trace.file_name), "%s", path);
				} else {
					snprintf(c->request_trace.file_name, sizeof(c->request_trace.file_name), "%s", path + strlen(path) - sizeof(c->request_trace.file_name));
				}
				c->request_trace.length = c->request_trace.offset = 0;
				write_io_trace(c);
				write_io_trace_to_file(c->thread, trace_file);
				break;
			case PROTOCOL_BINARY_CMD_FOPEN64:
				break;
			case PROTOCOL_BINARY_CMD_FCLOSE:
				
				assert(req->request.para_num == 1);
				buf_offset += sizeof(protocol_binary_request_header);
				ret = (FILE *)((char *)req + buf_offset);
				fd = fileno(ret);
				return_v = fclose(mapping[fd].fptr);
				c->write_and_free = malloc(sizeof(return_v));
				memcpy(c->write_and_free, &return_v, sizeof(return_v));
				write_bin_response(c, c->write_and_free, 0, 1, sizeof(return_v), 0, 1, 1, 
					sizeof(return_v), sizeof(return_v), sizeof(return_v), 0, 0, 0, 0, 0, 0);
				file_close(fd,0);
				memset(mapping[fd].file_name, 0, mapping[fd].path_length);
				mapping[fd].path_length = 0;
				mapping[fd].count -= 1;
				mapping[fd].flag = -1;
				mapping[fd].fptr = NULL;
				break;
			case PROTOCOL_BINARY_CMD_FREAD:
				assert(req->request.para_num == 4);
				buf_offset += sizeof(protocol_binary_request_header);

				size = *((size_t *)((char *)req + buf_offset));
				assert(req->request.para1_len == sizeof(size_t));
				buf_offset += req->request.para1_len;

				nmemb = *((size_t *)((char *)req + buf_offset));
				assert(req->request.para2_len == sizeof(size_t));
				buf_offset += req->request.para2_len;

				ret = (FILE *)((char *)req + buf_offset);
				assert(sizeof(FILE) == req->request.para3_len);
				buf_offset += req->request.para3_len;

				offset = *((long *)((char *)req + buf_offset));
				assert(sizeof(long) == req->request.para4_len);
				buf_offset += req->request.para4_len;

			

				assert(mapping[fileno(ret)].flag == 1);
				ret = mapping[fileno(ret)].fptr;
				c->write_and_free = malloc(size * nmemb);
				fseek(ret, offset, SEEK_SET);
				gettimeofday(&start,NULL);
				/* return_v = fread(c->write_and_free, size, nmemb, ret);*/
				return_v = item_read(fileno(ret), offset, size*nmemb, c->write_and_free);
				gettimeofday(&finish,NULL);
				write_bin_response(c, c->write_and_free, 0, 1, return_v, 0, 1, 1, 
					return_v, return_v, return_v, 0, 0, 0, 0, 0, 0);

				/*for trace: file_name, client name exc_time*/
				c->request_trace.exc_time = (finish.tv_sec - start.tv_sec) * 1000000 +
					finish.tv_usec - start.tv_usec;
				snprintf(c->request_trace.client_name, sizeof(c->request_trace.client_name), "%d", inet_ntoa(c->request_addr.sin_addr));
				c->request_trace.length = length;
				c->request_trace.offset = offset;
				/*write_io_trace(c);
				write_io_trace_to_file(c->thread, trace_file);*/
				break;
			case PROTOCOL_BINARY_CMD_FWRITE:
				assert(req->request.para_num == 6);
				buf_offset += sizeof(protocol_binary_request_header);

				data = (char *)req + buf_offset;
				data_length = req->request.para1_len;
				buf_offset += req->request.para1_len;

				size = *((size_t *)((char *)req + buf_offset));
				assert(req->request.para2_len == sizeof(size_t));
				buf_offset += req->request.para2_len;

				nmemb = *((size_t *)((char *)req + buf_offset));
				assert(req->request.para3_len == sizeof(size_t));
				buf_offset += req->request.para3_len;

				ret = (FILE *)((char *)req + buf_offset);
				assert(sizeof(FILE) == req->request.para4_len);
				buf_offset += req->request.para4_len;

				offset = *((long *)((char *)req + buf_offset));
				assert(sizeof(long) == req->request.para5_len);
				buf_offset += req->request.para5_len;

				process_id = *((int *)((char *)req + buf_offset));
				assert(sizeof(process_id) == req->request.para6_len);

				assert(mapping[fileno(ret)].flag == 1);
				ret = mapping[fileno(ret)].fptr;

				fseek(ret, offset, SEEK_SET);
				gettimeofday(&start,NULL);
				/*return_v = fwrite(data, size, nmemb, ret); */
				return_v = item_write(fileno(ret), offset, size * nmemb,data);
				gettimeofday(&finish,NULL);
				c->write_and_free = malloc(sizeof(return_v));
				memcpy(c->write_and_free, &return_v, sizeof(return_v));
				write_bin_response(c, c->write_and_free, 0, 1, sizeof(return_v), 0, 1, 1, 
					sizeof(return_v), sizeof(return_v), sizeof(return_v), 0, 0, 0, 0, 0, 0);

				/*for trace: file_name, client name exc_time*/
				c->request_trace.exc_time = (finish.tv_sec - start.tv_sec) * 1000000 +
					finish.tv_usec - start.tv_usec;
				snprintf(c->request_trace.client_name, sizeof(c->request_trace.client_name), "%s", inet_ntoa(c->request_addr.sin_addr));
				c->request_trace.pid = process_id;
				c->request_trace.length = length;
				c->request_trace.offset = offset;
				write_io_trace(c);
				write_io_trace_to_file(c->thread, trace_file);
				
				break;
			case PROTOCOL_BINARY_CMD_FSEEK:
				break;
			case PROTOCOL_BINARY_CMD_FSYNC:
				break;
			case PROTOCOL_BINARY_CMD_FDATASYNC:
				break;
			case PROTOCOL_BINARY_CMD_AIO_READ:
				break;
			case PROTOCOL_BINARY_CMD_AIO_READ64:
				break;
			case PROTOCOL_BINARY_CMD_AIO_WRITE:
				break;
			case PROTOCOL_BINARY_CMD_AIO_WRITE64:
				break;
			case PROTOCOL_BINARY_CMD_LIO_LISTIO:
				break;
			case PROTOCOL_BINARY_CMD_LIO_LISTIO64:
				break;
			case PROTOCOL_BINARY_CMD_AIO_RETURN:
				break;
			case PROTOCOL_BINARY_CMD_AIO_RETURN64:
				break;
			default:
				write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL, 0);
		}
	}
	if(c->frag_num > 1)
	{
		
		protocol_binary_request_header *req;
		req = (protocol_binary_request_header *)c->assemb_buf;
		assert(req->request.opcode == c->cmd);
	}
	
	
}


static enum try_read_result try_read_udp(conn *c) {
    int res;

    assert(c != NULL);

    c->request_addr_size = sizeof(c->request_addr);
    res = recvfrom(c->sfd, c->rbuf, c->rsize,
                   0, (struct sockaddr *)&c->request_addr,
                   &c->request_addr_size);
    if (res > 70) {
        unsigned char *buf = (unsigned char *)c->rbuf;
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.bytes_read += res;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        /* Beginning of UDP packet is the request ID; save it. */
        c->request_id = buf[0] * 256 + buf[1];

        /* If this is a multi-packet request, drop it. */
        if (buf[4] != 0 || buf[5] != 1) {
            out_string(c, "SERVER_ERROR multi-packet request not supported");
            return READ_NO_DATA_RECEIVED;
        }

        /* Don't care about any of the rest of the header. */
        res -= 8;
        memmove(c->rbuf, c->rbuf + 8, res);

        c->rbytes = res;
        c->rcurr = c->rbuf;
        return READ_DATA_RECEIVED;
    }
    return READ_NO_DATA_RECEIVED;
}

/*
 * read from network as much as we can, handle buffer overflow and connection
 * close.
 * before reading, move the remaining incomplete fragment of a command
 * (if any) to the beginning of the buffer.
 *
 * To protect us from someone flooding a connection with bogus data causing
 * the connection to eat up all available memory, break out and start looking
 * at the data I've got after a number of reallocs...
 *
 * @return enum try_read_result
 */

static enum try_read_result try_read_network(conn *c) {
    enum try_read_result gotdata = READ_NO_DATA_RECEIVED;
    int res;
    int num_allocs = 0;
    assert(c != NULL);

    if (c->rcurr != c->rbuf) {
        if (c->rbytes != 0) /* otherwise there's nothing to copy */
            memmove(c->rbuf, c->rcurr, c->rbytes);
        c->rcurr = c->rbuf;
    }

    while (1) {
        if (c->rbytes >= c->rsize) {
            if (num_allocs == 4) {
                return gotdata;
            }
            ++num_allocs;
            char *new_rbuf = realloc(c->rbuf, c->rsize * 2);
            if (!new_rbuf) {
                STATS_LOCK();
                stats.malloc_fails++;
                STATS_UNLOCK();
                if (global_settings.verbose > 0) {
                    fprintf(stderr, "Couldn't realloc input buffer\n");
                }
                c->rbytes = 0; /* ignore what we read */
                out_of_memory(c, "SERVER_ERROR out of memory reading request");
                c->write_and_go = conn_closing;
                return READ_MEMORY_ERROR;
            }
            c->rcurr = c->rbuf = new_rbuf;
            c->rsize *= 2;
        }

        int avail = c->rsize - c->rbytes;
        res = read(c->sfd, c->rbuf + c->rbytes, avail);
        if (res > 0) {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.bytes_read += res;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            gotdata = READ_DATA_RECEIVED;
            c->rbytes += res;
            if (res == avail) {
                continue;
            } else {
                break;
            }
        }
        if (res == 0) {
            return READ_ERROR;
        }
        if (res == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return READ_ERROR;
        }
    }
    return gotdata;
}


/*
 * if we have a complete line in the buffer, process it.
 */
static int try_read_command(conn *c) {
    assert(c != NULL);
    assert(c->rcurr <= (c->rbuf + c->rsize));
    assert(c->rbytes > 0);

    if (c->protocol == negotiating_prot || c->transport == udp_transport)  {
        if ((unsigned char)c->rbuf[0] == (unsigned char)PROTOCOL_BINARY_REQ) {
            c->protocol = binary_prot;
        } else {
            c->protocol = ascii_prot;
        }

        if (global_settings.verbose > 1) {
            fprintf(stderr, "%d: Client using the %s protocol\n", c->sfd,
                    prot_text(c->protocol));
        }
    }

    if (c->protocol == binary_prot) {
        /* Do we have the complete packet header? */
        if (c->rbytes < sizeof(c->binary_header)) {
            /* need more data! */
            return 0;
        } else {
#ifdef NEED_ALIGN
            if (((long)(c->rcurr)) % 8 != 0) {
                /* must realign input buffer */
                memmove(c->rbuf, c->rcurr, c->rbytes);
                c->rcurr = c->rbuf;
                if (global_settings.verbose > 1) {
                    fprintf(stderr, "%d: Realign input buffer\n", c->sfd);
                }
            }
#endif
            protocol_binary_request_header* req;
            req = (protocol_binary_request_header*)c->rcurr;

            if (global_settings.verbose > 1) {
                /* Dump the packet before we convert it to host order */
                int ii;
                fprintf(stderr, "<%d Read binary protocol data:", c->sfd);
                for (ii = 0; ii < sizeof(req->bytes); ++ii) {
                    if (ii % 4 == 0) {
                        fprintf(stderr, "\n<%d   ", c->sfd);
                    }
                    fprintf(stderr, " 0x%02x", req->bytes[ii]);
                }
                fprintf(stderr, "\n");
            }

            c->binary_header = *req;
			c->binary_header.request.reserved = ntohs(req->request.reserved);
			c->binary_header.request.body_len = ntohll(req->request.body_len);
			c->binary_header.request.request_len = ntohll(req->request.request_len);
			c->binary_header.request.para_len = ntohll(req->request.para_len);
			c->binary_header.request.para1_len = ntohll(req->request.para1_len);
			c->binary_header.request.para2_len = ntohll(req->request.para2_len);
			c->binary_header.request.para3_len = ntohll(req->request.para3_len);
			c->binary_header.request.para4_len = ntohll(req->request.para4_len);
			c->binary_header.request.para5_len = ntohll(req->request.para5_len);
			c->binary_header.request.para6_len = ntohll(req->request.para6_len);
			c->binary_header.request.frag_offset = ntohll(req->request.frag_offset);

			/*for trace*/
			c->request_trace.opcode = c->binary_header.request.opcode;
			c->request_trace.request_time = time(NULL);
			gethostname(c->request_trace.server_name,sizeof(c->request_trace.server_name));

            if (c->binary_header.request.magic != PROTOCOL_BINARY_REQ) {
                if (global_settings.verbose) {
                    fprintf(stderr, "Invalid magic:  %x\n",
                            c->binary_header.request.magic);
                }
                conn_set_state(c, conn_closing);
                return -1;
            }

            c->msgcurr = 0;
            c->msgused = 0;
            c->iovused = 0;
            if (add_msghdr(c) != 0) {
                out_of_memory(c, "SERVER_ERROR Out of memory allocating headers");
                return 0;
            }

            c->cmd = c->binary_header.request.opcode;
            /* clear the returned cas value */
            c->cas = 0;
			c->body_len = c->binary_header.request.body_len;
			c->r_len = c->binary_header.request.request_len;

            dispatch_bin_command(c);

        /*    c->rbytes -= sizeof(c->binary_header);
            c->rcurr += sizeof(c->binary_header);
            */
        }
    }else
    {
    	return -1;
    }

    return 1;
}


static void request_network_to_host(conn *c, char* buffer)
{
	assert(c != NULL);

	
	protocol_binary_request_header* req;
	req = (protocol_binary_request_header*)buffer;
	
	req->request.reserved = ntohs(req->request.reserved);
	req->request.body_len = ntohll(req->request.body_len);
	req->request.request_len = ntohll(req->request.request_len);
	req->request.para_len = ntohll(req->request.para_len);
	req->request.para1_len = ntohll(req->request.para1_len);
	req->request.para2_len = ntohll(req->request.para2_len);
	req->request.para3_len = ntohll(req->request.para3_len);
	req->request.para4_len = ntohll(req->request.para4_len);
	req->request.para5_len = ntohll(req->request.para5_len);
	req->request.para6_len = ntohll(req->request.para6_len);
	req->request.frag_offset = ntohll(req->request.frag_offset);

	return;
	
}

static void bin_read_request(conn *c)
{
	assert(c != NULL);

	c->rlbytes = c->r_len;
	/*ensure the buffer can store a entire request*/
	ptrdiff_t offset = c->rcurr + sizeof(protocol_binary_request_header) - c->rbuf;
	if(c->rlbytes > c->rsize - offset)
	{
		size_t nsize = c->rsize;
		size_t size = c->rlbytes + sizeof(protocol_binary_request_header);
		while(size > nsize) {
			nsize *= 2;
		}

		if(nsize != size)
		{
			char *newm = realloc(c->rbuf, nsize);
			if(newm == NULL)
			{
				STATS_LOCK();
				stats.malloc_fails++;
				STATS_UNLOCK();
				if(global_settings.verbose)
				{
					fprintf(stderr, "%d: Failed to grow buffer.. closing connection\n", c->sfd);
				}
				conn_set_state(c, conn_closing);
			}
			c->rbuf = newm;
			c->rcurr = c->rbuf + offset - sizeof(protocol_binary_request_header);
			c->rsize = nsize;
		}
		if(c->rbuf != c->rcurr)
		{
			memmove(c->rbuf, c->rcurr, c->rbytes);
			c->rcurr = c->rbuf;
		}
	}

	/*thinking about package assembly*/
	if(c->transport == tcp_transport)
	{
		if((c->binary_header.request.isfrag > 1) && 
		  (c->binary_header.request.body_len != c->binary_header.request.request_len))
		{
			
			if(c->assemb_buf == NULL)
			{	
				
				c->assemb_buf = (char *)calloc(1, c->binary_header.request.request_len + sizeof(c->binary_header.request));
				if(c->assemb_buf == NULL)
				{	
					STATS_LOCK();
					stats.malloc_fails++;
					STATS_UNLOCK();
					if(global_settings.verbose)
					{
						fprintf(stderr, "%d: Failed to grow buffer.. closing connection\n", c->sfd);
					}
					conn_set_state(c, conn_closing);
				}
				c->assemb_size = c->binary_header.request.request_len + sizeof(c->binary_header.request);
				c->assemb_curr = c->assemb_buf;
			} else {

				c->assemb_curr = NULL;
				free(c->assemb_buf);
				c->assemb_size = 0;
				c->assemb_buf = (char *)calloc(1, c->binary_header.request.request_len + sizeof(c->binary_header.request));
				if(c->assemb_buf == NULL)
				{	
					STATS_LOCK();
					stats.malloc_fails++;
					STATS_UNLOCK();
					if(global_settings.verbose)
					{
						fprintf(stderr, "%d: Failed to grow buffer.. closing connection\n", c->sfd);
					}
					conn_set_state(c, conn_closing);
				}
				c->assemb_size = c->binary_header.request.request_len + sizeof(c->binary_header.request);
				c->assemb_curr = c->assemb_buf;
			}
			
		}
	}

	c->frag_num = c->binary_header.request.isfrag;
	c->frag_lack = c->frag_num;

	conn_set_state(c, conn_nread);
	
}

static void dispatch_bin_command(conn * c)
{
	assert(c != NULL);

	bin_read_request(c);
}




/*
 * Transmit the next chunk of data from our list of msgbuf structures.
 *
 *   Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
 */
static enum transmit_result transmit(conn *c)
{
    assert(c != NULL);

    if (c->msgcurr < c->msgused && c->msglist[c->msgcurr].msg_iovlen == 0) {
        /* Finished writing the current msg; advance to the next. */
        c->msgcurr++;
    }
    if (c->msgcurr < c->msgused) {
        ssize_t res;
        struct msghdr *m = &c->msglist[c->msgcurr];

        res = sendmsg(c->sfd, m, 0);
        if (res > 0) {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.bytes_written += res;
            pthread_mutex_unlock(&c->thread->stats.mutex);

            /* We've written some of the data. Remove the completed iovec entries from the list of pending writes. */
            while (m->msg_iovlen > 0 && res >= m->msg_iov->iov_len) {
                res -= m->msg_iov->iov_len;
                m->msg_iovlen--;
                m->msg_iov++;
            }

            /* Might have written just part of the last iovec entry; adjust it so the next write will do the rest. */
            if (res > 0) {
                m->msg_iov->iov_base = (caddr_t)m->msg_iov->iov_base + res;
                m->msg_iov->iov_len -= res;
            }
            return TRANSMIT_INCOMPLETE;
        }
        if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!update_event(c, EV_WRITE | EV_PERSIST)) {
                if (global_settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                return TRANSMIT_HARD_ERROR;
            }
            return TRANSMIT_SOFT_ERROR;
        }
        /* if res == 0 or res == -1 and error is not EAGAIN or EWOULDBLOCK,
           we have a real error, on which we close the connection */
        if (global_settings.verbose > 0)
            perror("Failed to write, and not due to blocking");

        if (IS_UDP(c->transport))
            conn_set_state(c, conn_read);
        else
            conn_set_state(c, conn_closing);
        return TRANSMIT_HARD_ERROR;
    } else {
        return TRANSMIT_COMPLETE;
    }
}


static void drive_machine(conn *c) 
{
    bool stop = false;
    int sfd;
    socklen_t addrlen;
    struct sockaddr_storage addr;
    int nreqs = global_settings.reqs_per_event;
    int res;
    const char *str;
    int need_ntoh = 1;
    int tocopy_frag;
    int tocopy;

#ifdef HAVE_ACCEPT4
    static int  use_accept4 = 1;
#else
    static int  use_accept4 = 0;
#endif

    assert(c != NULL);

    while (!stop) {

        switch(c->state) {
        case conn_listening:
            addrlen = sizeof(addr);
#ifdef HAVE_ACCEPT4
            if (use_accept4) {
                sfd = accept4(c->sfd, (struct sockaddr *)&addr, &addrlen, SOCK_NONBLOCK);
            } else {
                sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
            }
#else
            sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
#endif
            if (sfd == -1) {
                if (use_accept4 && errno == ENOSYS) {
                    use_accept4 = 0;
                    continue;
                }
                perror(use_accept4 ? "accept4()" : "accept()");
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* these are transient, so don't log anything */
                    stop = true;
                } else if (errno == EMFILE) {
                    if (global_settings.verbose > 0)
                        fprintf(stderr, "Too many open connections\n");
                    accept_new_conns(false);
                    stop = true;
                } else {
                    perror("accept()");
                    stop = true;
                }
                break;
            }
            if (!use_accept4) {
                if (fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL) | O_NONBLOCK) < 0) {
                    perror("setting O_NONBLOCK");
                    close(sfd);
                    break;
                }
            }

            if (global_settings.maxconns_fast &&
                stats.curr_conns + stats.reserved_fds >= global_settings.maxconns - 1) {
                str = "ERROR Too many open connections\r\n";
                res = write(sfd, str, strlen(str));
                close(sfd);
                STATS_LOCK();
                stats.rejected_conns++;
                STATS_UNLOCK();
            } else {
                dispatch_conn_new(sfd, conn_new_cmd, EV_READ | EV_PERSIST,
                                     DATA_BUFFER_SIZE, tcp_transport);
            }

            stop = true;
            break;

        case conn_waiting:
            if (!update_event(c, EV_READ | EV_PERSIST)) {
                if (global_settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                break;
            }

            conn_set_state(c, conn_read);
            stop = true;
            break;

        case conn_read:
            res = IS_UDP(c->transport) ? try_read_udp(c) : try_read_network(c);

            switch (res) {
            case READ_NO_DATA_RECEIVED:
                conn_set_state(c, conn_waiting);
                break;
            case READ_DATA_RECEIVED:
                conn_set_state(c, conn_parse_cmd);
                break;
            case READ_ERROR:
                conn_set_state(c, conn_closing);
                break;
            case READ_MEMORY_ERROR: /* Failed to allocate more memory */
                /* State already set by try_read_network */
                break;
            }
            break;

        case conn_parse_cmd :
            if (try_read_command(c) == 0) {
                /* wee need more data! */
                conn_set_state(c, conn_waiting);
            }

            break;

        case conn_new_cmd:
            /* Only process nreqs at a time to avoid starving other
               connections */

            --nreqs;
            if (nreqs >= 0) {
                reset_cmd_handler(c);
            } else {
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.conn_yields++;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                if (c->rbytes > 0) {
                     /* We have already read in data into the input buffer,
                       		 so libevent will most likely not signal read events
                      		 on the socket (unless more data is available. As a
                      		 hack we should just put in a request to write data,
                      		 because that should be possible ;-)
                   			 */
                    if (!update_event(c, EV_WRITE | EV_PERSIST)) {
                        if (global_settings.verbose > 0)
                            fprintf(stderr, "Couldn't update event\n");
                        conn_set_state(c, conn_closing);
                        break;
                    }
                }
                stop = true;
            }
            break;

        case conn_nread:
		
		if((c->frag_lack == 0) && (c->rlbytes == 0)) {
                	complete_nread(c);
               		break;
		}

            /* Check if rbytes < 0, to prevent crash */
                if (c->rlbytes < 0) {
                    if (global_settings.verbose) {
                        fprintf(stderr, "Invalid rlbytes to read: len %d\n", c->rlbytes);
                    }
		    conn_set_state(c, conn_closing);
                    break;
                }

		protocol_binary_request_header *req;
		req = (protocol_binary_request_header *)c->rcurr;
		
		if(c->need_ntoh)
		{
			request_network_to_host(c, c->rcurr);
			c->need_ntoh = 0;
		}
		tocopy = req->request.body_len + sizeof(protocol_binary_request_header);
		tocopy_frag = req->request.body_len;
		printf("Data need to copy is %d, already copy %d\n", tocopy, c->rbytes);
		if(c->rbytes >= tocopy)
		{
			/* This package is of integrity*/
			if(c->frag_num == 1)
			{
				c->ritem = c->rcurr;
				c->rcurr = c->rcurr + tocopy;
				c->rbytes = c->rbytes - tocopy;
				c->rlbytes -= tocopy_frag;
				c->frag_lack--;
			
				if(c->rlbytes == 0)
				{
					c->need_ntoh = 1;
					break;
				} else {
					printf("The failure thread is %u\n", pthread_self());
					printf("Data received is %d\n", c->rbytes);
					fprintf(stderr, "Connection %d failed for unknown error \n", c->sfd);
					break;
				}	
			}
			else
			{
				if(c->frag_lack == c->frag_num)
				{
					memcpy(c->assemb_buf, c->rcurr, sizeof(protocol_binary_request_header));
					c->rcurr += sizeof(protocol_binary_request_header);
					c->rbytes -= sizeof(protocol_binary_request_header);
					memmove(c->rbuf, c->rcurr, c->rbytes);
					c->rcurr = c->rbuf;
				}
				if(req->request.request_id == c->binary_header.request.request_id)
				{
					if(c->frag_flag[req->request.frag_id - 1] == 0)
					{
						memcpy(c->assemb_buf + req->request.frag_offset +
							sizeof(protocol_binary_request_header), 
							c->rcurr, req->request.body_len);
						c->frag_flag[req->request.frag_id - 1] = 1;
						c->frag_lack--;
						c->rlbytes -= req->request.body_len;
							
					}
					
					c->rcurr += req->request.body_len;
					c->rbytes -= req->request.body_len;
						
					memmove(c->rbuf, c->rcurr, c->rbytes);
					c->rcurr = c->rbuf;
					if(c->rlbytes == 0 && c->frag_lack == 0)
						break;
				} else {
					/*this is a disorder package*/
				}
			}
				
		}
            /* first check if we have leftovers in the conn_read buffer */
            

            /*  now try reading from the socket */
		if(c->rcurr != c->rbuf)
		{
			printf("c->rcurr != c->rbuf\n");
			memmove(c->rbuf, c->rcurr, c->rbytes);
			c->rcurr = c->rbuf;
		}
		int avail = c->rsize - c->rbytes;
		res = read(c->sfd, ((char *)c->rbuf + c->rbytes), avail);
		if (res > 0) {
			printf("Res in nread is %d\n", res);
			pthread_mutex_lock(&c->thread->stats.mutex);
                	c->thread->stats.bytes_read += res;
                	pthread_mutex_unlock(&c->thread->stats.mutex);
                	c->rbytes += res;
               		break;
           	 }
	    printf("We arrived here, and res is %d, errno is %d\n", res, errno);
            if (res == 0) { /* end of stream */
                conn_set_state(c, conn_closing);
                break;
            }
            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		printf("We arrived here\n");
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (global_settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
            	printf("we arrived here before stop = true\n");
	        stop = true;
		printf("we arrived here after stop = true\n");
                break;
            }
	    
            /* otherwise we have a real error, on which we close the connection */
            if (global_settings.verbose > 0) {
                fprintf(stderr, "Failed to read, and not due to blocking:\n"
                        "errno: %d %s \n"
                        "rcurr=%lx ritem=%lx rbuf=%lx rlbytes=%d rsize=%d\n",
                        errno, strerror(errno),
                        (long)c->rcurr, (long)c->ritem, (long)c->rbuf,
                        (int)c->rlbytes, (int)c->rsize);
            }
            conn_set_state(c, conn_closing);
            break;

        case conn_write:
        /*
             * We want to write out a simple response. If we haven't already,
             * assemble it into a msgbuf list (this will be a single-entry
             * list for TCP or a two-entry list for UDP).
             */
            if (c->iovused == 0 || (IS_UDP(c->transport) && c->iovused == 1)) {
                if (add_iov(c, c->wcurr, c->wbytes) != 0) {
                    if (global_settings.verbose > 0)
                        fprintf(stderr, "Couldn't build response\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
            }

            /* fall through... */

        case conn_mwrite:
          if (IS_UDP(c->transport) && c->msgcurr == 0 && build_udp_headers(c) != 0) {
            if (global_settings.verbose > 0)
              fprintf(stderr, "Failed to build UDP headers\n");
            conn_set_state(c, conn_closing);
            break;
          }
            switch (transmit(c)) {
            case TRANSMIT_COMPLETE:
                if (c->state == conn_mwrite) {
                    conn_release_items(c);
                    /* XXX:  I don't know why this wasn't the general case */
		    if (c->write_and_free) {
                        free(c->write_and_free);
                        c->write_and_free = 0;
                    }
                    if(c->protocol == binary_prot) {
                        conn_set_state(c, c->write_and_go);
                    } else {
                        conn_set_state(c, conn_new_cmd);
                    }
                } else if (c->state == conn_write) {
                    if (c->write_and_free) {
                        free(c->write_and_free);
                        c->write_and_free = 0;
                    }
                    conn_set_state(c, c->write_and_go);
                } else {
                    if (global_settings.verbose > 0)
                        fprintf(stderr, "Unexpected state %d\n", c->state);
                    conn_set_state(c, conn_closing);
                }
                break;

            case TRANSMIT_INCOMPLETE:
            case TRANSMIT_HARD_ERROR:
                break;                   /* Continue in state machine. */

            case TRANSMIT_SOFT_ERROR:
                stop = true;
                break;
            }
            break;

        case conn_closing:
            if (IS_UDP(c->transport))
                conn_cleanup(c);
            else
                conn_close(c);
            stop = true;
            break;

        case conn_closed:
        case conn_max_state:
            assert(false);
            break;
        }
    }

    return;
}


int global_settings_init()
{
	global_settings.chunk_size = 4096;
	global_settings.factor = 2;
	global_settings.item_size_max = 1024*1024*16; /*16MB, 13*16MB = 208*/
	
	global_settings.slab_reassign = 1;
	global_settings.verbose = 2;
	global_settings.hash_algorithm = NULL;
	global_settings.num_threads = 8;
	global_settings.port = 11212;
	global_settings.udpport = 11212;
	global_settings.inter = NULL;
	global_settings.maxbytes = 1024*1024*1024;	/*1GB*/
	global_settings.maxconns = 4196;
	global_settings.max_open_files = 8192;

	global_settings.socketpath = NULL;

	
	global_settings.num_threads = 8;
	global_settings.num_threads_per_udp = 0;

	global_settings.reqs_per_event = 20;
	global_settings.backlog = 1024;
	global_settings.binding_protocol = binary_prot;
	
}



int main(int argc, char **argv)
{
	int retval = EXIT_SUCCESS;
	/* initialize main thread libevent instance */

	global_settings_init();
	
	printf("global settings init ok\n");
	assoc_init(0);
	printf("assoc init ok\n");
	slabs_init(global_settings.maxbytes,global_settings.factor,NULL);
	printf("slabs_init ok\n");
	file_table_init();
	printf("file table init ok\n");
	hash_init(hash_type);
	printf("hash init ok\n");

	file_mapping_init();
	printf("file mapping init finished!\n");

	bool tcp_specified = false;
    bool udp_specified = false;
	

	if (tcp_specified && !udp_specified) {
        global_settings.udpport = global_settings.port;
    } else if (udp_specified && !tcp_specified) {
        global_settings.port = global_settings.udpport;
    }

	
	struct rlimit rlim;
  /*
     * If needed, increase rlimits to allow as many connections
     * as needed.
     */

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        fprintf(stderr, "failed to getrlimit number of files\n");
        exit(EX_OSERR);
    } else {
        rlim.rlim_cur = global_settings.maxconns;
        rlim.rlim_max = global_settings.maxconns;
        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
            fprintf(stderr, "failed to set rlimit for open files. Try starting as root or requesting smaller maxconns value.\n");
            exit(EX_OSERR);
        }
    }

	
    main_base = event_init();
	conn_init();
	thread_init(global_settings.num_threads, main_base);

	char *trace_filename = getenv("AFAC_TRACE_FILE");
	char temp_trace_filename[PATH_MAX];
	char domain_name[8];
	
	gethostname(domain_name, sizeof(domain_name));
	
	if(trace_filename != NULL)
	{
		snprintf(temp_trace_filename, sizeof(temp_trace_filename),"%s",trace_filename);
        strncat(temp_trace_filename,".",1);
        strncat(temp_trace_filename, domain_name, strlen(domain_name));
        printf("The trace file name is %s\n", temp_trace_filename);
		trace_file = fopen(temp_trace_filename,"a");
		if(trace_file == NULL) {
			printf("Failed to open \"%s\": %s\n", temp_trace_filename, strerror(errno));
		}
	}
	
	if(global_settings.socketpath == NULL)
	{
		const char *portnumber_filename = getenv("AFAC_PORT_NUMBER");
		char temp_portnumber_filename[PATH_MAX];
		FILE *portnumber_file = NULL;
		if(portnumber_filename != NULL) {
			/*.lck is file extension name */
			snprintf(temp_portnumber_filename, sizeof(temp_portnumber_filename), 
				"%s.lck", portnumber_filename);

			portnumber_file = fopen(temp_portnumber_filename, "a");
			if(portnumber_file == NULL) {
				fprintf(stderr, "Failed to open \"%s\": %s\n", 
					temp_portnumber_filename, strerror(errno));
				}
		}

		errno = 0;
		if(global_settings.port && server_sockets(global_settings.port,tcp_transport,
			portnumber_file)){
			/**/
			fprintf(stderr, "failed to listen on TCP port %d\n",global_settings.port);
			exit(EX_OSERR);
		}

		
		errno = 0;
		if(global_settings.udpport && server_sockets(global_settings.port,udp_transport,
			portnumber_file)){
			/**/
			fprintf(stderr, "failed to listen on UDP port %d\n",global_settings.udpport);
			exit(EX_OSERR);
		}

		if(portnumber_file)
		{
			fclose(portnumber_file);
			rename(temp_portnumber_filename, portnumber_filename);
		}
	}
	usleep(1000);
	
	/* enter the event loop */
    if (event_base_loop(main_base, 0) != 0) {
        retval = EXIT_FAILURE;
    }
	fclose(trace_file);
	return 0;
}
