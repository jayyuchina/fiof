#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>

#define ACCESS_UNIT 1024*1024

int main(int argc, char *argv[])
{

    int fd = open("/WORK/home/yujie/yujie/afac/afac_client/jay_test_file.dat", O_RDWR);

    sleep(1000000);
}
