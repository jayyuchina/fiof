CC = /usr/local/mpi/bin/mpicc

INCLUDE_DIR = ../include
LIB_DIR = ../lib

HASHTABLE_FILE = hashtable.h hashtable.c murmur.h murmur.c hashfunc.h
HASHTABLE_OBJ = hashtable.o murmur.o
JAY_FALGS = -D__WITH_MURMUR

JAY_OBJ = util.o communication.o posix.o test_benchmark.o
DEPEND_OBJ = murmur3_hash.o thomas_jenkins_hash.o jenkins_hash.o hash.o $(HASHTABLE_OBJ) confread.o

OPT_GLEX = -L/WORK/home/yujie/yujie/afac/lib -lglex
OPT_SHF = /WORK/home/yujie/yujie/afac/lib/SharedHashFile.a


#sample
JAY_LD_GLEX = -L/WORK/home/yujie/yujie/afac 
JAY_GLEX_FLAG = -lglex
JAY_POSIX_A = /WORK/home/yujie/yujie/afac/afac_client/jay_posix.a
JAY_POSIX_FLAG = -Wl,-wrap,fopen -Wl,-wrap,fseek -Wl,-wrap,fclose -Wl,-wrap,fread -Wl,-wrap,fwrite -Wl,-wrap,close -Wl,-wrap,write \
	-Wl,-wrap,read -Wl,-wrap,open -Wl,-wrap,lseek -Wl,-wrap,lseek64 -Wl,-wrap,creat -Wl,-wrap,creat64 -Wl,-wrap,open64 -Wl,-wrap,fstat -Wl,-wrap,ioctl
#sample





#all: clean slib sample
all: clean main

main: $(DEPEND_OBJ) $(JAY_OBJ)  
	$(CC) -g -o test.hack $(JAY_OBJ) $(DEPEND_OBJ) $(OPT_SHF) $(OPT_GLEX) -lpthread -lm -Wl,-wrap,fopen -Wl,-wrap,fseek -Wl,-wrap,fclose -Wl,-wrap,fread -Wl,-wrap,fwrite -Wl,-wrap,close -Wl,-wrap,write -Wl,-wrap,read -Wl,-wrap,open -Wl,-wrap,lseek -Wl,-wrap,lseek64 -Wl,-wrap,open64 -Wl,-wrap,fstat -Wl,-wrap,ioctl




wait: $(DEPEND_OBJ) $(JAY_OBJ) open_then_wait.o 
	$(CC) -g -o open_then_wait.exe util.o communication.o posix.o open_then_wait.o $(DEPEND_OBJ) $(OPT_SHF) $(OPT_GLEX) -lpthread -lm -Wl,-wrap,fopen -Wl,-wrap,fseek -Wl,-wrap,fclose -Wl,-wrap,fread -Wl,-wrap,fwrite -Wl,-wrap,close -Wl,-wrap,write -Wl,-wrap,read -Wl,-wrap,open -Wl,-wrap,lseek -Wl,-wrap,lseek64 -Wl,-wrap,open64 -Wl,-wrap,fstat -Wl,-wrap,ioctl



# make with static lib
sample: test_benchmark.o $(JAY_POSIX_A)
	$(CC) -g -o sample test_benchmark.o $(JAY_POSIX_A) \
	$(JAY_LD_GLEX) $(JAY_POSIX_FLAG) $(JAY_GLEX_FLAG)
	

slib: $(DEPEND_OBJ) $(JAY_OBJ)
	ar -rc jay_posix.a $(DEPEND_OBJ) util.o communication.o posix.o

util.o:
	$(CC) -c -g -std=c99 util.c
communication.o:
	$(CC) -c -g communication.c
jenkins_hash.o:
	$(CC) -g -c jenkins_hash.c -std=c99
murmur3_hash.o:
	$(CC) -g -c murmur3_hash.c -std=c99
hash.o: hash.c hash.h
	$(CC) -g -c hash.c
posix.o:
	$(CC) -c -g -D__WITH_MURMUR posix.c
test_benchmark.o:
	$(CC) -c -g test_benchmark.c

.c.o:
	$(CC) -c -D__WITH_MURMUR -std=c99 $<

clean:
	rm -rf test.hack sample posix.o test_benchmark.o
