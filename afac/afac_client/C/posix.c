#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <search.h>
#include <assert.h>
#include <libgen.h>
#include <limits.h>
#include <aio.h>
#include <netinet/in.h>
#include <sys/resource.h>
#define __USE_GNU
#include <pthread.h>

#include "communication.h"
#include "protocol_binary.h"
#include "util.h"
#include "hash.h"
#ifndef HAVE_OFF64_T
typedef int64_t off64_t;
#endif
#ifndef HAVE_AIOCB64
#define aiocb64 aiocb
#endif



#ifdef DARSHAN_PRELOAD
#define __USE_GNU
#include <dlfcn.h>
#include <stdlib.h>

#define DARSHAN_FORWARD_DECL(name,ret,args) \
  ret (*__real_ ## name)args = NULL;

#define DARSHAN_DECL(__name) __name

#define DARSHAN_MPI_CALL(func) __real_ ## func

#define MAP_OR_FAIL(func) \
    if (!(__real_ ## func)) \
    { \
        __real_ ## func = dlsym(RTLD_NEXT, #func); \
        if(!(__real_ ## func)) { \
           fprintf(stderr, "Darshan failed to map symbol: %s\n", #func); \
           exit(1); \
       } \
    }

#else

#define DARSHAN_FORWARD_DECL(name,ret,args) \
  extern ret __real_ ## name args;

#define DARSHAN_DECL(__name) __wrap_ ## __name

#define MAP_OR_FAIL(func)

#define DARSHAN_MPI_CALL(func) func

#endif

DARSHAN_FORWARD_DECL(creat, int, (const char* path, mode_t mode));
DARSHAN_FORWARD_DECL(creat64, int, (const char* path, mode_t mode));
DARSHAN_FORWARD_DECL(open, int, (const char *path, int flags, ...));
DARSHAN_FORWARD_DECL(open64, int, (const char *path, int flags, ...));
DARSHAN_FORWARD_DECL(close, int, (int fd));
DARSHAN_FORWARD_DECL(write, ssize_t, (int fd, const void *buf, size_t count));
DARSHAN_FORWARD_DECL(read, ssize_t, (int fd, void *buf, size_t count));
DARSHAN_FORWARD_DECL(lseek, off_t, (int fd, off_t offset, int whence));
DARSHAN_FORWARD_DECL(lseek64, off64_t, (int fd, off64_t offset, int whence));
DARSHAN_FORWARD_DECL(pread, ssize_t, (int fd, void *buf, size_t count, off_t offset));
DARSHAN_FORWARD_DECL(pread64, ssize_t, (int fd, void *buf, size_t count, off64_t offset));
DARSHAN_FORWARD_DECL(pwrite, ssize_t, (int fd, const void *buf, size_t count, off_t offset));
DARSHAN_FORWARD_DECL(pwrite64, ssize_t, (int fd, const void *buf, size_t count, off64_t offset));
DARSHAN_FORWARD_DECL(readv, ssize_t, (int fd, const struct iovec *iov, int iovcnt));
DARSHAN_FORWARD_DECL(writev, ssize_t, (int fd, const struct iovec *iov, int iovcnt));
DARSHAN_FORWARD_DECL(__fxstat, int, (int vers, int fd, struct stat *buf));
DARSHAN_FORWARD_DECL(__fxstat64, int, (int vers, int fd, struct stat64 *buf));
DARSHAN_FORWARD_DECL(__lxstat, int, (int vers, const char* path, struct stat *buf));
DARSHAN_FORWARD_DECL(__lxstat64, int, (int vers, const char* path, struct stat64 *buf));
DARSHAN_FORWARD_DECL(__xstat, int, (int vers, const char* path, struct stat *buf));
DARSHAN_FORWARD_DECL(__xstat64, int, (int vers, const char* path, struct stat64 *buf));
DARSHAN_FORWARD_DECL(mmap, void*, (void *addr, size_t length, int prot, int flags, int fd, off_t offset));
DARSHAN_FORWARD_DECL(mmap64, void*, (void *addr, size_t length, int prot, int flags, int fd, off64_t offset));
DARSHAN_FORWARD_DECL(fopen, FILE*,  (const char *path, const char *mode));
DARSHAN_FORWARD_DECL(fopen64, FILE*, (const char *path, const char *mode));
DARSHAN_FORWARD_DECL(fclose, int, (FILE *fp));
DARSHAN_FORWARD_DECL(fread, size_t, (void *ptr, size_t size, size_t nmemb, FILE *stream));
DARSHAN_FORWARD_DECL(fwrite, size_t, (const void *ptr, size_t size, size_t nmemb, FILE *stream));
DARSHAN_FORWARD_DECL(fseek, int, (FILE *stream, long offset, int whence));
DARSHAN_FORWARD_DECL(fsync, int, (int fd));
DARSHAN_FORWARD_DECL(fdatasync, int, (int fd));
DARSHAN_FORWARD_DECL(aio_read, int, (struct aiocb *aiocbp));
DARSHAN_FORWARD_DECL(aio_read64, int, (struct aiocb64 *aiocbp));
DARSHAN_FORWARD_DECL(aio_write, int, (struct aiocb *aiocbp));
DARSHAN_FORWARD_DECL(aio_write64, int, (struct aiocb64 *aiocbp));
DARSHAN_FORWARD_DECL(lio_listio, int, (int mode, struct aiocb *const aiocb_list[], int nitems, struct sigevent *sevp));
DARSHAN_FORWARD_DECL(lio_listio64, int, (int mode, struct aiocb64 *const aiocb_list[], int nitems, struct sigevent *sevp));
DARSHAN_FORWARD_DECL(aio_return, ssize_t, (struct aiocb *aiocbp));
DARSHAN_FORWARD_DECL(aio_return64, ssize_t, (struct aiocb64 *aiocbp));

extern int server_num_w;
extern struct name_quota *name_quota_run;
enum hashfunc_type hash_type = JENKINS_HASH;

/* these are paths that we will not trace */
static char* exclusions[] = {
"/etc/",
"/dev/",
"/usr/",
"/bin/",
"/boot/",
"/lib/",
"/opt/",
"/sbin/",
"/sys/",
"/proc/",
NULL
};
/*this is the path we only intercept*/
static char* inclusions[] = {
"/vol-th/",
"/vol-6/",
"/WORK/",
NULL
};

typedef struct {
	int sfd;
	char server_name[8];
	char file_name[250]; 
	uint64_t file_offset;
} fd_map;

static fd_map *fd_map_array;

pthread_mutex_t cp_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#define CP_LOCK() pthread_mutex_lock(&cp_mutex)
#define CP_UNLOCK() pthread_mutex_unlock(&cp_mutex)

static int init_count = 0;

int main_init()
{
	if(!init_count) {
		CP_LOCK();
		if(!init_count) {
			init_count++;
			if((intercept_init() != 0) || (conn_cli_init(0) != 0))
				return -1;
		}
		server_map_init();
		hash_init(hash_type);
		CP_UNLOCK();
	}
	return 0;
}


int intercept_init(void) {
    /* We're unlikely to see an FD much higher than maxconns. */
    int next_fd = dup(1);
    int headroom = 10;      /* account for extra unexpected open FDs */
    struct rlimit rl;

    int max_fds = 1024 + headroom + next_fd;

    /* But if possible, get the actual highest FD we can possibly ever see. */
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        max_fds = rl.rlim_max;
    } else {
        fprintf(stderr, "Failed to query maximum file descriptor; "
				"falling back to maxconns\n");
    }
    MAP_OR_FAIL(close);
    __real_close(next_fd);

    if ((fd_map_array = calloc(max_fds, sizeof(fd_map))) == NULL) {
        fprintf(stderr, "Failed to allocate connection structures\n");
        /* This is unrecoverable so bail out early. */
        return -1;
    }
	return 0;
}



int DARSHAN_DECL(close)(int fd)
{
	int ret;
	int sfd;
	int connected;
	long curr_offset;
	conn_cli* c;

	MAP_OR_FAIL(close);
	if(fd_map_array == NULL)
	{
		return __real_close(fd);
	}
	/*test the connection already existed or not*/
	struct sockaddr server_addr;
	if(fd_map_array[fd].sfd == 0)
		return __real_close(fd);
	connected = connection_close_exam(fd_map_array[fd].sfd, &server_addr);
	
	int request_len = sizeof(fd);
	int message_len = request_len + sizeof(protocol_binary_request_header);
	
	if(connected != 0)
	{
		c = cli_socket(fd_map_array[fd].server_name, server_port, tcp_transport);
		if(c == NULL) {
			return __real_close(fd);
		}
	}
	if(connected == 0) /*just do not consider the ip address equal or not*/
	{
		/*send data to server*/
		c = conn_cli_new(fd_map_array[fd].sfd, 0, tcp_transport);
		if(c == NULL) {
			return -1;
		}	
	}
	
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: fclose, file: %d\n", fd);
			return -1;
		}
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	c->wbytes += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_CLOSE;
	req->request.isfrag = 1; /*do not package any more*/
	req->request.frag_id = 1; /*beginning from 1*/
	req->request.request_id = 0;
	req->request.para_num = 1;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)(sizeof(fd)));
	req->request.para2_len = htonll((uint64_t)0);
	req->request.para3_len = htonll((uint64_t)0);
	req->request.para4_len = htonll((uint64_t)0);
	req->request.para5_len = htonll((uint64_t)0);
	req->request.para6_len = 0;
	
	
	/*copy the parameters of the fread request to the message body*/		
	memcpy(c->wcurr, &fd, sizeof(fd));
	c->wcurr = c->wcurr + sizeof(fd);
	c->wbytes += sizeof(fd);
	
	
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;
	MAP_OR_FAIL(write);
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			c->wbytes -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return -1;
		}
		if(res == 0) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return -1;
		}
	}

	int i;
	/*fourth: wait for response, check integrity of the package */
	i = cli_try_read_command(c);
	if(i != 0) {
		return -1;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_CLOSE))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_CLOSE))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->assemb_buf + sizeof(protocol_binary_response_header), resp->response.para1_len);
	}
	
	__real_close(c->sfd);
	memset((void* )&fd_map_array[fd], 0, sizeof(fd_map));

    return(ret);
}

