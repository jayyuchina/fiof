#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#define ACCESS_UNIT 1024*1024

int main()
{
	int data[ACCESS_UNIT];
	struct timeval start, finish;
	long int access_time;
	int i = 0;
	int check;
	FILE *fd;
	printf("The size of int is %d\n", sizeof(int));

	char host_name[16];
	gethostname(host_name, sizeof(host_name));
	char path[256];
	snprintf(path, sizeof(path), "%s", "/WORK/home/yujie/antique/experiment/afac/afac_client/data");
	strncat(path, ".", 1);
	strncat(path, "public", 6);
	fd = fopen(path,"w+");

	/*first write 4k to file*/
        for(i = 0; i < 1024; i++)
        {
                data[i] = i + 1;
        }
	gettimeofday(&start, NULL);
	fwrite(data, sizeof(int), 1024, fd);
	gettimeofday(&finish, NULL);
	access_time = (finish.tv_sec - start.tv_sec) * 1000000 + finish.tv_usec - start.tv_usec;
	printf("Step1: The write time for the first one page is %ld\n", access_time);

	/*second, write less than one page, leave a hole in this page, write 2k first,
	 then write the last 1k, leave the third 1k, all this data are 2 times of the first write*/
	for(i =0; i < 512; i++)
	{
		data[i] = (i + 1) * 2;
	}

	gettimeofday(&start, NULL);
	fwrite(data, sizeof(int), 512, fd);
	gettimeofday(&finish, NULL);
	access_time = (finish.tv_sec - start.tv_sec) * 1000000 + finish.tv_usec - start.tv_usec;
        printf("Step2: The write time for the first 2k of the first page is %ld\n", access_time);

	for(i =0; i < 256; i++)
        {
                data[i] = (i + 1) * 2;
        }
        
        gettimeofday(&start, NULL);
	fseek(fd, 256*sizeof(int), SEEK_CUR);
        fwrite(data, sizeof(int), 256, fd);
        gettimeofday(&finish, NULL);
        access_time = (finish.tv_sec - start.tv_sec) * 1000000 + finish.tv_usec - start.tv_usec;
        printf("Step2: The write time for the last 1k of the first page is %ld\n", access_time);

	/*Step3: read 3 number from the first page*/
	fseek(fd, 510*sizeof(int), SEEK_SET);
	gettimeofday(&start, NULL);
	fread(&check, sizeof(int), 1, fd);
	gettimeofday(&finish, NULL);
        access_time = (finish.tv_sec - start.tv_sec) * 1000000 + finish.tv_usec - start.tv_usec;
        printf("Step3: The read time for 4 byte is %ld\n", access_time);
	printf("The number is %d\t, and should be %d\n", check, 511*2);

	fseek(fd, 766*sizeof(int), SEEK_SET);
        gettimeofday(&start, NULL);
        fread(&check, sizeof(int), 1, fd);
        gettimeofday(&finish, NULL);
        access_time = (finish.tv_sec - start.tv_sec) * 1000000 + finish.tv_usec - start.tv_usec;
        printf("Step3: The read time for 4 byte is %ld\n", access_time);
        printf("The number is %d\t, and should be %d\n", check, 767);

	fseek(fd, 1022*sizeof(int), SEEK_SET);
        gettimeofday(&start, NULL);
        fread(&check, sizeof(int), 1, fd);
        gettimeofday(&finish, NULL);
        access_time = (finish.tv_sec - start.tv_sec) * 1000000 + finish.tv_usec - start.tv_usec;
        printf("Step3: The read time for 4 byte is %ld\n", access_time);
        printf("The number is %d\t, and should be %d\n", check, 1023*2);

	/*step4: write 1k data stride two page, 
	512 bytes for first page and 512 bytes for second page*/
	for(i = 0; i < 256; i++)
	{
		data[i] = (i + 1)*3;
	}
	
	gettimeofday(&start, NULL);
        fseek(fd, 896*sizeof(int), SEEK_CUR);
        fwrite(data, sizeof(int), 256, fd);
        gettimeofday(&finish, NULL);
        access_time = (finish.tv_sec - start.tv_sec) * 1000000 + finish.tv_usec - start.tv_usec;
        printf("Step4: The write time 1k stride the first page and the second page is %ld\n", access_time);


	fclose(fd);

}
