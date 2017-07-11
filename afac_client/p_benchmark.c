#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>

int is_read;
int blk_size_in_kb;
int blk_count;
int file_fd;

void worker_thread(void * id_in)
{
    //pthread_detach(pthread_self());

    int id = *((int*) id_in);

    // init the write out blk
    unsigned long blk_size = 1024L * blk_size_in_kb / sizeof(unsigned long);
    unsigned long tol_size = 1024L * blk_size_in_kb * blk_count;
    lseek(file_fd, tol_size * id, SEEK_SET);

    unsigned long write_out_buf[blk_size];
    int i;
    for(i=0; i<blk_size; i++)
    {
        write_out_buf[i] = i;
    }

    struct timeval start_time, end_time;
    unsigned long read_in_buf[blk_size];
    // read
    int bytes;
    if(is_read == 0)
    {
        gettimeofday(&start_time, NULL);
        for(i=0; i<blk_count; i++)
        {
            bytes = read(file_fd, read_in_buf, blk_size * sizeof(unsigned long));
			if(bytes < 0)
			{
				perror("read error");
				exit(1);
			}
			
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
            bytes = write(file_fd, write_out_buf, blk_size * sizeof(unsigned long));
			if(bytes < 0)
			{
				perror("read error");
				exit(1);
			}
            printf("%llu\n", write_out_buf[0]);
        }
        gettimeofday(&end_time, NULL);
        double dur_usec =  1000000 * ( end_time.tv_sec - start_time.tv_sec ) + end_time.tv_usec - start_time.tv_usec;
        double bandwidth = ((double)blk_size_in_kb) * blk_count / 1024 * 1000000 / dur_usec;
        printf("bandwidth: %0.2f MB/S\n", bandwidth);
    }

}


int main(int argc, char *argv[])
{
    if(argc != 5)
    {
        printf("useage: main num_thread read/write blk_size_in_kb blk_count\n");
        return 0;
    }
    int num_thread = atoi(argv[1]);
    is_read = atoi(argv[2]);
    blk_size_in_kb = atoi(argv[3]);
    blk_count = atoi(argv[4]);

    file_fd = open("/WORK/home/yujie/yujie/afac/afac_client/jay_test_file.dat", O_RDWR);
	if(file_fd <  0)
	{
		perror("open failed:");
		exit(1);
	}


    pthread_t thread_id[num_thread];
    int i;
    for(i=0; i<num_thread; i++)
    {
        int* id_thread = calloc(1, sizeof(int));
        *id_thread = i;
        if(pthread_create(&thread_id[i], NULL, (void *)(&worker_thread), (void *)id_thread) == -1)
        {
            fprintf(stderr,"pthread_create error!\n");
            break;
        }
        printf("create thread %d\n", i);
    }
	
    for(i=0; i<num_thread; i++)
    {
	    pthread_join(thread_id[i], NULL);
    }


	close(file_fd);
}
