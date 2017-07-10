
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/resource.h>
#include <sysexits.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <assert.h>
#include "../protocol_binary.h"
#include "communication.h"
#include "util.h"

conn_cli **conns;
static int max_fds;
static void conn_cli_free(conn_cli *c);

int conn_cli_init(int max)
{
    int next_fd = dup(1);
    int headroom = 10;
    struct rlimit rl;

    if( max >= 0)
        max_fds = max + headroom + next_fd;
    else
        max_fds = 1024 + headroom + next_fd;

    if(getrlimit(RLIMIT_NOFILE, &rl) == 0)
        max_fds = rl.rlim_max;
    else
        fprintf(stderr, "Failed to query maximum file descriptor; "
                "falling back to maxconns\n");
    __real_close(next_fd);

    if((conns = calloc(max_fds, sizeof(conn_cli *))) == NULL)
    {
        fprintf(stderr, "Failed to allocate connection structures\n");
        return -1;
    }
    return 0;
}

conn_cli *get_conn(int sfd)
{
	assert(conns[sfd] != NULL);
	return conns[sfd];
}

conn_cli *conn_cli_new(int sfd, int data_buffer_size, enum network_transport transport)
{
    conn_cli *c;
    assert(sfd >= 0 && sfd < max_fds);

    c = conns[sfd];
    if(c == NULL)
    {
        c = (conn_cli *)calloc(1, sizeof(conn_cli));
        if(c == NULL)
        {
            fprintf(stderr, "Failed to allocate connection object\n");
            return NULL;
        }
        c->rbuf = c->wbuf = 0;
        c->msglist = 0;
        c->iov = 0;
        if(data_buffer_size != 0)
            c->rsize = c->wsize = data_buffer_size;
        else
            c->rsize = c->wsize = DATA_BUFFER_SIZE;

        c->iovsize = IOV_LIST_INITIAL;
        c->msgsize = MSG_LIST_INITIAL;

        c->rbuf = (char *)malloc((size_t)c->rsize);
        c->wbuf = (char *)malloc((size_t)c->wsize);
        c->iov = (struct iovec *)malloc(sizeof(struct iovec) * c->iovsize);
        c->msglist = (struct msghdr *)malloc(sizeof(struct msghdr) * c->msgsize);
        if (c->rbuf == 0 || c->wbuf == 0 || c->iov == 0 || c->msglist == 0)
        {
            conn_cli_free(c);
            fprintf(stderr, "Failed to allocate buffers for connection\n");
            return NULL;
        }
        c->sfd = sfd;
        conns[sfd] = c;
		
    }
    c->transport = transport;

    c->assemb_buf = NULL;
    c->assemb_size = 0;
    c->frag_num = 0;
    c->frag_lack = -1;

    c->rbytes = c->wbytes = 0;
    c->rcurr = c->rbuf;
    c->wcurr = c->wbuf;
    c->iovused = 0;
    c->msgcurr = 0;
    c->msgused = 0;
    c->sfd = sfd;

    c->cmd = -1;
    c->body_len = 0;
    c->r_len = 0;

    return c;
}

static void conn_cli_free(conn_cli *c)
{
    if( c != NULL)
    {
        conns[c->sfd] = NULL;
        if(c->msglist)
            free(c->msglist);
        if(c->iov)
            free(c->iov);
        if(c->rbuf)
            free(c->rbuf);
        if(c->wbuf)
            free(c->wbuf);
        free(c);
    }
}

static int new_socket(struct addrinfo *ai);
static void maximize_sndbuf(int sfd);

static int set_socket_nonblock(int fd)
{
   if (fd < 0) return -1;

   int flags = fcntl(fd, F_GETFL, 0);
   if (flags < 0) return -1;
   flags = flags | O_NONBLOCK;
   return (fcntl(fd, F_SETFL, flags) == 0) ? 1 : -1;
}

