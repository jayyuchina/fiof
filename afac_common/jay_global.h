
#ifndef JAY_GLOBAL_H
#define JAY_GLOBAL_H




#include "glex.h"
#include <pthread.h>
#include <linux/limits.h>

// global params

#define SERVER_LIST_PATH "/WORK/home/yujie/yujie/iof/server_list"
#define CONFIG_PARAM_PATH "/WORK/home/yujie/yujie/iof/flashcache.conf"

#define G_BLOCK_SIZE_IN_KB 1024		// 1024K
#define G_BLOCK_SIZE_IN_BYTES (uint64_t)(G_BLOCK_SIZE_IN_KB * 1024)		// 1024K
#define G_BLOCK_SIZE_SHIFT 20		// 2^20 = 1024k


#define CFD_MFD_LENGTH 32		// sizeof(int)
#define CFD_MFD_OFFSET 20		// 2^20 = 1048576 > max_fd 819200
#define DIF_CFD_FROM_REAL_FD 1	// need to differ mfd of server 0 from unhacked real fds, so here we add an OFFSET
#define MFD_TO_CFD(server_id, mfd) (((server_id + DIF_CFD_FROM_REAL_FD)<< CFD_MFD_OFFSET) | mfd)
#define CFD_TO_MFD(cfd) (cfd & (0xFFFFFFFF >> (CFD_MFD_LENGTH - CFD_MFD_OFFSET)))
#define CFD_TO_SRV_ID(cfd) ((cfd >> CFD_MFD_OFFSET) - DIF_CFD_FROM_REAL_FD)
#define CHECK_IS_CFD(cfd) (cfd >> CFD_MFD_OFFSET)	// get "srv_id", srv_id > 0 means cfd, srv_id == 0 means real fd

#define OST_CACHE_DEVICE_PATH_LEN 128

#define MAX_SOCKET_SND_BUFFER (256UL * 1024 * 1024)

// RDMA PARAMS
#define SIZE_RDMA_MEM_BLK G_BLOCK_SIZE_IN_BYTES


// SIMULATE SSD
#define SIMULATE_SSD_SEM_KEY 1314

typedef struct
{
	int requesting_srv_id;
	int cfd;
	int file_block_id;
	int start_offset;
	int end_offset;
} block_data_req;

typedef struct
{
	glex_mh_t mh;
	glex_ep_addr_t ep_addr;
	uint32_t offset;
	uint32_t len;

	int src_srv_id;
	int mem_blk_id;		// the mem_blk_id of src endpoint of rdma_pull
} rdma_data_req;

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
    int is_free;
	int id;
    rdma_endpoint *endpoint;
    char* mem;
} rdma_mem_block;


typedef struct
{
	int arg_num;
	char path[PATH_MAX];
	int flags;
	int modes;
} ost_open_arg;

typedef struct
{
	int ost_start_block_offset;
	int ost_end_block_offset;
} ost_block_offset;


#define MAX_SERVER_NAME 16
typedef struct
{
	char name[MAX_SERVER_NAME];
} server_name;


typedef struct
{
    unsigned long accumulated_data_size;
    int last_cached_server_id;
	int is_read;
} SuccessiveInfo;	// for info tranfer from client to server


typedef struct
{
	int server_id;
	int device_block_id;	// -1 means not local, -2 means need wait
} MetadataCachingEntry;

union semun 
{
	int val;   
	struct semid_ds  *buf;  
	unsigned short *array;
}sem_union;


/*
// block_access_info is used to transfer the info of blocks when retrieved from OSTs
typedef struct
{
	int block_id;
	int start_offset;
	int end_offset;
} block_access_info;
*/

#endif
