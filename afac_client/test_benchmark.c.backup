#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>

#define ACCESS_UNIT 1024*1024
int main(int argc, char *argv[])
{

    if(argc != 4)
    {
        printf("useage: main read/write blk_size_in_kb blk_count\n");
        return 0;
    }
    int read_or_write = atoi(argv[1]);
    double blk_size_in_kb = atof(argv[2]);
    int blk_count = atoi(argv[3]);

    FILE* fp = fopen("/WORK/home/yujie/yujie/afac/afac_client/jay_test_file.dat", "r+");

    // init the write out blk
    unsigned long blk_size = 1024L * blk_size_in_kb / sizeof(unsigned long);
    unsigned long write_out_buf[blk_size];
    int i;
    for(i=0; i<blk_size; i++)
    {
        write_out_buf[i] = i;
    }

	struct timeval start_time, end_time;
    unsigned long read_in_buf[blk_size];
    // read
    if(read_or_write == 0)
    {
		gettimeofday(&start_time, NULL);
        for(i=0; i<blk_count; i++)
        {
			fread(read_in_buf, sizeof(unsigned long), blk_size, fp);
			printf("%llu\n", read_in_buf[0]);
        }
		gettimeofday(&end_time, NULL);
		double dur_usec =  1000000 * ( end_time.tv_sec - start_time.tv_sec ) + end_time.tv_usec - start_time.tv_usec;
		double bandwidth = ((double)blk_size_in_kb) * blk_count / 1024 * 1000000 / dur_usec;
		printf("bandwidth: %0.2f MB/S\n", bandwidth);
    }
    // write
    else
    {
		gettimeofday(&start_time, NULL);
        for(i=0; i<blk_count; i++)
        {
			fwrite(write_out_buf, sizeof(unsigned long), blk_size, fp);
			printf("%llu\n", write_out_buf[0]);
        }
		gettimeofday(&end_time, NULL);
		double dur_usec =  1000000 * ( end_time.tv_sec - start_time.tv_sec ) + end_time.tv_usec - start_time.tv_usec;
		double bandwidth = ((double)blk_size_in_kb) * blk_count / 1024 * 1000000 / dur_usec;
		printf("bandwidth: %0.2f MB/S\n", bandwidth);
    }

	fclose(fp);

}

int main_io(int argc, char *argv[])
{
    if(argc != 4)
    {
        printf("useage: main read/write blk_size_in_kb blk_count\n");
        return 0;
    }
    int read_or_write = atoi(argv[1]);
    double blk_size_in_kb = atof(argv[2]);
    int blk_count = atoi(argv[3]);

    int fd = open("/WORK/home/yujie/yujie/afac/afac_client/jay_test_file.dat", O_RDWR);

    // init the write out blk
    unsigned long blk_size = 1024L * blk_size_in_kb / sizeof(unsigned long);
    unsigned long write_out_buf[blk_size];
    int i;
    for(i=0; i<blk_size; i++)
    {
        write_out_buf[i] = i;
    }

	struct timeval start_time, end_time;
    unsigned long read_in_buf[blk_size];
    // read
    if(read_or_write == 0)
    {
		gettimeofday(&start_time, NULL);
        for(i=0; i<blk_count; i++)
        {
            read(fd, read_in_buf, blk_size * sizeof(unsigned long));
			printf("%llu\n", read_in_buf[0]);
        }
		gettimeofday(&end_time, NULL);
		double dur_usec =  1000000 * ( end_time.tv_sec - start_time.tv_sec ) + end_time.tv_usec - start_time.tv_usec;
		double bandwidth = ((double)blk_size_in_kb) * blk_count / 1024 * 1000000 / dur_usec;
		printf("bandwidth: %0.2f MB/S\n", bandwidth);
    }
    // write
    else
    {
		gettimeofday(&start_time, NULL);
        for(i=0; i<blk_count; i++)
        {
            write(fd, write_out_buf, blk_size * sizeof(unsigned long));
			printf("%llu\n", write_out_buf[0]);
        }
		gettimeofday(&end_time, NULL);
		double dur_usec =  1000000 * ( end_time.tv_sec - start_time.tv_sec ) + end_time.tv_usec - start_time.tv_usec;
		double bandwidth = ((double)blk_size_in_kb) * blk_count / 1024 * 1000000 / dur_usec;
		printf("bandwidth: %0.2f MB/S\n", bandwidth);
    }
}