int DARSHAN_DECL(fclose)(FILE *fp)
{
	int fd;
	int sfd;
	int connected;
	long curr_offset;
	conn_cli* c;
	int ret;
	if(!fp)
		return 0;
	fd = fileno(fp);
	fprintf(stderr, "The fclose operation of file %d\n", fd);
	MAP_OR_FAIL(close);
	
	/*test the connection already existed or not*/
	struct sockaddr server_addr;
	connected = connection_close_exam(fd_map_array[fd].sfd, &server_addr);
	
	int request_len = sizeof(*fp);
	int message_len = request_len + sizeof(protocol_binary_request_header);
	
	
	MAP_OR_FAIL(fclose);
	
	if(connected != 0)
	{
		c = cli_socket(fd_map_array[fd].server_name, server_port, tcp_transport);
		if(c == NULL) {
			return __real_fclose(fp);
		}
	}
	if(connected == 0) /*just do not consider the ip address equal or not*/
	{
		/*send data to server*/
		c = conn_cli_new(fd_map_array[fd].sfd, 0, tcp_transport);
		if(c == NULL) {
			return -1;
		}	
	}
	
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: fclose, file: %d\n", fd);
			return -1;
		}
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	c->wbytes += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_FCLOSE;
	req->request.isfrag = 1; /*do not package any more*/
	req->request.frag_id = 1; /*beginning from 1*/
	req->request.request_id = 0;
	req->request.para_num = 1;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)(sizeof(*fp)));
	req->request.para2_len = htonll((uint64_t)0);
	req->request.para3_len = htonll((uint64_t)0);
	req->request.para4_len = htonll((uint64_t)0);
	req->request.para5_len = htonll((uint64_t)0);
	req->request.para6_len = 0;
	
	
	/*copy the parameters of the fread request to the message body*/		
	memcpy(c->wcurr, fp, sizeof(*fp));
	c->wcurr = c->wcurr + sizeof(*fp);
	c->wbytes += sizeof(*fp);
	
	
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;
	MAP_OR_FAIL(write);
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			c->wbytes -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return 0;
		}
		if(res == 0) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return 0;
		}
	}
	fprintf(stderr, "send data finished. there are %d bytes data left in the buffer\n", c->wbytes);
	int i;
	/*fourth: wait for response, check integrity of the package */
	i = cli_try_read_command(c);
	if(i != 0) {
		return -1;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_FCLOSE))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_FCLOSE))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->assemb_buf + sizeof(protocol_binary_response_header), resp->response.para1_len);
	}

	__real_close(c->sfd);
	memset((void* )&fd_map_array[fd], 0, sizeof(fd_map));
    return(ret);
}
	



int DARSHAN_DECL(fsync)(int fd)
{
    int ret;
 

    return(ret);
}

int DARSHAN_DECL(fdatasync)(int fd)
{
    int ret;
 

    return(ret);
}


void* DARSHAN_DECL(mmap64)(void *addr, size_t length, int prot, int flags,
    int fd, off64_t offset)
{
    void* ret;
    

    return(ret);
}


void* DARSHAN_DECL(mmap)(void *addr, size_t length, int prot, int flags,
    int fd, off_t offset)
{
    void* ret;
    
    return(ret);
}

int DARSHAN_DECL(creat)(const char* path, mode_t mode)
{
    int ret;
    double tm1, tm2;



    return(ret);
}

int DARSHAN_DECL(creat64)(const char* path, mode_t mode)
{
    int ret;

    return(ret);
}