conn_cli* cli_socket(char *server_name, int port, enum network_transport transport)
{
    int sfd;
    conn_cli *c;

    struct addrinfo *ai;
    struct addrinfo *next;
    struct addrinfo hints = { .ai_flags = AI_PASSIVE, .ai_family = AF_UNSPEC };
    hints.ai_socktype = IS_UDP(transport)? SOCK_DGRAM: SOCK_STREAM;

    char port_buf[NI_MAXSERV];
    int error;
    int success = 0;
    int flags = 1;
	int snd_buf_size = 2 * 1024 * 1024;
    struct linger ling = {0, 0};
    if(port == -1)
        port = 0;

    /*new socket for client*/
    char interface[10];
    gethostname(interface, sizeof(interface));
    error = getaddrinfo(interface, NULL, &hints, &ai);
    if (error != 0)
    {
        if (error != EAI_SYSTEM)
            fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
        else
            perror("getaddrinfo()");
        return NULL;
    }

    for(next = ai; next; next = next->ai_next)
    {
        if((sfd = new_socket(next)) == -1)
        {
            if(errno == EMFILE)
            {
                perror("client_socket");
                exit(EX_OSERR);
            }
            continue;
        }
        else
        {
            setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
            if(!IS_UDP(transport))
            {
                error = setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
                if (error != 0)
                    perror("setsockopt");
                error = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
                if (error != 0)
                    perror("setsockopt");
                error = setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
                if (error != 0)
                    perror("setsockopt");
            	error = setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&snd_buf_size, sizeof(snd_buf_size));
            	if (error != 0)
                	perror("setsockopt");
                //maximize_sndbuf(sfd);
                success++;
                break;
            }
            else
            {
                maximize_sndbuf(sfd);
                success++;
                break;
            }
        }
    }
    freeaddrinfo(ai);
    if(success <= 0)
    {
        fprintf(stderr, "new socket failure\n");
        return NULL;
    }


    if(IS_UDP(transport))
    {
        c = conn_cli_new(sfd, UDP_READ_BUFFER_SIZE, transport);
        return c;
    }

    /*for TCP transport, we need to know the ipaddress and port information for send,
    we can get the port number and hostname from parameters*/
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    error = getaddrinfo(server_name, port_buf, &hints, &ai);		// get the server's ip
    if (error != 0)
    {
        if (error != EAI_SYSTEM)
            fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
        else
            perror("getaddrinfo()");
        return NULL;
    }

    success = 0;
    for(next = ai; next; next = next->ai_next)
    {
        error = connect(sfd, next->ai_addr, next->ai_addrlen);
        if(error == -1)
        {
            switch(errno)
            {
            case EBADF:
                printf("sockfd is illegal\n");
                break;
            case ENOTSOCK:
                printf("sockfd is not a socket\n");
                break;
            case EISCONN:
                printf("sockfd is already connected\n");
                break;
            case ECONNREFUSED:
                printf("server refused\n");
                break;
            case ETIMEDOUT:
                printf("timeout\n");
                break;
            case ENETUNREACH:
                printf("can not send data to server\n");
                break;
            case EAFNOSUPPORT:
                printf("sa_family not correct\n");
                break;
            case EALREADY:
                printf("not finished\n");
                break;
            default:
                printf("the errno is %d\n",errno);
                break;

            }

            if(errno == EINPROGRESS)
            {
                success++;
                break;
            }
            continue;
        }
        else
        {
            success++;
            break;
        }
    }

    freeaddrinfo(ai);
    if(success >0)
    {		
        c = conn_cli_new(sfd, 0, transport);
		if(set_socket_nonblock(c->sfd) < 0) 
			perror("wrong in set socket nonblock");
        return c;
    }
    else
    {
        fprintf(stderr, "TCP connect failure on fd: %d\n", sfd);
        return NULL;
    }
}


static int new_socket(struct addrinfo *ai)
{
    int sfd;
    int flags;

    if ((sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1)
    {
        return -1;
    }

    /*    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
    			fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("setting O_NONBLOCK");
            close(sfd);
            return -1;
        }
    */
    return sfd;
}

static void maximize_sndbuf(int sfd)
{
    socklen_t intsize = sizeof(int);
    int last_good = 0;
    int min, max, avg;
    int old_size;
    if(getsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &old_size, &intsize) != 0)
    {
        perror("getsockopt(SO_SNDBUF)");
        return;
    }
    min = old_size;
    max = MAX_SENDBUF_SIZE;
    while(min <= max)
    {
        avg = ((unsigned int)(min + max))/2;
        if(setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&avg, intsize) == 0)
        {
            min = avg + 1;
            last_good = avg;
        }
        else
        {
            max = avg - 1;
        }
    }
    fprintf(stderr, "<%d send buffer was %d, now %d\n", sfd, old_size, last_good);
}

