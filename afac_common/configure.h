#ifndef JAY_CONFIGURE_H
#define JAY_CONFIGURE_H

#include "jay_global.h"
#include "confread.h"



typedef struct
{
	int server_debug_level;
	int client_debug_level;

	int ion_modulus;

	int use_cache;
	int cache_device_tmpfs;
	char tmpfs_path[PATH_MAX];
	char ssd_path[PATH_MAX];

	// MULTI ION
	int customizable_ion;
	int primary_ion_hash;
	int primary_ion_pos;
	int ION_NUM_PER_FILE;
	int BLOCK_NUM_PER_ION_CHUNK;

	// LASIOD
	int LASIOD;
	int LASIOD_SCATTER_NUM;
	unsigned long LASIOD_SMALL_IO_SIZE;
	unsigned long LASIOD_LARGE_IO_SIZE;

	// RDMA
	int SRV_RDMA_EP_NUM;
	int SRV_MEM_BLK_PER_EP;
} Config_Param;


void read_config_file(Config_Param *config_param);


#endif