int DARSHAN_DECL(open64)(const char* path, int flags, ...)
{
	int mode = 0;
 	int ret;
	int para_num;
	char absolute_path[256] = {0};
	
	if(main_init() == -1)
	{
		return -1;
	}
	
	if (flags & O_CREAT)
	{
		va_list arg;
        	va_start(arg, flags);
       		mode = va_arg(arg, int);
        	va_end(arg);
		para_num = 3;
	} else
	{
		para_num = 2;
	}
	if(path[0] != '/')
	{
		getcwd(absolute_path, sizeof(absolute_path));
		strncat(absolute_path, "/", 1);
		strncat(absolute_path, path, strlen(path));
	} else {
		snprintf(absolute_path, strlen(path) + 1, "%s", path);
	}
	
	conn_cli* c;
	int i;
	char *s;
	
	MAP_OR_FAIL(open64);
	
	/*if the file path is in the exclusions, just excute the normal opreation*/
	for(i = 0; (s = inclusions[i]) != NULL; i++)
	{
		if(!(strncmp(s, absolute_path, strlen(s))))
			break;
	}
	if(!s) {
		if(para_num == 2)
			return __real_open64(absolute_path, flags);
		else 
			return __real_open64(absolute_path, flags, mode);
	}
	
	uint64_t hv = hash(absolute_path, strlen(absolute_path) + 1);
	hv = hv % server_num_w;
	/*first: according the file name, choose a file server, we need our hash algorithm*/
	char * server_name = name_quota_run[hv].name;
	
	/*second: new a connection, TCP or UDP*/
	c = cli_socket(server_name, server_port, tcp_transport);
	
	if(c == NULL) {
		fprintf(stderr,"cli_socket for open failure because the return value is null\n");
		return -1;
	}
	
	/*third: send data to server*/
	int request_len = strlen(absolute_path) + 1 + sizeof(int);
	if(para_num == 3)
		request_len += sizeof(int);
	
	int message_len = request_len + sizeof(protocol_binary_request_header);
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: open, file: %s\n", absolute_path);
			return -1;
		}
	}
	if(c->wcurr != c->wbuf)
	{
		memmove(c->wbuf, c->wcurr, c->wbytes);
		c->wcurr = c->wbuf;
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_OPEN64;
	req->request.isfrag = 1; /*fopen is usually smaller than a package*/
	req->request.frag_id = 1;
	req->request.request_id = 0;
	req->request.para_num = para_num;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)(strlen(absolute_path) + 1));
	req->request.para2_len = htonll((uint64_t)(sizeof(int)));
	req->request.para3_len = 0;
	if(para_num == 3)
		req->request.para3_len = htonll((uint64_t)(sizeof(int)));
	req->request.para4_len = 0;
	req->request.para5_len = 0;
	req->request.para6_len = 0;
	
	/*copy the parameters of the fopen request to the message body*/
	
	
	memcpy(c->wcurr, absolute_path, strlen(absolute_path) + 1);
	c->wcurr = c->wcurr + strlen(absolute_path) + 1;
	memcpy(c->wcurr, &flags, sizeof(int));
	c->wcurr = c->wcurr + sizeof(int);
	if(para_num == 3)
	{
		memcpy(c->wcurr, &mode, sizeof(int));
		c->wcurr = c->wcurr + sizeof(int);
	}
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;
	MAP_OR_FAIL(write);
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return -1;
		}
		if(res == -1) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return -1;
		}
	}
	
	/*fourth: wait for response, check integrity of the package */
	i = cli_try_read_command(c);
	if(i != 0) {
		return -1;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_OPEN64))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		ret = *(int *)((char*)c->rbuf + sizeof(protocol_binary_response_header));
		
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_OPEN64))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->assemb_buf + sizeof(protocol_binary_response_header), resp->response.para1_len);
	}
	snprintf(fd_map_array[ret].server_name, sizeof(fd_map_array[ret].server_name), server_name);
	snprintf(fd_map_array[ret].file_name,sizeof(fd_map_array[ret].file_name), absolute_path);
	fd_map_array[ret].sfd = c->sfd;
	fd_map_array[ret].file_offset = 0;
	/*close the connection*/
		
    return(ret);
}

int DARSHAN_DECL(open)(const char *path, int flags, ...)
{
    int mode = 0;
    int ret;
	int para_num;
	
	if(main_init() == -1)
	{
		return -1;
	}
	
	if (flags & O_CREAT)
	{
		va_list arg;
        va_start(arg, flags);
        mode = va_arg(arg, int);
        va_end(arg);
		para_num = 3;
	} else
	{
		para_num = 2;
	}
	

	conn_cli* c;
	int i;
	char *s;
	
	MAP_OR_FAIL(open);
	
	/*if the file path is in the exclusions, just excute the normal opreation*/
	for(i = 0; (s = inclusions[i]) != NULL; i++)
	{
		if(!(strncmp(s, path, strlen(s))))
			break;
	}
	if(!s) {
		if(para_num == 2)
			return __real_open(path, flags);
		else 
			return __real_open(path, flags, mode);
	}
	
	uint64_t hv = hash(path, strlen(path) + 1);
	printf("The hash value is %d\n", hv);
	hv = hv % server_num_w;
	printf("The hash server is %d\n", hv);
	/*first: according the file name, choose a file server, we need our hash algorithm*/
	char * server_name = name_quota_run[hv].name;
	
	/*second: new a connection, TCP or UDP*/
	c = cli_socket(server_name, server_port, tcp_transport);
	
	if(c == NULL) {
		fprintf(stderr,"cli_socket for open failure because the return value is null\n");
		return -1;
	}
	
	/*third: send data to server*/
	int request_len = strlen(path) + 1 + sizeof(int);
	if(para_num == 3)
		request_len += sizeof(int);
	
	int message_len = request_len + sizeof(protocol_binary_request_header);
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: open, file: %s\n", path);
			return -1;
		}
	}
	if(c->wcurr != c->wbuf)
	{
		memmove(c->wbuf, c->wcurr, c->wbytes);
		c->wcurr = c->wbuf;
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_OPEN;
	req->request.isfrag = 1; /*fopen is usually smaller than a package*/
	req->request.frag_id = 1;
	req->request.request_id = 0;
	req->request.para_num = para_num;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)(strlen(path) + 1));
	req->request.para2_len = htonll((uint64_t)(sizeof(int)));
	req->request.para3_len = 0;
	if(para_num == 3)
		req->request.para3_len = htonll((uint64_t)(sizeof(int)));
	req->request.para4_len = 0;
	req->request.para5_len = 0;
	req->request.para6_len = 0;
	
	/*copy the parameters of the fopen request to the message body*/
	
	
	memcpy(c->wcurr, path, strlen(path) + 1);
	c->wcurr = c->wcurr + strlen(path) + 1;
	memcpy(c->wcurr, &flags, sizeof(int));
	c->wcurr = c->wcurr + sizeof(int);
	if(para_num == 3)
	{
		memcpy(c->wcurr, &mode, sizeof(int));
		c->wcurr = c->wcurr + sizeof(int);
	}
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;
	MAP_OR_FAIL(write);
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return -1;
		}
		if(res == -1) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return -1;
		}
	}
	
	/*fourth: wait for response, check integrity of the package */
	i = cli_try_read_command(c);
	if(i != 0) {
		return -1;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_OPEN))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_OPEN))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->assemb_buf + sizeof(protocol_binary_response_header), resp->response.para1_len);
	}
	snprintf(fd_map_array[ret].server_name, sizeof(fd_map_array[ret].server_name), server_name);
	snprintf(fd_map_array[ret].file_name,sizeof(fd_map_array[ret].file_name), path);
	fd_map_array[ret].sfd = c->sfd;
	fd_map_array[ret].file_offset = 0;
	/*close the connection*/
	

    return(ret);
}

FILE* DARSHAN_DECL(fopen64)(const char *path, const char *mode)
{
    FILE* ret;


    return(ret);
}

