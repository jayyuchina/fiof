#ifndef _COMMUNICATION_H
#define _COMMUNICATION_H
#include <stdlib.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#define server_port 11212

enum network_transport
{
    local_transport,
    tcp_transport,
    udp_transport
};


typedef struct
{
    int sfd;
    short cmd;
    uint64_t r_len;
    uint64_t body_len;

    char *rbuf;
    char *rcurr;
    int rsize;
    int rbytes;

    char *wbuf;
    char *wcurr;
    int wsize;
    int wbytes;

    struct msghdr *msglist;
    int msgsize;
    int msgused;
    int msgcurr;
    int msgbytes;

    struct iovec *iov;
    int iovsize;
    int iovused;

    enum network_transport transport;

    uint8_t frag_flag[256];
    int frag_num;
    int frag_lack;

    char *assemb_buf;
    int assemb_size;


} conn_cli;

#define IS_UDP(x) (x == udp_transport)

/*socket related*/
#define DATA_BUFFER_SIZE 2048
#define UDP_READ_BUFFER_SIZE 65536
#define UDP_HEADER_SIZE 8
#define UDP_MAX_PAYLOAD_SIZE 1400
#define MAX_SENDBUF_SIZE (256 * 1024 * 1024)

#define PAGE_SIZE_LIMIT 1400

/** High water marks for buffer shrinking */
#define READ_BUFFER_HIGHWAT 8192
#define ITEM_LIST_HIGHWAT 400
#define IOV_LIST_HIGHWAT 600
#define MSG_LIST_HIGHWAT 100

/** Initial size of the sendmsg() scatter/gather array. */
#define IOV_LIST_INITIAL 400
/** Initial number of sendmsg() argument structures to allocate. */
#define MSG_LIST_INITIAL 10

/*package related, */
#define PACKAGE_MAX 65536
conn_cli *get_conn(int sfd);
extern conn_cli *conn_cli_new(int sfd, int data_buffer_size, enum network_transport transport);
conn_cli* cli_socket(char *server_name, int port, enum network_transport transport);
int cli_must_read_command(conn_cli *c);
#endif
