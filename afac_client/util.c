#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include "util.h"
#include <string.h>


int getpid_afac()
{
#ifndef MPI_INCLUDED
    return getpid();
#else
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, *rank);
    return rank;
#endif
}

#ifndef HAVE_HTONLL
static uint64_t mc_swap64(uint64_t in)
{
#ifdef ENDIAN_LITTLE
    /* Little endian, flip the bytes around until someone makes a faster/better
    * way to do this. */
    int64_t rv = 0;
    int i = 0;
    for(i = 0; i<8; i++)
    {
        rv = (rv << 8) | (in & 0xff);
        in >>= 8;
    }
    return rv;
#else
    /* big-endian machines don't need byte swapping */
    return in;
#endif
}

uint64_t ntohll(uint64_t val)
{
    return mc_swap64(val);
}

uint64_t htonll(uint64_t val)
{
    return mc_swap64(val);
}
#endif

#define CACHE_UNIT 4 /*GB*/

struct name_quota *name_quota_run;
int server_num = 0;
int server_num_w = 0;
void server_map_init()
{
    int server_max = 64;
    int total_quota = 0;
    struct name_quota *name_quota_t;
    struct name_quota *name_quota_c;
    name_quota_c = calloc(server_max, sizeof(struct name_quota));
    char *name;
    name = malloc(64 * sizeof(char));
    FILE *fd;
    fd = __real_fopen("/WORK/home/yujie/yujie/afac/afac_client/name_quota", "a+");

    int i = 0;
    int j = 0;
    int file_end = 0;
    char delim[] = " ";
    char *p;
    for(i = 0; (i< server_max) && (!file_end); i++)
    {
        name = fgets(name, 64, fd);
        if(name == NULL)
        {
            file_end = 1;
            break;
        }

        p = strtok(name, delim);
        memcpy(name_quota_c[i].name, p, strlen(p)+ 1);
        printf("The server name is %s\n", name_quota_c[i].name);
        p = strtok(NULL, delim);
        sscanf(p, "%d", &(name_quota_c[i].quota));
        printf("The server quota is %d\n", name_quota_c[i].quota);
        total_quota += name_quota_c[i].quota;
        server_num++;

        if((i == server_max - 1) && (!file_end))
        {
            name_quota_t = calloc(server_max * 2, sizeof(struct name_quota));
            if(name_quota_t != NULL)
            {
                memcpy(name_quota_t, name_quota_c, sizeof(struct name_quota)* server_max);
                server_max *= 2;
                free(name_quota_c);
                name_quota_c = name_quota_t;
            }
        }
    }
    __real_fclose(fd);
    server_num_w = total_quota/CACHE_UNIT + ((total_quota % CACHE_UNIT == 0)? 0: 1);
    name_quota_run = calloc(server_num_w + 10, sizeof(struct name_quota));
    int k = 0;
    for(i = 0; i < server_num; i++)
    {
        for(j = 0; j < name_quota_c[i].quota/CACHE_UNIT; j++)
        {
            memcpy(name_quota_run[k].name, name_quota_c[i].name, sizeof(name_quota_c[i].name));
            name_quota_run[k].quota = (j < name_quota_c[i].quota/CACHE_UNIT - 1)?
                                      CACHE_UNIT: (name_quota_c[i].quota % CACHE_UNIT + CACHE_UNIT);
            k++;
        }
    }
    server_num_w = k;

}

