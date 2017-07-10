#include <stdio.h>
int main(void)
{
    int i;
    union endian
    {
        int data;
        char ch;
    }test;

    test.data = 0x12345678;
    if(test.ch == 0x78)
    {
        printf("little endian!\n");
    }
    else
    {
        printf("big endian!\n");
    }
 
    for(i=0; i<4; i++)
    {
        printf("%#x ------- %p\n",*((char *)&test.data + i),(char *)&test.data + i);
    }
    return 0;
}