FILE* DARSHAN_DECL(fopen)(const char *path, const char *mode)
{
    if(main_init() == -1)
	{
		return NULL;
	}
	
	FILE* ret = calloc(1, sizeof(FILE));
	if(ret == NULL)
	{
		return ret;
	}
	conn_cli* c;
	int i;
	char *s;
	
	MAP_OR_FAIL(fopen);
	
	/*if the file path is in the exclusions, just excute the normal opreation*/
	for(i = 0; (s = inclusions[i]) != NULL; i++)
	{
		if(!(strncmp(s, path, strlen(s))))
			break;
	}
	if(!s) {
		return __real_fopen(path, mode);
	}
	
	uint64_t hv = hash(path, strlen(path) + 1);
	printf("The hash value is %d\n", hv);
	hv = hv % server_num_w;
	printf("The hash server is %d\n", hv);
	/*first: according the file name, choose a file server, we need our hash algorithm*/
	char * server_name = name_quota_run[hv].name;
	
	/*second: new a connection, TCP or UDP*/
	c = cli_socket(server_name, server_port, tcp_transport);
	
	if(c == NULL) {
		fprintf(stderr,"cli_socket for fopen failure because the return value is null\n");
		return NULL;
	}
	
	/*third: send data to server*/
	int request_len = strlen(path) + strlen(mode) + 2;
	int message_len = request_len + sizeof(protocol_binary_request_header);
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: fopen, file: %s\n", path);
			return NULL;
		}
	}
	if(c->wcurr != c->wbuf)
	{
		memmove(c->wbuf, c->wcurr, c->wbytes);
		c->wcurr = c->wbuf;
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_FOPEN;
	req->request.isfrag = 1; /*fopen is usually smaller than a package*/
	req->request.frag_id = 1;
	req->request.request_id = 0;
	req->request.para_num = 2;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)(strlen(path) + 1));
	req->request.para2_len = htonll((uint64_t)(strlen(mode) + 1));
	req->request.para3_len = 0;
	req->request.para4_len = 0;
	req->request.para5_len = 0;
	req->request.para6_len = 0;
	
	/*copy the parameters of the fopen request to the message body*/
	
	
	memcpy(c->wcurr, path, strlen(path) + 1);
	c->wcurr = c->wcurr + strlen(path) + 1;
	memcpy(c->wcurr, mode, strlen(mode) + 1);
	c->wcurr = c->wcurr + strlen(mode) + 1;
	
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;
	MAP_OR_FAIL(write);
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return NULL;
		}
		if(res == -1) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return NULL;
		}
	}
	
	/*fourth: wait for response, check integrity of the package */
	i = cli_try_read_command(c);
	if(i != 0) {
		return NULL;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	printf("c->frag_num is %d\n",c->frag_num);
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_FOPEN))
		{
			free(ret);
			return NULL;
		}
		memcpy(ret, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_FOPEN))
		{
			free(ret);
			return NULL;
		}
		memcpy(ret, (char*)c->assemb_buf + sizeof(protocol_binary_response_header), resp->response.para1_len);
	}
	snprintf(fd_map_array[fileno(ret)].server_name, sizeof(fd_map_array[fileno(ret)].server_name), server_name);
	snprintf(fd_map_array[fileno(ret)].file_name,sizeof(fd_map_array[fileno(ret)].file_name), path);
	fd_map_array[fileno(ret)].sfd = c->sfd;
	fd_map_array[fileno(ret)].file_offset = 0;
	/*close the connection*/
	
    return(ret);
}

int DARSHAN_DECL(__xstat64)(int vers, const char *path, struct stat64 *buf)
{
    int ret;
   

    return(ret);
}

int DARSHAN_DECL(__lxstat64)(int vers, const char *path, struct stat64 *buf)
{
    int ret;


    return(ret);
}

int DARSHAN_DECL(__fxstat64)(int vers, int fd, struct stat64 *buf)
{
    int ret;
   
    return(ret);
}


int DARSHAN_DECL(__xstat)(int vers, const char *path, struct stat *buf)
{
    int ret;

    return(ret);
}

int DARSHAN_DECL(__lxstat)(int vers, const char *path, struct stat *buf)
{
    int ret;

    return(ret);
}

int DARSHAN_DECL(__fxstat)(int vers, int fd, struct stat *buf)
{
    int ret;



    return(ret);
}

ssize_t DARSHAN_DECL(pread64)(int fd, void *buf, size_t count, off64_t offset)
{
    ssize_t ret;

    return(ret);
}

ssize_t DARSHAN_DECL(pread)(int fd, void *buf, size_t count, off_t offset)
{
    ssize_t ret;

    return(ret);
}


ssize_t DARSHAN_DECL(pwrite)(int fd, const void *buf, size_t count, off_t offset)
{
    ssize_t ret;

    return(ret);
}

ssize_t DARSHAN_DECL(pwrite64)(int fd, const void *buf, size_t count, off64_t offset)
{
    ssize_t ret;

    return(ret);
}

ssize_t DARSHAN_DECL(readv)(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t ret;

    return(ret);
}

ssize_t DARSHAN_DECL(writev)(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t ret;

    return(ret);
}

size_t DARSHAN_DECL(fread)(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t ret = 0;
	int fd;
	int sfd;
	int connected;
	long curr_offset;
	conn_cli* c;
	int k = 0;
	char *s;

	if(!stream)
		return 0;
	fd = fileno(stream);
	if((fd == 0) || (fd == 1) || (fd == 2))
        {
                MAP_OR_FAIL(fread);
                return __real_fread(ptr, size, nmemb, stream);
        }	

	/*if the file path is in the exclusions, just excute the normal opreation*/
	for(k = 0; (s = inclusions[k]) != NULL; k++)
	{
		if(!(strncmp(s, fd_map_array[fd].file_name, strlen(s))))
			break;
	}
	if(!s) {
		MAP_OR_FAIL(fread);
		return __real_fread(ptr, size, nmemb, stream);
	}

	/*we do not intercept ftell, so do not to mapping ftell*/
	curr_offset = fd_map_array[fd].file_offset;
	
	/*test the connection already existed or not*/
	struct sockaddr server_addr;
	connected = connection_close_exam(fd_map_array[fd].sfd, &server_addr);
	
	int request_len = sizeof(size) + sizeof(nmemb) + sizeof(*stream) + sizeof(curr_offset);
	int message_len = request_len + sizeof(protocol_binary_request_header);
	int package_num;
	int package_id;
	int package_offset;
	int package_body_len;
		
	if(connected != 0)
	{
		c = cli_socket(fd_map_array[fd].server_name, server_port, tcp_transport);
		if(c == NULL) {
			fprintf(stderr, "cli_socket failed!\n");
			return 0;
		}
	}
	if(connected == 0) /*just do not consider the ip address equal or not*/
	{
		/*send data to server*/
		c = conn_cli_new(fd_map_array[fd].sfd, 0, tcp_transport);
		if(c == NULL) {
			fprintf(stderr, "conn_cli_new failed!\n");
			return 0;
		}	
	}
	
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: fread, file: %d\n", fd);
			return 0;
		}
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	c->wbytes += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_FREAD;
	req->request.isfrag = 1; /*do not package any more*/
	req->request.frag_id = 1; /*beginning from 1*/
	req->request.request_id = 0;
	req->request.para_num = 4;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)(sizeof(size)));
	req->request.para2_len = htonll((uint64_t)(sizeof(nmemb)));
	req->request.para3_len = htonll((uint64_t)(sizeof(*stream)));
	req->request.para4_len = htonll((uint64_t)(sizeof(curr_offset)));
	req->request.para5_len = 0;
	req->request.para6_len = 0;
	
	
	/*copy the parameters of the fread request to the message body*/		
	memcpy(c->wcurr, &size, sizeof(size));
	c->wcurr = c->wcurr + sizeof(size);
	c->wbytes += sizeof(size);
	memcpy(c->wcurr, &nmemb, sizeof(nmemb));
	c->wcurr = c->wcurr + sizeof(nmemb);
	c->wbytes += sizeof(nmemb);
	memcpy(c->wcurr, stream, sizeof(*stream));
	c->wcurr = c->wcurr + sizeof(*stream);
	c->wbytes += sizeof(*stream);
	memcpy(c->wcurr, &curr_offset, sizeof(curr_offset));
	c->wcurr = c->wcurr + sizeof(curr_offset);
	c->wbytes += sizeof(curr_offset);
	
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;

	MAP_OR_FAIL(write);
	
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			c->wbytes -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return 0;
		}
		if(res == 0) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return 0;
		}
	}

	int i;
	/*fourth: wait for response, check integrity of the package */
	i = cli_try_read_command(c);
	if(i != 0) {
		fprintf(stderr, "cli_try_read_command failed\n");
		return 0;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_FREAD))
		{
			fprintf(stderr, "Message failure, magic or opcode is wrong\n");
			return 0;
		}
		memcpy(ptr, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header) + resp->response.para1_len, resp->response.para2_len);
		fd_map_array[fd].file_offset += ret;
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_FREAD))
		{
			fprintf(stderr, "Message failure, magic or opcode is wrong\n");
			return 0;
		}
		memcpy(ptr, (char*)c->assemb_buf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		memcpy(&ret, c->rbuf + sizeof(protocol_binary_response_header) + resp->response.para1_len, resp->response.para2_len);
		fd_map_array[fd].file_offset += ret;
	}

    return(ret);
}

