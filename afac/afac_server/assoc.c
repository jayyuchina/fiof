 /* This is used to maintain hash table, which is used to allocate the file cache header */
#include <stdio.h>
#include "assoc.h"
#include <sys/types.h>
#include <stdlib.h>



unsigned int hashpower = HASHPOWER_DEFAULT;

#define hashsize(n) ((unsigned long int)1<<(n))
#define hashmask(n) (hashsize(n)-1)


/*hash_table_dong used to locate data of files*/
static file_cache_t **hash_table_dong = 0;

/*number of files in the hash table*/
static unsigned int hash_file = 0;



static file_cache_t** _hashfile_before (const unsigned long fd, const uint32_t hv);

void assoc_init(const int hashpower_init)
{
	if(hashpower_init)
	{
		hashpower = hashpower_init;
	}
	hash_table_dong = calloc(hashsize(hashpower), sizeof(void *));
	if(! hash_table_dong)
	{
		fprintf(stderr, "Failed to init hashtable \n");
		exit(EXIT_FAILURE);
	}
}


file_cache_t *assoc_find(char *path, int length, const uint32_t hv)
{
	file_cache_t *fi;
	file_cache_t *ret = NULL;
	fi = hash_table_dong[hv];
	while(fi)
		{
			if(strncmp(fi->file_name, path, length) == 0)
				{
					ret = fi;
					break;
				}
			else
				{
					fi = fi->h_next;
				}
		}
	return ret;
}
int assoc_insert(file_cache_t *file, uint32_t hv)
{
	file->h_next = hash_table_dong[hv];
	hash_table_dong[hv] = file;
	return 0;
}
int assoc_delete(const unsigned long fd, uint32_t hv)
{
	file_cache_t **before = _hashfile_before(fd, hv);
	if(*before)
		{
			file_cache_t *nxt;
			nxt = (*before)->h_next;
			(*before)->next = 0;
			*before = nxt;
		}
	return 0;
}

static file_cache_t** _hashfile_before (const unsigned long fd, const uint32_t hv)
{
	file_cache_t** pos;
	pos = &hash_table_dong[hv];
	while(*pos && (*pos)->fd ==fd)
		{
			pos = &(*pos)->h_next;
		}
	return pos;
}


