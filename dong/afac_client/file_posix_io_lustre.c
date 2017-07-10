# include "mpi.h"
# include <stdio.h>
# define UNIT_SIZE 1024 * 1024    /* 1M */
int main(int argc, char **argv)
{
	int rank, num;
	int size = UNIT_SIZE;
	int a[UNIT_SIZE];
	int b[UNIT_SIZE];
	long offset;
	MPI_Status status;
	FILE * fd = NULL;
	int j=1;
	int i=1;
	double start_time, end_time, bandwidth;
	MPI_Init( &argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size( MPI_COMM_WORLD, &num);
	fd = fopen("/WORK/home/yujie/antique/experiment/b.b", "w");
	if (fd == NULL)
	{ 
		MPI_Abort(MPI_COMM_WORLD, 1);
	}
	MPI_Barrier(MPI_COMM_WORLD);
	
	for(j=1; j<=size; j++)
	{
		a[j-1] = rank * size + j;
	}
	if (rank == 0)
	{ 
		offset = 0;
	}
	else
	{
		offset = rank * size * 4 - 1;
	}
	fprintf(stderr, " The offset of process %d is %ld\n", rank, (offset+1)/size);
	start_time = MPI_Wtime();
	fseek (fd, offset, SEEK_SET);
	fwrite( a, sizeof(int), size, fd);
	/* fprintf(stderr, " The last integer write by process %d is %d\n", rank, a[size-1]); */
	fclose(fd);
	end_time = MPI_Wtime();
	bandwidth = UNIT_SIZE * sizeof(int)/(1048576*(end_time - start_time)); /* 1M/s */
	fprintf (stderr, "The bandwitch get by process %d is %f \n", rank, bandwidth);
	MPI_Barrier(MPI_COMM_WORLD);

	fd = fopen("/WORK/home/yujie/antique/experiment/b.b", "r");
	fseek (fd, offset, SEEK_SET);
	fread(b,4,size,fd);
	for ( i=1; i <= size; i++)
	{
		fprintf(stderr, "the %d number of process %d is %d\n", i, rank, b[i-1]);
	}
	fclose(fd);     
	MPI_Finalize();
	return 0;

}
