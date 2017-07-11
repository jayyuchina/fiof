#ifndef JAY_CONFIGURE_H
#define JAY_CONFIGURE_H

#include "jay_global.h"
#include "confread.h"



typedef struct
{
	int server_debug_level;
	int client_debug_level;

	int ion_modulus;
	int ion_with_hash;
	
	int metadata_hash;
	
	int metadata_caching;

	int cache_device_tmpfs;
	char tmpfs_path[PATH_MAX];
	char ssd_path[PATH_MAX];

	// SSD Simulator
	int simulate_ssd;
	double simulate_read_latency;
	double simulate_write_latency;

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


