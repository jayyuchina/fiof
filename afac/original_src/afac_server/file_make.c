#include <stdio.h>
#include <stdlib.h>

int main()
{
	int i = 1048576;
	FILE *fp = fopen("/WORK/home/yujie/antique/experiment/afac/data1","a+");
	for(int j=1; j<=i; j++)
	{
		fwrite(&j,sizeof(int),1,fp);
	}
	fseek(fp,-4,SEEK_CUR);
	int k = 0;
	fread(&k,sizeof(int),1,fp);
	printf("%d\n",k);
	fclose(fp);
	return 0;
}