ssize_t DARSHAN_DECL(read)(int fd, void *buf, size_t count)
{
	size_t ret = 0;
	int sfd;
	int connected;
	long curr_offset;
	conn_cli* c;
	int k = 0;
	char *s;
	

	
	/*if the file path is in the exclusions, just excute the normal opreation*/
	for(k = 0; (s = inclusions[k]) != NULL; k++)
	{
		if(!(strncmp(s, fd_map_array[fd].file_name, strlen(s))))
			break;
	}
	if(!s) {
		MAP_OR_FAIL(read);
		return __real_read(fd, buf, count);
	}
	
	/*we do not intercept ftell, so do not to mapping ftell*/
	curr_offset = fd_map_array[fd].file_offset;
	
	/*test the connection already existed or not*/
	struct sockaddr server_addr;
	connected = connection_close_exam(fd_map_array[fd].sfd, &server_addr);
	
	int request_len = sizeof(fd) + sizeof(count) + sizeof(curr_offset);
	int message_len = request_len + sizeof(protocol_binary_request_header);
	int package_num;
	int package_id;
	int package_offset;
	int package_body_len;
	
	if(connected != 0)
	{
		c = cli_socket(fd_map_array[fd].server_name, server_port, tcp_transport);
		if(c == NULL) {
			fprintf(stderr, "cli_socket failed!\n");
			return 0;
		}
	}
	if(connected == 0) /*just do not consider the ip address equal or not*/
	{
		/*send data to server*/
		c = conn_cli_new(fd_map_array[fd].sfd, 0, tcp_transport);
		if(c == NULL) {
			fprintf(stderr, "conn_cli_new failed!\n");
			return 0;
		}	
	}
	
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: fread, file: %d\n", fd);
			return 0;
		}
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	c->wbytes += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_READ;
	req->request.isfrag = 1; /*do not package any more*/
	req->request.frag_id = 1; /*beginning from 1*/
	req->request.request_id = 0;
	req->request.para_num = 3;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)(sizeof(fd)));
	req->request.para2_len = htonll((uint64_t)(sizeof(count)));
	req->request.para3_len = htonll((uint64_t)(sizeof(curr_offset)));
	req->request.para4_len = 0;
	req->request.para5_len = 0;
	req->request.para6_len = 0;
	
	
	/*copy the parameters of the fread request to the message body*/		
	memcpy(c->wcurr, &fd, sizeof(fd));
	c->wcurr = c->wcurr + sizeof(fd);
	c->wbytes += sizeof(fd);
	memcpy(c->wcurr, &count, sizeof(count));
	c->wcurr = c->wcurr + sizeof(count);
	c->wbytes += sizeof(count);
	memcpy(c->wcurr, &curr_offset, sizeof(curr_offset));
	c->wcurr = c->wcurr + sizeof(curr_offset);
	c->wbytes += sizeof(curr_offset);
	
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;
	
	MAP_OR_FAIL(write);
	
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			c->wbytes -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return 0;
		}
		if(res == 0) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return 0;
		}
	}
	
	int i;
	/*fourth: wait for response, check integrity of the package */
	if(c->rsize < (sizeof(protocol_binary_response_header) + count + 16))
	{
		c->rbuf = realloc(c->rbuf, sizeof(protocol_binary_response_header) + count + 16);
		c->rsize = sizeof(protocol_binary_response_header) + count + 16;
		c->rcurr = c->rbuf;
	}
	i = cli_try_read_command(c);
	if(i != 0) {
		fprintf(stderr, "cli_try_read_command failed\n");
		return 0;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_READ))
		{
			fprintf(stderr, "Message failure, magic or opcode is wrong\n");
			return 0;
		}
		memcpy(buf, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header) + resp->response.para1_len, resp->response.para2_len);
		fd_map_array[fd].file_offset += ret;
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_FREAD))
		{
			fprintf(stderr, "Message failure, magic or opcode is wrong\n");
			return 0;
		}
		memcpy(buf, (char*)c->assemb_buf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		memcpy(&ret, c->rbuf + sizeof(protocol_binary_response_header) + resp->response.para1_len, resp->response.para2_len);
		fd_map_array[fd].file_offset += ret;
	}
	
    return(ret);
}

