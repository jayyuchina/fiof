#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct name_quota {
char name[16];
int quota;
};

int main()
{
	int server_max = 64;
	struct name_quota *name_quota_c, *name_quota_t;
	name_quota_c = calloc(server_max, sizeof(struct name_quota));
	char *name;
	name = malloc(64 * sizeof(char));
	FILE *fd;
	fd = fopen("/WORK/home/yujie/antique/experiment/afac_client/name_quota", "a+");
	
	int i = 0;
	int file_end = 0;
	char delim[] = " ";
	char *p;
	for(i = 0; (i< server_max) && (!file_end); i++)
	{
		name = fgets(name, 64, fd);
		if(name == NULL) {
			file_end = 1;
			break;
		}
		p = strtok(name, delim);
		memcpy(name_quota_c[i].name, p, strlen(p)+ 1);
		printf("The server name is %s\n", name_quota_c[i].name);
		p = strtok(NULL, delim);
		sscanf(p, "%d", &(name_quota_c[i].quota));
		printf("The server quota is %d\n", name_quota_c[i].quota);
		if((i == server_max - 1) && (!file_end))
		{
			name_quota_t = calloc(server_max * 2, sizeof(struct name_quota));
			if(name_quota_t != NULL)
			{
				memcpy(name_quota_t, name_quota_c, sizeof(struct name_quota)* server_max);
				server_max *= 2;
				free(name_quota_c);
				name_quota_c = name_quota_t;
			}
		}
	}
	fclose(fd);
	printf("%d\n", sizeof(name_quota_c[1].name));
	
	return 0;

}