int add_cli_msghdr(conn_cli *c)
{
    struct msghdr *msg;
    if(c->msgsize == c->msgused)
    {
        msg = realloc(c->msglist, c->msgsize * 2 * sizeof(struct msghdr));
        if(!msg)
        {
            fprintf(stderr, "Memory allocation for msglist failed on socket %d\n", c->sfd);
            return -1;
        }
        c->msglist = msg;
        c->msgsize *= 2;
    }

    msg = c->msglist + c->msgused;
    memset(msg, 0, sizeof(struct msghdr));
    msg->msg_iov = &c->iov[c->iovused];
    c->msgbytes = 0;
    c->msgused++;


    msg = &c->msglist[c->msgused - 1];
    msg->msg_iov[msg->msg_iovlen].iov_base = (void *)c->rcurr;
    msg->msg_iov[msg->msg_iovlen].iov_len = c->rsize;
    c->iovused++;
    msg->msg_iovlen++;

    return 0;
}

int delete_cli_msghdr(conn_cli *c)
{
    struct msghdr *msg;
    if(c->msgused > 0)
    {
        msg = &c->msglist[c->msgused - 1];
        msg->msg_iov[msg->msg_iovlen - 1].iov_base = NULL;
        msg->msg_iov[msg->msg_iovlen - 1].iov_len = 0;
        msg->msg_iovlen--;
        if(msg->msg_iovlen == 0)
            msg->msg_iov = NULL;
        c->iovused--;
        c->msgbytes = 0;
    }
    return 0;
}

void response_network_to_host(char* buffer);
int cli_try_read_command(conn_cli *c)
{
    assert(c != NULL);
    int i, condition, res;
    char *assemb_temp = NULL;
    char *rbuf_temp;
    struct msghdr *msg;
    protocol_binary_response_header* response;

    i = add_cli_msghdr(c);
    if(i == -1)
    {
        fprintf(stderr, "Add msghdr error for sfd: %d\n", c->sfd);
        return -1;
    }

    condition = 1;
    while (condition)
    {
        if(c->rbytes == c->rsize)
        {
            rbuf_temp = (char *)realloc(c->rbuf, c->rsize * 2);
            if(!rbuf_temp)
            {
                c->rbuf = rbuf_temp;
                c->rsize *= 2;
                c->rcurr = c->rbuf + c->rbytes;
            }
            else
            {
                fprintf(stderr, "Memory reallocation for c->rbuf failed!\n");
                delete_cli_msghdr(c);
                return -1;
            }
        }

        msg = &c->msglist[c->msgused - 1];
        msg->msg_iov[msg->msg_iovlen].iov_base = (void *)c->rcurr;
        msg->msg_iov[msg->msg_iovlen].iov_len = c->rsize - c->rbytes;
		//printf("reading...\n");
        res = recvmsg(c->sfd, &(c->msglist[c->msgused - 1]), 0);
		//printf("read %d\n", res);

        if(res > 0)
        {
            c->rcurr += res;
            c->rbytes += res;
            c->msgbytes += res;
            if(c->rbytes < sizeof(protocol_binary_response_header))
            {
                break;
            }
            else
            {
                condition = 0;
                response = (protocol_binary_response_header* )c->rbuf;
                response_network_to_host((char*)response);
                c->frag_num = response->response.isfrag;
                c->frag_lack = c->frag_num;
                c->cmd = response->response.opcode;
                c->body_len = response->response.body_len;
                c->r_len = response->response.response_len;

                if((c->frag_num > 1) && (c->body_len < c->r_len))
                {
                    if(c->assemb_buf == NULL)
                    {
                        c->assemb_buf = (char *)calloc(1,c->r_len + sizeof(protocol_binary_response_header));
                        if(c->assemb_buf)
                        {
                            fprintf(stderr, "Memory allocate failed for assemb_buf of opereation fopen\n");
                            delete_cli_msghdr(c);
                            return -1;
                        }
                        c->assemb_size = c->r_len + sizeof(protocol_binary_response_header);
                    }
                    else
                    {
                        if(c->assemb_size < c->r_len + sizeof(protocol_binary_response_header))
                        {
                            assemb_temp = realloc(c->assemb_buf, c->r_len + sizeof(protocol_binary_response_header));
                            if(assemb_temp != NULL)
                            {
                                c->assemb_buf = assemb_temp;
                                c->assemb_size = c->r_len + sizeof(protocol_binary_response_header);
                            }
                            else
                            {
                                free(c->assemb_buf);
                                c->assemb_size = 0;
                                delete_cli_msghdr(c);
                                return -1;
                            }
                        }
                    }
                }

                if(cli_try_read_response(c) != 0)
                {
                    delete_cli_msghdr(c);
                    return -1;
                }
            }
            break;
        }
        if(res == 0)
        {
            /*connection has been closed*/
            delete_cli_msghdr(c);
            return -1;
        }

        if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
        {
            break;
        }
        else
        {
            delete_cli_msghdr(c);
            return -1;
        }
    }
    delete_cli_msghdr(c);
    return 0;
}

