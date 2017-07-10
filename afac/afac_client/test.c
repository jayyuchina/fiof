#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main()
{
	FILE *fd;
	char re[250];
	fd = fopen("/WORK/home/yujie/antique/experiment/afac/afac_client/string.txt","w+");
	if(fd == NULL)
		return -1;
	char *s = "abcdefg,hijklmnopqresdddddd9";
	printf("the length of string got by strlen is %d\n", strlen(s)+1);
	fwrite(s,sizeof(char),strlen(s)+1,fd);
	char *h = &re[0];
	fseek(fd, 0, SEEK_SET);
	fread(h,sizeof(char),strlen(s)+1,fd);
	printf("the string is %s\n", h);
	return 0;
	
}
