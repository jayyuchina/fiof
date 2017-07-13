#include "configure.h"


static void digest_config_pair(Config_Param *config_param, struct confread_pair *pair)
{
    char * key = pair->key;
    char * value = pair->value;

    if(strcmp(key, "server debug level") == 0)
    {
        config_param->server_debug_level = atoi(value);
    }
    else if(strcmp(key, "client debug level") == 0)
    {
        config_param->client_debug_level = atoi(value);
    }
    else if(strcmp(key, "ion_modulus") == 0)
    {
        config_param->ion_modulus = atoi(value);	
    }
    else if(strcmp(key, "ion_with_hash") == 0)
    {
		if(strcmp(value, "on") == 0)
        	config_param->ion_with_hash = 1;	
		else
			config_param->ion_with_hash = 0;
    }
    else if(strcmp(key, "cache") == 0)
    {
        if(strcmp(value, "on") == 0)
            config_param->use_cache = 1;
        else
            config_param->use_cache = 0;
    }
    else if(strcmp(key, "cache device") == 0)
    {
        if(strcmp(value, "tmpfs") == 0)
            config_param->cache_device_tmpfs = 1;
        else
            config_param->cache_device_tmpfs = 0;
    }
    else if(strcmp(key, "tmpfs path") == 0)
    {
        strcpy(config_param->tmpfs_path, value);
    }
    else if(strcmp(key, "ssd path") == 0)
    {
        strcpy(config_param->ssd_path, value);
    }
    else if(strcmp(key, "ION_NUM_PER_FILE") == 0)
    {
        config_param->ION_NUM_PER_FILE = atoi(value);
    }
    else if(strcmp(key, "BLOCK_NUM_PER_ION_CHUNK") == 0)
    {
        config_param->BLOCK_NUM_PER_ION_CHUNK = atoi(value);
    }
    else if(strcmp(key, "LASIOD") == 0)
    {
        if(strcmp(value, "on") == 0)
            config_param->LASIOD = 1;
        else
            config_param->LASIOD = 0;
    }
    else if(strcmp(key, "LASIOD_SCATTER_NUM") == 0)
    {
        config_param->LASIOD_SCATTER_NUM = atoi(value);
    }
    else if(strcmp(key, "LASIOD_SMALL_IO_SIZE_KB") == 0)
    {
        config_param->LASIOD_SMALL_IO_SIZE = atoi(value) * 1024;
    }
    else if(strcmp(key, "LASIOD_LARGE_IO_SIZE_MB") == 0)
    {
        config_param->LASIOD_LARGE_IO_SIZE = atoi(value) * 1024 * 1024;
    }
    else if(strcmp(key, "SRV_RDMA_EP_NUM") == 0)
    {
        config_param->SRV_RDMA_EP_NUM = atoi(value);
    }
    else if(strcmp(key, "SRV_MEM_BLK_PER_EP") == 0)
    {
        config_param->SRV_MEM_BLK_PER_EP = atoi(value);
    }
    else
    {
        fprintf(stderr, "Config File Error: Unknown Key-Value Pair <%s, %s>!\n", key, value);
        exit(1);
    }
}

void read_config_file(Config_Param *config_param)
{
	memset(config_param, 0, sizeof(Config_Param));
	
    char *path = CONFIG_PARAM_PATH;

    struct confread_file *configFile;
    struct confread_section *thisSect = NULL;
    struct confread_pair *thisPair = NULL;


    if (!(configFile = confread_open(path)))
    {
        fprintf(stderr, "Config open failed\n");
        exit(1);
    }

    thisSect = configFile->sections;
    while(thisSect)
    {
        thisPair = thisSect->pairs;
        while (thisPair)
        {
            digest_config_pair(config_param, thisPair);
            thisPair = thisPair->next;
        }
        thisSect = thisSect->next;
    }

    confread_close(&configFile);

}



