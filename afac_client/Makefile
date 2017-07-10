
HASHTABLE_FILE = hashtable.h hashtable.c murmur.h murmur.c hashfunc.h
HASHTABLE_OBJ = hashtable.o murmur.o
JAY_FALGS = -D__WITH_MURMUR

JAY_OBJ = util.o communication.o posix.o test_benchmark.o
DEPEND_OBJ = murmur3_hash.o thomas_jenkins_hash.o jenkins_hash.o hash.o $(HASHTABLE_OBJ)

LD_GLEX=-L/WORK/home/yujie/yujie/afac

all: clean main

main: $(DEPEND_OBJ) $(JAY_OBJ)  
	gcc -g -o test_benchmark $(JAY_OBJ) $(DEPEND_OBJ) $(LD_GLEX) -lglex -Wl,-wrap,fopen -Wl,-wrap,fseek -Wl,-wrap,fclose -Wl,-wrap,fread -Wl,-wrap,fwrite -Wl,-wrap,close -Wl,-wrap,write -Wl,-wrap,read -Wl,-wrap,open -Wl,-wrap,lseek -Wl,-wrap,lseek64 -Wl,-wrap,open64

# no fopen
#main: $(DEPEND_OBJ) $(JAY_OBJ)  
#	gcc -g -o test_benchmark $(JAY_OBJ) $(DEPEND_OBJ) $(LD_GLEX) -lglex -Wl,-wrap,fseek -Wl,-wrap,fclose -Wl,-wrap,fread -Wl,-wrap,fwrite -Wl,-wrap,close -Wl,-wrap,write -Wl,-wrap,read -Wl,-wrap,open -Wl,-wrap,lseek -Wl,-wrap,lseek64 -Wl,-wrap,open64


util.o:
	gcc -c -g -std=c99 util.c
communication.o:
	gcc -c -g communication.c
jenkins_hash.o:
	gcc -g -c jenkins_hash.c -std=c99
murmur3_hash.o:
	gcc -g -c murmur3_hash.c -std=c99
hash.o: hash.c hash.h
	gcc -g -c hash.c
posix.o:
	gcc -c -g -D__WITH_MURMUR posix.c
test_benchmark.o:
	gcc -c -g test_benchmark.c

.c.o:
	gcc -c -D__WITH_MURMUR -std=c99 $<

clean:
	rm -rf test_benchmark $(JAY_OBJ)
