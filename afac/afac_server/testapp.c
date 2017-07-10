#include "afac.h"
#include "cache.h"
#include "hash.h"
#include "assoc.h"
#include "file_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main()
{
	global_settings_init();
	printf("global settings init ok\n");
	assoc_init(65535);
	printf("assoc init ok\n");
	slabs_init(global_settings.maxbytes,global_settings.factor,NULL);
	printf("slabs_init ok\n");
	file_table_init();
	printf("file table init ok\n");
	hash_init(hash_type);
	printf("hash init ok\n");
	FILE *fp = fopen("/WORK/home/yujie/antique/experiment/afac/data1","r+");
	int fd = fileno(fp);
	int j = 0;
	int w[4097];
	printf("ok for init...................................\n");
	time_t start_time;
	time(&start_time);
	time_t  end_time;
	
	item_read(fd, 4*4097, sizeof(int)*4097, w);
	printf("time is %ld\n",start_time);

	time(&end_time);
	printf("time is %ld\n",end_time);
	printf("the num is %d\n",w[7]);
	printf("time is %ld\n",(end_time-start_time));
	 time(&start_time);
	 printf("time is %ld\n",start_time);
	item_read(fd, 4*4098, sizeof(int), &j);
	j= 65537;
	item_write(fd, 4*4098,sizeof(int),&j);
	item_read(fd, 4*4098, sizeof(int), &j);
	time(&end_time);
	 printf("time is %ld\n",end_time);
        printf("the num is %d\n",j);
	printf("time is %ld\n",(end_time-start_time));
	time(&start_time);
        item_read(fd, 4*5121, sizeof(int), &j);
        time(&end_time);
        printf("the num is %d\n",j);
        printf("time is %ld\n",(end_time-start_time));
	fclose(fp);
	return 0;

}
