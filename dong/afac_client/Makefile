
OBJS = util.o communication.o posix.o test_benchmark.o murmur3_hash.o thomas_jenkins_hash.o jenkins_hash.o hash.o
main: $(OBJS)
	gcc -g -o test_benchmark $(OBJS) -Wl,-wrap,fopen -Wl,-wrap,fseek -Wl,-wrap,fclose -Wl,-wrap,fread -Wl,-wrap,fwrite -Wl,-wrap,close -Wl,-wrap,write -Wl,-wrap,read -Wl,-wrap,open -Wl,-wrap,lseek -Wl,-wrap,lseek64 -Wl,-wrap,open64
util.o:
	gcc -c -g util.c
communication.o:
	gcc -c -g communication.c
jenkins_hash.o:
	gcc -g -c jenkins_hash.c -std=c99
murmur3_hash.o:
	gcc -g -c murmur3_hash.c -std=c99
hash.o: hash.c hash.h
	gcc -g -c hash.c
posix.o:
	gcc -c -g posix.c
test_benchmark.o:
	gcc -c -g test_benchmark.c
clean:
	rm -rf test_benchmark $(OBJS)