ssize_t DARSHAN_DECL(write)(int fd, const void *buf, size_t count)
{
	size_t ret = 0;

	int sfd;
	int connected;
	int process_pid;
	long curr_offset;
	conn_cli* c;
	int path_look = 0;
	char *s;	
	
	for(path_look = 0; (s = inclusions[path_look]) != NULL; path_look++)
	{
		if(!(strncmp(s, fd_map_array[fd].file_name, strlen(s))))
			break;
	}
	if(!s) {
		MAP_OR_FAIL(write);
		return __real_write(fd, buf, count);
	}
	
	
	
	curr_offset = fd_map_array[fd].file_offset;
	process_pid = getpid_afac();
	/*test the connection already existed or not*/
	struct sockaddr server_addr;
	connected = connection_close_exam(fd_map_array[fd].sfd, &server_addr);
	
	int request_len = sizeof(fd) + sizeof(count) + count + sizeof(curr_offset) + sizeof(process_pid);
	int message_len = request_len + sizeof(protocol_binary_request_header);
	int package_num;
	int package_id;
	int package_offset;
	int package_body_len;
	
	if(connected != 0)
	{
		c = cli_socket(&(fd_map_array[fd].server_name[0]), server_port, tcp_transport);
		if(c == NULL) {
			fprintf(stderr, "cli_socket failed\n");
			return 0;
		}
	}
	if(connected == 0) /*just do not consider the ip address equal or not*/
	{
		/*send data to server*/
		c = conn_cli_new(fd_map_array[fd].sfd, 0, tcp_transport);
		if(c == NULL) {
			fprintf(stderr, "conn_cli_new failed\n");
			return 0;
		}	
	}
	
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: write, file: %d\n", fd);
			return 0;
		}
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	c->wbytes += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_WRITE;
	req->request.isfrag = 1; /*do not package any more*/
	req->request.frag_id = 1; /*beginning from 1*/
	req->request.request_id = 0;
	req->request.para_num = 5;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)(sizeof(fd)));
	req->request.para2_len = htonll((uint64_t) count);
	req->request.para3_len = htonll((uint64_t)(sizeof(count)));
	req->request.para4_len = htonll((uint64_t)(sizeof(curr_offset)));
	req->request.para5_len = htonll((uint64_t)(sizeof(process_pid)));
	req->request.para6_len = 0;
	
	
	/*copy the parameters of the fwrite request to the message body*/		
	memcpy(c->wcurr, &fd, sizeof(fd));
	c->wcurr = c->wcurr + sizeof(fd);
	c->wbytes += sizeof(fd);
	memcpy(c->wcurr, buf, count);
	c->wcurr = c->wcurr + count;
	c->wbytes += count;
	memcpy(c->wcurr, &count, sizeof(count));
	c->wcurr = c->wcurr + sizeof(count);
	c->wbytes += sizeof(count);
	memcpy(c->wcurr, &curr_offset, sizeof(curr_offset));
	c->wcurr = c->wcurr + sizeof(curr_offset);
	c->wbytes += sizeof(curr_offset);
	memcpy(c->wcurr, &process_pid, sizeof(process_pid));
	c->wcurr = c->wcurr + sizeof(process_pid);
	c->wbytes += sizeof(process_pid);
	
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;
	MAP_OR_FAIL(write);
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			c->wbytes -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return -1;
		}
		if(res == -1) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return -1;
		}
	}
	
	int i;
	/*fourth: wait for response, check integrity of the package */
	i = cli_try_read_command(c);
	if(i != 0) {
		return 0;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_WRITE))
		{
			fprintf(stderr, "message failure for magic or opcode\n");
			return 0;
		}
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		fd_map_array[fd].file_offset += ret;
		
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_WRITE))
		{
			fprintf(stderr, "message failure for magic or opcode\n");
			return 0;
		}
		memcpy(&ret, (char*)c->assemb_buf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		fd_map_array[fd].file_offset += ret;
	}

    return(ret);
}

size_t DARSHAN_DECL(fwrite)(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t ret = 0;
	int fd;
	int sfd;
	int connected;
	int process_pid;
	long curr_offset;
	conn_cli* c;
	int path_look = 0;
	char *s;	

	if(stream == NULL) {
		return 0;
	}
	
	fd = fileno(stream);
	if((fd == 0) || (fd == 1) || (fd == 2))
        {
                MAP_OR_FAIL(fwrite);
                return __real_fwrite(ptr, size, nmemb, stream);
        }

	for(path_look = 0; (s = inclusions[path_look]) != NULL; path_look++)
        {
                if(!(strncmp(s, fd_map_array[fd].file_name, strlen(s))))
                        break;
        }
        if(!s) {
                MAP_OR_FAIL(fwrite);
                return __real_fwrite(ptr, size, nmemb, stream);
        }
	


	curr_offset = fd_map_array[fd].file_offset;
	process_pid = getpid_afac();
	/*test the connection already existed or not*/
	struct sockaddr server_addr;
	connected = connection_close_exam(fd_map_array[fd].sfd, &server_addr);
	long data_size = 0;
	if(sizeof(*ptr) < size * nmemb) {
		if(strlen((char *)ptr) + 1 < size * nmemb){
			data_size = size * nmemb;
		}
		if(strlen((char *)ptr) + 1 == size * nmemb){
			data_size = strlen(ptr) + 1;
		}
		data_size = size * nmemb;
	} else {
		data_size = sizeof(*ptr);
	}
	int request_len = data_size + sizeof(size) + sizeof(nmemb) + sizeof(*stream) + sizeof(curr_offset) + sizeof(process_pid);
	int message_len = request_len + sizeof(protocol_binary_request_header);
	int package_num;
	int package_id;
	int package_offset;
	int package_body_len;
	
	if(connected != 0)
	{
		c = cli_socket(&(fd_map_array[fd].server_name[0]), server_port, tcp_transport);
		if(c == NULL) {
			fprintf(stderr, "cli_socket failed\n");
			return 0;
		}
	}
	if(connected == 0) /*just do not consider the ip address equal or not*/
	{
		/*send data to server*/
		c = conn_cli_new(fd_map_array[fd].sfd, 0, tcp_transport);
		if(c == NULL) {
			fprintf(stderr, "conn_cli_new failed\n");
			return 0;
		}	
	}
	
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: fwrite, file: %d\n", fd);
			return 0;
		}
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	c->wbytes += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_FWRITE;
	req->request.isfrag = 1; /*do not package any more*/
	req->request.frag_id = 1; /*beginning from 1*/
	req->request.request_id = 0;
	req->request.para_num = 6;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)data_size);
	req->request.para2_len = htonll((uint64_t)(sizeof(size)));
	req->request.para3_len = htonll((uint64_t)(sizeof(nmemb)));
	req->request.para4_len = htonll((uint64_t)(sizeof(*stream)));
	req->request.para5_len = htonll((uint64_t)(sizeof(curr_offset)));
	req->request.para6_len = htonll((uint64_t)(sizeof(process_pid)));
	
	
	/*copy the parameters of the fwrite request to the message body*/		
	memcpy(c->wcurr, ptr, data_size);
	c->wcurr = c->wcurr + data_size;
	c->wbytes += data_size;
	memcpy(c->wcurr, &size, sizeof(size));
	c->wcurr = c->wcurr + sizeof(size);
	c->wbytes += sizeof(size);
	memcpy(c->wcurr, &nmemb, sizeof(nmemb));
	c->wcurr = c->wcurr + sizeof(nmemb);
	c->wbytes += sizeof(nmemb);
	memcpy(c->wcurr, stream, sizeof(*stream));
	c->wcurr = c->wcurr + sizeof(*stream);
	c->wbytes += sizeof(*stream);
	memcpy(c->wcurr, &curr_offset, sizeof(curr_offset));
	c->wcurr = c->wcurr + sizeof(curr_offset);
	c->wbytes += sizeof(curr_offset);
	memcpy(c->wcurr, &process_pid, sizeof(process_pid));
	c->wcurr = c->wcurr + sizeof(process_pid);
	c->wbytes += sizeof(process_pid);
	
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;
	MAP_OR_FAIL(write);
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			c->wbytes -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return -1;
		}
		if(res == -1) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return -1;
		}
	}
	
	int i;
	/*fourth: wait for response, check integrity of the package */
	i = cli_try_read_command(c);
	if(i != 0) {
		return 0;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_FWRITE))
		{
			fprintf(stderr, "message failure for magic or opcode\n");
			return 0;
		}
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		fd_map_array[fd].file_offset += ret;
		
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_FWRITE))
		{
			fprintf(stderr, "message failure for magic or opcode\n");
			return 0;
		}
		memcpy(&ret, (char*)c->assemb_buf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		fd_map_array[fd].file_offset += ret;
	}
	
    return(ret);
}

