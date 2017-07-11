HASHTABLE_FILE = hashtable.h hashtable.c murmur.h murmur.c hashfunc.h
HASHTABLE_OBJ = hashtable.o murmur.o
JAY_FALGS = -D__WITH_MURMUR

JAY_OBJ = afac.o 
DEPEND_OBJ = thomas_jenkins_hash.o radix-tree.o cache.o util.o thread.o hash.o assoc.o file_table.o items.o cache_c.o $(HASHTABLE_OBJ) confread.o 
#OBJS = thomas_jenkins_hash.o radix-tree.o cache.o thread.o hash.o assoc.o file_table.o items.o  afac.o util.o cache_c.o

OPT_GLEX = -L/WORK/home/yujie/yujie/afac/lib -lglex
OPT_SHF = /WORK/home/yujie/yujie/afac/lib/SharedHashFile.a


all: clean main

main: $(DEPEND_OBJ) $(JAY_OBJ) jenkins_hash.o murmur3_hash.o
	gcc -g -o afac_server $(DEPEND_OBJ) $(JAY_OBJ) jenkins_hash.o murmur3_hash.o $(OPT_SHF) $(OPT_GLEX) -lm -lpthread -levent -Wall

jenkins_hash.o:
	gcc -g -c jenkins_hash.c -std=c99 -Wall

murmur3_hash.o:
	gcc -g -c murmur3_hash.c -std=c99 -Wall

afac.o:
	gcc -g -c -D__WITH_MURMUR afac.c -pthread -Wall

.c.o:
	gcc -c -D__WITH_MURMUR -std=c99 $<

clean:
	rm -rf afac_server $(JAY_OBJ)