int cli_must_read_command(conn_cli *c)
{
    assert(c != NULL);
    int i, condition, res;
    char *assemb_temp = NULL;
    char *rbuf_temp;
    struct msghdr *msg;
    protocol_binary_response_header* response;

    i = add_cli_msghdr(c);
    if(i == -1)
    {
        fprintf(stderr, "Add msghdr error for sfd: %d\n", c->sfd);
        return -1;
    }

    condition = 1;
    while (condition)
    {
        if(c->rbytes == c->rsize)
        {
            rbuf_temp = (char *)realloc(c->rbuf, c->rsize * 2);
            if(!rbuf_temp)
            {
                c->rbuf = rbuf_temp;
                c->rsize *= 2;
                c->rcurr = c->rbuf + c->rbytes;
            }
            else
            {
                fprintf(stderr, "Memory reallocation for c->rbuf failed!\n");
                delete_cli_msghdr(c);
                return -1;
            }
        }

        msg = &c->msglist[c->msgused - 1];
        msg->msg_iov[msg->msg_iovlen].iov_base = (void *)c->rcurr;
        msg->msg_iov[msg->msg_iovlen].iov_len = c->rsize - c->rbytes;
		//printf("reading...\n");
        res = recvmsg(c->sfd, &(c->msglist[c->msgused - 1]), 0);
		//printf("read %d\n", res);

        if(res > 0)
        {
            c->rcurr += res;
            c->rbytes += res;
            c->msgbytes += res;
            if(c->rbytes < sizeof(protocol_binary_response_header))
            {
                break;
            }
            else
            {
                condition = 0;
                response = (protocol_binary_response_header* )c->rbuf;
                response_network_to_host((char*)response);
                c->frag_num = response->response.isfrag;
                c->frag_lack = c->frag_num;
                c->cmd = response->response.opcode;
                c->body_len = response->response.body_len;
                c->r_len = response->response.response_len;

                if((c->frag_num > 1) && (c->body_len < c->r_len))
                {
                    if(c->assemb_buf == NULL)
                    {
                        c->assemb_buf = (char *)calloc(1,c->r_len + sizeof(protocol_binary_response_header));
                        if(c->assemb_buf)
                        {
                            fprintf(stderr, "Memory allocate failed for assemb_buf of opereation fopen\n");
                            delete_cli_msghdr(c);
                            return -1;
                        }
                        c->assemb_size = c->r_len + sizeof(protocol_binary_response_header);
                    }
                    else
                    {
                        if(c->assemb_size < c->r_len + sizeof(protocol_binary_response_header))
                        {
                            assemb_temp = realloc(c->assemb_buf, c->r_len + sizeof(protocol_binary_response_header));
                            if(assemb_temp != NULL)
                            {
                                c->assemb_buf = assemb_temp;
                                c->assemb_size = c->r_len + sizeof(protocol_binary_response_header);
                            }
                            else
                            {
                                free(c->assemb_buf);
                                c->assemb_size = 0;
                                delete_cli_msghdr(c);
                                return -1;
                            }
                        }
                    }
                }

                if(cli_try_read_response(c) != 0)
                {
                    delete_cli_msghdr(c);
                    return -1;
                }
            }
            break;
        }
        if(res == 0)
        {
            /*connection has been closed*/
            delete_cli_msghdr(c);
            return -1;
        }

        if((res == -1)&&((errno == EAGAIN)||(errno == EWOULDBLOCK)))
        {
			continue;
        }
        else
        {
            delete_cli_msghdr(c);
            return -1;
        }
    }
    delete_cli_msghdr(c);
    return 0;
}