off64_t DARSHAN_DECL(lseek64)(int fd, off64_t offset, int whence)
{
    off64_t ret;
   
	int sfd;
	int connected;
	long curr_offset;
	conn_cli* c;
	if(fd_map_array == NULL)
	{
		MAP_OR_FAIL(lseek64);
		return __real_lseek64(fd, offset, whence);
	}
	curr_offset = fd_map_array[fd].file_offset;
	
	switch(whence)
	{
		case SEEK_SET:
			if(offset >= 0) {
				fd_map_array[fd].file_offset = offset;
				return 0;
			} else {
				printf("illegal offset for SEEK_SET\n");
				return -1;
			}
		case SEEK_CUR:
			curr_offset += offset;
			if(curr_offset >= 0) {
				fd_map_array[fd].file_offset = curr_offset;
				return 0;
			} else {
				printf("illegal offset for SEEK_CUR\n");
				return -1;
			}
		case SEEK_END:
			break;
		default:
			printf("illegal whence\n");
			return -1;
	}
	
	
	/*test the connection already existed or not*/
	struct sockaddr server_addr;
	connected = connection_close_exam(fd_map_array[fd].sfd, &server_addr);
	
	int request_len = sizeof(fd) + sizeof(offset) + sizeof(whence) + sizeof(curr_offset);
	int message_len = request_len + sizeof(protocol_binary_request_header);
	
	if(connected != 0)
	{
		c = cli_socket(fd_map_array[fd].server_name, server_port, tcp_transport);
		if(c == NULL) {
			fprintf(stderr, "Can not connect server...\n");
			return -1;
		}
	}
	if(connected == 0) /*just do not consider the ip address equal or not*/
	{
		/*send data to server*/
		c = conn_cli_new(fd_map_array[fd].sfd, 0, tcp_transport);
		if(c == NULL) {
			return -1;
		}	
	}
	
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: fseek, file: %d\n", fd);
			return -1;
		}
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	c->wbytes += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_LSEEK64;
	req->request.isfrag = 1; /*do not package any more*/
	req->request.frag_id = 1; /*beginning from 1*/
	req->request.request_id = 0;
	req->request.para_num = 4;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)(sizeof(fd)));
	req->request.para2_len = htonll((uint64_t)(sizeof(offset)));
	req->request.para3_len = htonll((uint64_t)(sizeof(whence)));
	req->request.para4_len = htonll((uint64_t)(sizeof(curr_offset)));
	req->request.para5_len = htonll((uint64_t)0);
	req->request.para6_len = 0;
	
	
	/*copy the parameters of the fseek request to the message body*/		
	memcpy(c->wcurr, &fd, sizeof(fd));
	c->wcurr = c->wcurr + sizeof(fd);
	c->wbytes += sizeof(fd);
	
	memcpy(c->wcurr, &offset, sizeof(offset));
	c->wcurr = c->wcurr + sizeof(offset);
	c->wbytes += sizeof(offset);
	
	memcpy(c->wcurr, &whence, sizeof(whence));
	c->wcurr = c->wcurr + sizeof(whence);
	c->wbytes += sizeof(whence);
	
	memcpy(c->wcurr, &curr_offset, sizeof(curr_offset));
	c->wcurr = c->wcurr + sizeof(curr_offset);
	c->wbytes += sizeof(curr_offset);
	
	
	
	
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;
	
	MAP_OR_FAIL(write);
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			c->wbytes -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return -1;
		}
		if(res == 0) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return -1;
		}
	}
	fprintf(stderr, "send data finished. there are %d bytes data left in the buffer\n", c->wbytes);
	int i;
	/*fourth: wait for response, check integrity of the package */
	i = cli_try_read_command(c);
	if(i != 0) {
		return -1;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_LSEEK64))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		fd_map_array[fd].file_offset = ret;
		
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_LSEEK64))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		fd_map_array[fd].file_offset = ret;
	}
	
    return(ret);
}

off_t DARSHAN_DECL(lseek)(int fd, off_t offset, int whence)
{
    off_t ret;

	int sfd;
	int connected;
	long curr_offset;
	conn_cli* c;
	curr_offset = fd_map_array[fd].file_offset;
	
	switch(whence)
	{
		case SEEK_SET:
			if(offset >= 0) {
				fd_map_array[fd].file_offset = offset;
				return 0;
			} else {
				printf("illegal offset for SEEK_SET\n");
				return -1;
			}
		case SEEK_CUR:
			curr_offset += offset;
			if(curr_offset >= 0) {
				fd_map_array[fd].file_offset = curr_offset;
				return 0;
			} else {
				printf("illegal offset for SEEK_CUR\n");
				return -1;
			}
		case SEEK_END:
			break;
		default:
			printf("illegal whence\n");
			return -1;
	}
	
	
	/*test the connection already existed or not*/
	struct sockaddr server_addr;
	connected = connection_close_exam(fd_map_array[fd].sfd, &server_addr);
	
	int request_len = sizeof(fd) + sizeof(offset) + sizeof(whence) + sizeof(curr_offset);
	int message_len = request_len + sizeof(protocol_binary_request_header);
	
	if(connected != 0)
	{
		c = cli_socket(fd_map_array[fd].server_name, server_port, tcp_transport);
		if(c == NULL) {
			fprintf(stderr, "Can not connect server...\n");
			return -1;
		}
	}
	if(connected == 0) /*just do not consider the ip address equal or not*/
	{
		/*send data to server*/
		c = conn_cli_new(fd_map_array[fd].sfd, 0, tcp_transport);
		if(c == NULL) {
			return -1;
		}	
	}
	
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: fseek, file: %d\n", fd);
			return -1;
		}
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	c->wbytes += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_LSEEK;
	req->request.isfrag = 1; /*do not package any more*/
	req->request.frag_id = 1; /*beginning from 1*/
	req->request.request_id = 0;
	req->request.para_num = 4;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)(sizeof(fd)));
	req->request.para2_len = htonll((uint64_t)(sizeof(offset)));
	req->request.para3_len = htonll((uint64_t)(sizeof(whence)));
	req->request.para4_len = htonll((uint64_t)(sizeof(curr_offset)));
	req->request.para5_len = htonll((uint64_t)0);
	req->request.para6_len = 0;
	
	
	/*copy the parameters of the fseek request to the message body*/		
	memcpy(c->wcurr, &fd, sizeof(fd));
	c->wcurr = c->wcurr + sizeof(fd);
	c->wbytes += sizeof(fd);
	
	memcpy(c->wcurr, &offset, sizeof(offset));
	c->wcurr = c->wcurr + sizeof(offset);
	c->wbytes += sizeof(offset);
	
	memcpy(c->wcurr, &whence, sizeof(whence));
	c->wcurr = c->wcurr + sizeof(whence);
	c->wbytes += sizeof(whence);
	
	memcpy(c->wcurr, &curr_offset, sizeof(curr_offset));
	c->wcurr = c->wcurr + sizeof(curr_offset);
	c->wbytes += sizeof(curr_offset);
	
	
	
	
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;
	
	MAP_OR_FAIL(write);
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			c->wbytes -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return -1;
		}
		if(res == 0) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return -1;
		}
	}
	fprintf(stderr, "send data finished. there are %d bytes data left in the buffer\n", c->wbytes);
	int i;
	/*fourth: wait for response, check integrity of the package */
	i = cli_try_read_command(c);
	if(i != 0) {
		return -1;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_LSEEK))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		fd_map_array[fd].file_offset = ret;
		
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_LSEEK))
		{
			return -1;
		}
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		fd_map_array[fd].file_offset = ret;
	}
	
    return(ret);
}

ssize_t DARSHAN_DECL(aio_return64)(struct aiocb64 *aiocbp)
{
    int ret;
    

    return(ret);
}

ssize_t DARSHAN_DECL(aio_return)(struct aiocb *aiocbp)
{
    int ret;

    return(ret);
}

