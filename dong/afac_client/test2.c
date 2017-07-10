#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#define ACCESS_UNIT 1024*1024*16

int main()
{
	int data[ACCESS_UNIT];
	struct timeval start, finish;
	long int access_time;
	int i = 0;
	int check;
	FILE *fd;

	for(i = 0; i < ACCESS_UNIT; i++)
	{
		data[i] = i + 1;
	}

	char host_name[16];
	gethostname(host_name, sizeof(host_name));
	char path[256];
	snprintf(path, sizeof(path), "%s", "/WORK/home/yujie/antique/experiment/afac/afac_client/data");
	strncat(path, ".", 1);
	/* strncat(path, host_name, strlen(host_name));*/
	strncat(path, "public", 6);
	fd = fopen(path,"w+");
	gettimeofday(&start, NULL);
	fwrite(data, sizeof(int), ACCESS_UNIT, fd);
	gettimeofday(&finish, NULL);
	access_time = (finish.tv_sec - start.tv_sec) * 1000000 + finish.tv_usec - start.tv_usec;
	printf("The write time is %ld\n", access_time);

	fseek(fd, 1023*sizeof(int), SEEK_SET);
	gettimeofday(&start, NULL);
	fread(&check, sizeof(int), 1, fd);
	gettimeofday(&finish, NULL);
        access_time = (finish.tv_sec - start.tv_sec) * 1000000 + finish.tv_usec - start.tv_usec;
        printf("The read time is %ld\n", access_time);
	printf("The number is %d\n", check);

	fclose(fd);

}
