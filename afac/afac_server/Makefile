
OBJS = thomas_jenkins_hash.o radix-tree.o cache.o thread.o hash.o assoc.o file_table.o items.o  afac.o util.o cache_c.o
main: $(OBJS) jenkins_hash.o murmur3_hash.o
	gcc -o afac_server $(OBJS) jenkins_hash.o murmur3_hash.o -lpthread -levent -Wall
jenkins_hash.o:
	gcc -g -c jenkins_hash.c -std=c99 -Wall
murmur3_hash.o:
	gcc -g -c murmur3_hash.c -std=c99 -Wall
$(OBJS):%.o:%.c
	gcc -g -c $< -Wall
clean:
	rm -rf afac_server $(OBJS) *.o
