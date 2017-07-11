#include <stdlib.h>
#include <stdio.h>

void main()
{
	unsigned long i;
	unsigned long size = 1024L * 1024 * 1024 * 4;		// 400 MB
	//char* path = "/WORK/home/yujie/yujie/afac/afac_client/jay_test_file.dat";
	char* path = "/WORK/home/yujie/yujie/afac/cache_device/tmpfs_device/device_cn7/ram_file";
	FILE* fp = fopen(path, "w");
	if(fp == NULL)
	{
		printf("cann't open the file: %s", path);
		return;
	}

	unsigned long content = 0;
	unsigned long count = size / sizeof(unsigned long);
	for(i=0; i<count; i++)
	{
		fwrite(&content, 1, sizeof(content), fp);
		content++;
	} 

	fclose(fp);
}
