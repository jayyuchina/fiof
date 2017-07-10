#ifndef __UTIL_H
#define __UTIL_H

#define TRACE_MEMORY_PER_THREAD 16*1024*1024

struct io_trace {
char file_name[16]; 	/*the last 15 char of file name*/
uint8_t opcode;			/*the opcode of the operation*/
uint64_t offset;		/*the offset of this operation*/
uint64_t length;		/*the length of this operation*/
double exc_time;		/*time cosumed for excutation of this operation*/
time_t request_time;	/*time that request arrived or send by the client*/
int pid;
char client_name[16];
char server_name[8];
};

struct thread_file_trace {
	struct io_trace *file_trace;
	int count;
	int max;
};

bool safe_strtol(const char *str, int32_t *out);


#ifndef HAVE_HTONLL
extern uint64_t htonll(uint64_t);
extern uint64_t ntohll(uint64_t);
#endif

#endif