int cli_try_read_response(conn_cli *c)
{
    assert(c!= NULL);
    protocol_binary_response_header* req;
    struct msghdr *msg;
    int condition = 1;
    int need_read = 0;
    int res;
    int ntoh_done = 0;
    char * rbuf_temp = NULL;
    while(condition)
    {
        if(need_read)
        {
            if(c->rbytes == c->rsize)
            {
                rbuf_temp = (char *)realloc(c->rbuf, c->rsize * 2);
                if(!rbuf_temp)
                {
                    c->rbuf = rbuf_temp;
                    c->rsize *= 2;
                    c->rcurr = c->rbuf + c->rbytes;
                }
                else
                {
                    fprintf(stderr, "Memory reallocation for c->rbuf failed!\n");
                    return -1;
                }
            }
            msg = &c->msglist[c->msgused - 1];
            msg->msg_iov[msg->msg_iovlen - 1].iov_base = (void *)c->rcurr;
            msg->msg_iov[msg->msg_iovlen - 1].iov_len = c->rsize - c->rbytes;

            res = recvmsg(c->sfd, &(c->msglist[c->msgused - 1]), 0);
            if(res == 0)
            {
                fprintf(stderr, "The connection has been closed or some other error happend at sfd:\n", c->sfd);
                return -1;
            }
            if((res == -1) && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                need_read = 1;
                break;
            }
            if(res == -1)
            {
                return -1;
            }
            if(res > 0)
            {
                c->rcurr += res;
                c->rbytes += res;
                c->msgbytes += res;
            }
        }

        if(c->rbytes < sizeof(protocol_binary_response_header))
        {
            need_read = 1;
            fprintf(stderr, "This is not a situation that should appear!\n");
            continue;
        }
        else if((c->frag_num != c->frag_lack) && (ntoh_done != 1))
        {
            req = (protocol_binary_response_header* )c->rbuf;
            response_network_to_host((char*)req);
            ntoh_done = 1;
        }
        req = (protocol_binary_response_header*)c->rbuf;
        if(req->response.body_len > c->rbytes - sizeof(protocol_binary_response_header))
        {
            need_read = 1;
            continue;
        }

        /*The whole package has aready been received by the host*/

        if(c->frag_num == 1)
        {
            /*this is not a muti-packages response*/

            condition = 0;
            ntoh_done = 0;
            return 0;
        }
        else
        {
            ntoh_done = 0;
            if(c->frag_lack == c->frag_num)
            {
                /*copy the response header in the buf*/
                memcpy(c->assemb_buf, c->rbuf, sizeof(protocol_binary_response_header));
            }
            if(c->cmd == req->response.opcode)
            {
                if(c->frag_flag[req->response.frag_id - 1] == 0)
                {
                    memcpy(c->assemb_buf + req->response.frag_offset + sizeof(protocol_binary_response_header),
                           c->rbuf + sizeof(protocol_binary_response_header),req->response.body_len);
                    c->frag_flag[req->response.frag_id - 1] = 1;
                    c->frag_lack--;
                }
            }
            c->rbytes = c->rbytes - (req->response.body_len + sizeof(protocol_binary_response_header));
            memmove(c->rbuf, c->rbuf + req->response.body_len + sizeof(protocol_binary_response_header), c->rbytes);
            c->rcurr = c->rcurr - (req->response.body_len + sizeof(protocol_binary_response_header));
        }

        if(c->frag_lack == 0)
        {
            condition = 0;
            return 0;
        }
    }

}

void response_network_to_host(char* buffer)
{
    protocol_binary_response_header* req;
    req = (protocol_binary_response_header*)buffer;

    req->response.reserved = ntohs(req->response.reserved);
    req->response.body_len = ntohll(req->response.body_len);
    req->response.response_len = ntohll(req->response.response_len);
    req->response.para_len = ntohll(req->response.para_len);
    req->response.para1_len = ntohll(req->response.para1_len);
    req->response.para2_len = ntohll(req->response.para2_len);
    req->response.para3_len = ntohll(req->response.para3_len);
    req->response.para4_len = ntohll(req->response.para4_len);
    req->response.para5_len = ntohll(req->response.para5_len);
    req->response.para6_len = ntohll(req->response.para6_len);
    req->response.frag_offset = ntohll(req->response.frag_offset);

}

int connection_close_exam(int sfd, struct sockaddr *sa)
{
    int r;
    int len = sizeof(struct sockaddr);
    r = getpeername(sfd, sa, &len);
    if(r == 0)
        return 0;		// exit without error
    if(r < 0 && (errno == ENOTCONN || errno == ENOTSOCK))
        return 1;
    return -1;
}

