int DARSHAN_DECL(lio_listio)(int mode, struct aiocb *const aiocb_list[],
    int nitems, struct sigevent *sevp)
{
    int ret;
    int i;


    return(ret);
}

int DARSHAN_DECL(lio_listio64)(int mode, struct aiocb64 *const aiocb_list[],
    int nitems, struct sigevent *sevp)
{
    int ret;
    int i;



    return(ret);
}

int DARSHAN_DECL(aio_write64)(struct aiocb64 *aiocbp)
{
    int ret;



    return(ret);
}

int DARSHAN_DECL(aio_write)(struct aiocb *aiocbp)
{
    int ret;



    return(ret);
}

int DARSHAN_DECL(aio_read64)(struct aiocb64 *aiocbp)
{
    int ret;



    return(ret);
}

int DARSHAN_DECL(aio_read)(struct aiocb *aiocbp)
{
    int ret;

   

    return(ret);
}

int DARSHAN_DECL(fseek)(FILE *stream, long offset, int whence)
{
    int ret;
	int fd;
	int sfd;
	int connected;
	long curr_offset;
	conn_cli* c;
	
	if(!stream)
		return 0;
	fd = fileno(stream);
	fprintf(stderr, "The fseek operation of file %d\n", fd);
	curr_offset = fd_map_array[fd].file_offset;
	
	switch(whence)
	{
		case SEEK_SET:
			if(offset >= 0) {
				fd_map_array[fd].file_offset = offset;
				return 0;
			} else {
				printf("illegal offset for SEEK_SET\n");
				return -1;
			}
		case SEEK_CUR:
			curr_offset += offset;
			if(curr_offset >= 0) {
				fd_map_array[fd].file_offset = curr_offset;
				return 0;
			} else {
				printf("illegal offset for SEEK_CUR\n");
				return -1;
			}
		case SEEK_END:
			break;
		default:
			printf("illegal whence\n");
			return -1;
	}
	
	
	/*test the connection already existed or not*/
	struct sockaddr server_addr;
	connected = connection_close_exam(fd_map_array[fd].sfd, &server_addr);
	
	int request_len = sizeof(*stream) + sizeof(offset) + sizeof(whence) + sizeof(curr_offset);
	int message_len = request_len + sizeof(protocol_binary_request_header);
	
	if(connected != 0)
	{
		c = cli_socket(fd_map_array[fd].server_name, server_port, tcp_transport);
		if(c == NULL) {
			fprintf(stderr, "Can not connect server...\n");
			return -1;
		}
	}
	if(connected == 0) /*just do not consider the ip address equal or not*/
	{
		/*send data to server*/
		c = conn_cli_new(fd_map_array[fd].sfd, 0, tcp_transport);
		if(c == NULL) {
			return -1;
		}	
	}
	
	if(c->wsize < message_len)
	{
		char *newbuf = realloc(c->wbuf, message_len);
		if(newbuf != NULL) {
			c->wbuf = newbuf;
			c->wsize = message_len;
			c->wcurr = c->wbuf;
		} else {
			perror("Out of memory");
			fprintf(stderr, "no memory for sendbuf, op: fseek, file: %d\n", fd);
			return -1;
		}
	}
	
	protocol_binary_request_header *req;
	req = (protocol_binary_request_header *)(c->wcurr);
	c->wcurr += sizeof(protocol_binary_request_header);
	c->wbytes += sizeof(protocol_binary_request_header);
	/*fill the header of the message*/
	req->request.magic = PROTOCOL_BINARY_REQ;
	req->request.opcode = PROTOCOL_BINARY_CMD_FSEEK;
	req->request.isfrag = 1; /*do not package any more*/
	req->request.frag_id = 1; /*beginning from 1*/
	req->request.request_id = 0;
	req->request.para_num = 4;
	req->request.reserved = 0;
	req->request.body_len = htonll((uint64_t)request_len);
	req->request.request_len = req->request.body_len;
	req->request.frag_offset = 0;
	req->request.para_len = req->request.request_len;
	
	req->request.para1_len = htonll((uint64_t)(sizeof(*stream)));
	req->request.para2_len = htonll((uint64_t)(sizeof(offset)));
	req->request.para3_len = htonll((uint64_t)(sizeof(whence)));
	req->request.para4_len = htonll((uint64_t)(sizeof(curr_offset)));
	req->request.para5_len = htonll((uint64_t)0);
	req->request.para6_len = 0;
	
	
	/*copy the parameters of the fseek request to the message body*/		
	memcpy(c->wcurr, stream, sizeof(*stream));
	c->wcurr = c->wcurr + sizeof(*stream);
	c->wbytes += sizeof(*stream);
	
	memcpy(c->wcurr, &offset, sizeof(offset));
	c->wcurr = c->wcurr + sizeof(offset);
	c->wbytes += sizeof(offset);
	
	memcpy(c->wcurr, &whence, sizeof(whence));
	c->wcurr = c->wcurr + sizeof(whence);
	c->wbytes += sizeof(whence);
	
	memcpy(c->wcurr, &curr_offset, sizeof(curr_offset));
	c->wcurr = c->wcurr + sizeof(curr_offset);
	c->wbytes += sizeof(curr_offset);
	
	
	
	
	/*write the message to the server*/
	int res = 0;
	int left = message_len;
	char *snd_curr = c->wbuf;
	
	MAP_OR_FAIL(write);
	while(left > 0)
	{
		res = __real_write(c->sfd, snd_curr, (size_t)left);
		if(res > 0)
		{
			left -= res;
			c->wbytes -= res;
			snd_curr = snd_curr + res;
		}
		/*error handling*/
		if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
			break;
		if(res == 0) {
			fprintf(stderr, "The connection is closed\n");
			return 0;
		}
		if(res == 0) {
			fprintf(stderr, "send data error: some unknown error happen\n");
			return 0;
		}
	}
	fprintf(stderr, "send data finished. there are %d bytes data left in the buffer\n", c->wbytes);
	int i;
	/*fourth: wait for response, check integrity of the package */
	i = cli_try_read_command(c);
	if(i != 0) {
		return -1;
	} 
	protocol_binary_response_header *resp;
	/*fifth: finish this I/O request*/
	if(c->frag_num == 1)
	{
		resp = (protocol_binary_response_header *)c->rbuf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_FSEEK))
		{
			return -1;
		}
		memcpy(stream, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		memcpy(&(fd_map_array[fd].file_offset), (char*)c->rbuf + sizeof(protocol_binary_response_header) + resp->response.para1_len, resp->response.para2_len);
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header) + resp->response.para1_len + resp->response.para2_len, resp->response.para3_len);
		
	}
	if(c->frag_num > 1)
	{
		resp = (protocol_binary_response_header *)c->assemb_buf;
		if((resp->response.magic != PROTOCOL_BINARY_RES) || (resp->response.opcode != PROTOCOL_BINARY_CMD_FSEEK))
		{
			return -1;
		}
		memcpy(stream, (char*)c->rbuf + sizeof(protocol_binary_response_header), resp->response.para1_len);
		memcpy(&(fd_map_array[fd].file_offset), (char*)c->rbuf + sizeof(protocol_binary_response_header) + resp->response.para1_len, resp->response.para2_len);
		memcpy(&ret, (char*)c->rbuf + sizeof(protocol_binary_response_header) + resp->response.para1_len + resp->response.para2_len, resp->response.para3_len);

	}
	
    return(ret);
}
