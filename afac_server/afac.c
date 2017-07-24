#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <string.h>
#include <sysexits.h>
#include <assert.h>
#include <event2/event.h>
#include "afac.h"
#include "hash.h"
#include "assoc.h"
#include "cache.h"
#include "items.h"
#include "file_table.h"
#include "thread.h"


// JAY
#include "hashtable.h"
#include "../afac_common/jay_global.h"
#include "../afac_common/protocol_binary.h"
#include "../afac_common/glex.h"
#include "shf.private.h"
#include "shf.h"
#include "tap.h"
#include <sys/sem.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../afac_common/configure.h"
#include "../afac_common/ion_util.h"




#ifndef HAVE_OFF64_T
typedef int64_t off64_t;
#endif

#ifndef __need_IOV_MAX
#define __need_IOV_MAX
#endif

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif


#define hashsize(n) ((unsigned long int)1<<(n))
#define hashmask(n) (hashsize(n)-1)

/********************************************************************/
/**********************  JAY STRUCTS  ******* *************************/


/********************************************************************/
/**********************  JAY GLOBLES  ******* *************************/
static int DEBUG;

static server_name 	*G_SERVER_NAME_ARRAY;
static int 			G_SERVER_NUM;
static int 			MY_SERVER_ID;


static char tmp_block_buf[G_BLOCK_SIZE_IN_BYTES];

static rdma_endpoint *srv_rdmas;
pthread_cond_t srv_mem_blocks_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t srv_mem_blocks_mutex = PTHREAD_MUTEX_INITIALIZER;
static rdma_mem_block *srv_mem_blocks;
static int MAX_NUM_SRV_RDMA_MEM_BLOCK;
static int num_free_srv_rdma_mem_block;


static Config_Param config_param;

/********************************************************************/
/**********************  JAY DECLS  ***********************************/


static void put_entry_metadata_cache(char * path, int file_block_id, MetadataCachingEntry * entry);
static MetadataCachingEntry * get_entry_metadata_cache(char * path, int file_block_id);
static void del_entry_metadata_cache(char * path, int file_block_id);


// open mds file table
static void open_mds_file_table_init();
static int check_if_mds_file_is_opened(char *path, int path_len);
static void insert_open_mds_file(char *path, int path_len, int mfd);
static int get_open_mds_file_mfd(char *path, int path_len);
static void remove_open_mds_file(char *path, int path_len);
static inline int check_if_mfd_is_occupied(int mfd);

// ost file table
static void ost_file_table_init();
static int check_if_ost_file_exists(int cfd);
static void insert_ost_file(int cfd, Ost_File *calloc_file);
static Ost_File * get_ost_file(int cfd);
static void remove_ost_file(int cfd);

// mfd array
static int assign_mfd(int arg_num, char *path, int flag, int mode);
static void free_mfd(int mfd);
static mfd_array_entry * get_mfd_entry(int mfd);


//
static void query_block_metadata(int mfd, int start_block_index, int num_block, int *buf,
                                 int query_server_id, SuccessiveInfo *successive_info, size_t size);


static int read_blocks_from_lustre(int fd, char*buf, int *block_array, int num_block,
                                   int first_block_start_offset, int last_block_end_offset);

//

static void set_conn_cork(int sfd);
static void set_conn_uncork(int sfd);
static void set_conn_snd_buf(int sfd);


static void init_ost_file(Ost_File * file);
static void update_ost_file_size(Ost_File * file, size_t offset, size_t count);
static uint64_t read_ost_data(Ost_File *file, char* buf, int* file_block_array, int num_file_block,
                              int first_block_start_offset, int last_block_end_offset);
static uint64_t write_ost_data(Ost_File *file, char* data_buf, uint64_t data_buf_len,
                               int* file_block_array, int num_file_block,
                               int first_block_start_offset, int last_block_end_offset);

static void write_block(Ost_File *file, char* buf, block_data_req *blk_req);
static void read_block(Ost_File *file, char* buf, block_data_req *blk_req);
static void prepare_for_first_cached_in_data(Ost_File *ost_file, int *metadata_array, int num_block, int start_block_id,
															long offset, size_t size, int is_read);

// RDMA

static rdma_mem_block* get_mem_block();
static void release_mem_block(int mem_block_id);




/*
 * forward declarations
 */
static void drive_machine(conn *c);
static int new_socket(struct addrinfo *ai);
static int try_read_command(conn *c);
static int server_socket(const char *interface, int port, enum network_transport transport, FILE *portnumber_file);
conn *conn_new(const int sfd, enum conn_states init_state,
               const int event_flags,
               const int read_buffer_size, enum network_transport transport,
               struct event_base *base);
void event_handler(const int fd, const short which, void *arg);
static void maximize_sndbuf(const int sfd);
static int add_msghdr(conn *c);
static void out_of_memory(conn *c, char *ascii_error);
static void dispatch_bin_command(conn * c);
static void write_io_trace_to_file(LIBEVENT_THREAD * curr_thread, FILE *t_file);
static void write_io_trace(conn *c);



enum try_read_result
{
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,            /** an error occured (on the socket) (or client closed connection) */
    READ_MEMORY_ERROR      /** failed to allocate more memory */
};


struct settings global_settings;
struct stats_t stats;
FILE *trace_file = NULL;

struct file_mapping *mapping;

enum hashfunc_type hash_type = JENKINS_HASH;

struct item_list
{
    unsigned long start_offset;
    unsigned long end_offset;
    item* it;
};

/*communication*/
static conn *listen_conn = NULL;
static struct event_base *main_base;

enum transmit_result
{
    TRANSMIT_COMPLETE,   /** All done writing. */
    TRANSMIT_INCOMPLETE, /** More data remaining to write. */
    TRANSMIT_SOFT_ERROR, /** Can't write any more right now. */
    TRANSMIT_HARD_ERROR  /** Can't write (c->state is set to conn_closing) */
};


static conn **conns;


/* This reduces the latency without adding lots of extra wiring to be able to
 * notify the listener thread of when to listen again.
 * Also, the clock timer could be broken out into its own thread and we
 * can block the listener via a condition.
 */
static volatile bool allow_new_conns = true;
extern pthread_mutex_t *file_find_open_clean_locks;




static struct event maxconnsevent;


/*
static void file_mapping_init()
{
    int i = 0;
    int next_fd = dup(1);
    int headroom = 10;      
    struct rlimit rl;

    MAX_FDS = global_settings.max_open_files + headroom + next_fd;

    
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
    {
        MAX_FDS = rl.rlim_max;
        printf("Max number of file descriptor is %d\n", MAX_FDS);
    }
    else
    {
        fprintf(stderr, "Failed to query maximum file descriptor; "
                "falling back to maxfiles\n");
    }

    close(next_fd);

    if ((mapping = calloc(MAX_FDS, sizeof(struct file_mapping))) == NULL)
    {
        fprintf(stderr, "Failed to allocate file mappings structures\n");
        
        exit(1);
    }
    for(i = 0; i < MAX_FDS; i++)
    {
        pthread_mutex_init(&(mapping[i].file_unit_lock), NULL);
    }
}
*/

static void maxconns_handler(const int fd, const short which, void *arg)
{
    struct timeval t = {.tv_sec = 0, .tv_usec = 10000};

    if (fd == -42 || allow_new_conns == false)
    {
        /* reschedule in 10ms if we need to keep polling */
        evtimer_set(&maxconnsevent, maxconns_handler, 0);
        event_base_set(main_base, &maxconnsevent);
        evtimer_add(&maxconnsevent, &t);
    }
    else
    {
        evtimer_del(&maxconnsevent);
        accept_new_conns(true);
    }
}


static const char *prot_text(enum protocol prot)
{
    char *rv = "unknown";
    switch(prot)
    {
    case ascii_prot:
        rv = "ascii";
        break;
    case binary_prot:
        rv = "binary";
        break;
    case negotiating_prot:
        rv = "auto-negotiate";
        break;
    }
    return rv;
}


/*
 * Initializes the connections array. We don't actually allocate connection
 * structures until they're needed, so as to avoid wasting memory when the
 * maximum connection count is much higher than the actual number of
 * connections.
 *
 * This does end up wasting a few pointers' worth of memory for FDs that are
 * used for things other than connections, but that's worth it in exchange for
 * being able to directly index the conns array by FD.
 */
static void conn_init(void)
{
	/*
    int next_fd = dup(1);
    int headroom = 10;     
    struct rlimit rl;

    MAX_FDS = global_settings.maxconns + headroom + next_fd;

    if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
    {
        MAX_FDS = rl.rlim_max;
    }
    else
    {
        fprintf(stderr, "Failed to query maximum file descriptor; "
                "falling back to maxconns\n");
    }

    close(next_fd);
	*/

    if ((conns = calloc(MAX_FDS, sizeof(conn *))) == NULL)
    {
        fprintf(stderr, "Failed to allocate connection structures\n");
        /* This is unrecoverable so bail out early. */
        exit(1);
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

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
            fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("setting O_NONBLOCK");
        close(sfd);
        return -1;
    }
    return sfd;
}



static int server_sockets(int port, enum network_transport transport,
                          FILE *portnumber_file)
{
    if (global_settings.inter == NULL)
    {
        return server_socket(global_settings.inter, port, transport, portnumber_file);
    }
    else   /*if a server has multiple ip address, then bind a afac instance to each (ip, port) pair*/
    {
        /* tokenize them and bind to each one of them.. */
        char *b;
        int ret = 0;
        char *list = strdup(global_settings.inter);

        if (list == NULL)
        {
            fprintf(stderr, "Failed to allocate memory for parsing server interface string\n");
            return 1;
        }
        char *p;
        for (p = strtok_r(list, ";,", &b); p != NULL; p = strtok_r(NULL, ";,", &b))
        {
            int the_port = port;
            char *s = strchr(p, ':');
            if (s != NULL)
            {
                *s = '\0';
                ++s;
                if (!safe_strtol(s, &the_port))
                {
                    fprintf(stderr, "Invalid port number: \"%s\"", s);
                    return 1;
                }
            }
            if (strcmp(p, "*") == 0)
            {
                p = NULL;
            }
            ret |= server_socket(p, the_port, transport, portnumber_file);
        }
        free(list);
        return ret;
    }
}


/**
 * Create a socket and bind it to a specific port number
 * @param interface the interface to bind to
 * @param port the port number to bind to
 * @param transport the transport protocol (TCP / UDP)
 * @param portnumber_file A filepointer to write the port numbers to
 *        when they are successfully added to the list of ports we
 *        listen on.
 */
static int server_socket(const char *interface, int port,
                         enum network_transport transport, FILE *portnumber_file)
{
    int sfd;
    struct linger ling = {0, 0};
    struct addrinfo *ai;
    struct addrinfo *next;
    struct addrinfo hints = { .ai_flags = AI_PASSIVE,
        .ai_family = AF_UNSPEC
    };
    char port_buf[NI_MAXSERV];
    int error;
    int success = 0;
    int flags =1;
    int snd_buf_size = 2 * 1024 * 1024; // sys limit max

    hints.ai_socktype = IS_UDP(transport) ? SOCK_DGRAM : SOCK_STREAM;

    if (port == -1)
    {
        port = 0;
    }
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    error= getaddrinfo(interface, port_buf, &hints, &ai);
    if (error != 0)
    {
        if (error != EAI_SYSTEM)
            fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
        else
            perror("getaddrinfo()");
        return 1;
    }

    for (next= ai; next; next= next->ai_next)
    {
        conn *listen_conn_add;
        if ((sfd = new_socket(next)) == -1)
        {
            /* getaddrinfo can return "junk" addresses,
             * we make sure at least one works before erroring.
             */
            if (errno == EMFILE)
            {
                /* ...unless we're out of fds */
                perror("server_socket");
                exit(EX_OSERR);
            }
            continue;
        }

#ifdef IPV6_V6ONLY
        if (next->ai_family == AF_INET6)
        {
            error = setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY,
                               (char *) &flags, sizeof(flags));
            if (error != 0)
            {
                perror("setsockopt");
                close(sfd);
                continue;
            }
        }
#endif

        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
        if (IS_UDP(transport))
        {
            maximize_sndbuf(sfd);
        }
        else
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
        }

        if (bind(sfd, next->ai_addr, next->ai_addrlen) == -1)
        {
            if (errno != EADDRINUSE)
            {
                perror("bind()");
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            close(sfd);
            continue;
        }
        else
        {
            success++;
            if (!IS_UDP(transport) && listen(sfd, global_settings.backlog) == -1)
            {
                perror("listen()");
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            if (portnumber_file != NULL &&
                    (next->ai_addr->sa_family == AF_INET ||
                     next->ai_addr->sa_family == AF_INET6))
            {
                union
                {
                    struct sockaddr_in in;
                    struct sockaddr_in6 in6;
                } my_sockaddr;
                socklen_t len = sizeof(my_sockaddr);
                if (getsockname(sfd, (struct sockaddr*)&my_sockaddr, &len)==0)
                {
                    if (next->ai_addr->sa_family == AF_INET)
                    {
                        fprintf(portnumber_file, "%s INET: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in.sin_port));
                    }
                    else
                    {
                        fprintf(portnumber_file, "%s INET6: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in6.sin6_port));
                    }
                }
            }
        }

        if (IS_UDP(transport))
        {
            int c;

            for (c = 0; c < global_settings.num_threads_per_udp; c++)
            {

                /* Allocate one UDP file descriptor per worker thread;
                       * this allows "stats conns" to separately list multiple
                       * parallel UDP requests in progress.
                       *
                       * The dispatch code round-robins new connection requests
                       * among threads, so this is guaranteed to assign one
                       * FD to each thread.
                       */
                int per_thread_fd = c ? dup(sfd) : sfd;
                dispatch_conn_new(per_thread_fd, conn_read,
                                  EV_READ | EV_PERSIST,
                                  UDP_READ_BUFFER_SIZE, transport);
            }
        }
        else
        {
            if (!(listen_conn_add = conn_new(sfd, conn_listening,
                                             EV_READ | EV_PERSIST, 1,
                                             transport, main_base)))
            {
                fprintf(stderr, "failed to create listening connection\n");
                exit(EXIT_FAILURE);
            }
            listen_conn_add->next = listen_conn;
            listen_conn = listen_conn_add;
        }
    }

    freeaddrinfo(ai);

    /* Return zero iff we detected no errors in starting up connections */
    return success == 0;
}

/*
 * Frees a connection.
 */
void conn_free(conn *c)
{
    if (c)
    {
        assert(c != NULL);
        assert(c->sfd >= 0 && c->sfd < MAX_FDS);

        /* MEMCACHED_CONN_DESTROY(c); */
        conns[c->sfd] = NULL;
        if (c->hdrbuf)
            free(c->hdrbuf);
        if (c->msglist)
            free(c->msglist);
        if (c->rbuf)
            free(c->rbuf);
        if (c->wbuf)
            free(c->wbuf);
        if (c->ilist)
            free(c->ilist);
        if (c->suffixlist)
            free(c->suffixlist);
        if (c->iov)
            free(c->iov);
        free(c);
    }
}


conn *conn_new(const int sfd, enum conn_states init_state,
               const int event_flags,
               const int read_buffer_size, enum network_transport transport,
               struct event_base *base)
{
    conn *c;

    assert(sfd >= 0 && sfd < MAX_FDS);
    c = conns[sfd];

    if (NULL == c)
    {
        if (!(c = (conn *)calloc(1, sizeof(conn))))
        {
            fprintf(stderr, "Failed to allocate connection object\n");
            return NULL;
        }

        c->rbuf = c->wbuf = 0;
        c->ilist = 0;
        c->suffixlist = 0;
        c->iov = 0;
        c->msglist = 0;
        c->hdrbuf = 0;

        c->rsize = read_buffer_size;
        c->wsize = DATA_BUFFER_SIZE;
        c->isize = ITEM_LIST_INITIAL;
        c->suffixsize = SUFFIX_LIST_INITIAL;
        c->iovsize = IOV_LIST_INITIAL;
        c->msgsize = MSG_LIST_INITIAL;
        c->hdrsize = 0;
        c->need_ntoh = 1;

        c->rbuf = (char *)malloc((size_t)c->rsize);
        c->wbuf = (char *)malloc((size_t)c->wsize);
        c->ilist = (item **)malloc(sizeof(item *) * c->isize);
        c->suffixlist = (char **)malloc(sizeof(char *) * c->suffixsize);
        c->iov = (struct iovec *)malloc(sizeof(struct iovec) * c->iovsize);
        c->msglist = (struct msghdr *)malloc(sizeof(struct msghdr) * c->msgsize);

        c->assemb_buf = NULL;
        c->assemb_curr = NULL;
        c->assemb_size = 0;

        c->frag_num = 0;
        c->frag_lack = -1;

        if (c->rbuf == 0 || c->wbuf == 0 || c->ilist == 0 || c->iov == 0 ||
                c->msglist == 0 || c->suffixlist == 0)
        {
            conn_free(c);
            fprintf(stderr, "Failed to allocate buffers for connection\n");
            return NULL;
        }

        c->sfd = sfd;
        conns[sfd] = c;
    }

    c->transport = transport;
    c->protocol = global_settings.binding_protocol;

    /* unix socket mode doesn't need this, so zeroed out.  but why
       * is this done for every command?  presumably for UDP
       * mode.
       */
    if (!global_settings.socketpath)
    {
        c->request_addr_size = sizeof(c->request_addr);
    }
    else
    {
        c->request_addr_size = 0;
    }

    if (transport == tcp_transport && init_state == conn_new_cmd)
    {
        if (getpeername(sfd, (struct sockaddr *) &c->request_addr,
                        &c->request_addr_size))
        {
            perror("getpeername");
            memset(&c->request_addr, 0, sizeof(c->request_addr));
        }
    }

    if (global_settings.verbose > 1)
    {
        if (init_state == conn_listening)
        {
            fprintf(stderr, "<%d server listening (%s)\n", sfd,
                    prot_text(c->protocol));
        }
        else if (IS_UDP(transport))
        {
            fprintf(stderr, "<%d server listening (udp)\n", sfd);
        }
        else if (c->protocol == negotiating_prot)
        {
            fprintf(stderr, "<%d new auto-negotiating client connection\n",
                    sfd);
        }
        else if (c->protocol == ascii_prot)
        {
            fprintf(stderr, "<%d new ascii client connection.\n", sfd);
        }
        else if (c->protocol == binary_prot)
        {
            fprintf(stderr, "<%d new binary client connection.\n", sfd);
        }
        else
        {
            fprintf(stderr, "<%d new unknown (%d) client connection\n",
                    sfd, c->protocol);
            assert(false);
        }
    }

    c->state = init_state;
    c->rlbytes = 0;
    c->cmd = -1;
    c->rbytes = c->wbytes = 0;
    c->wcurr = c->wbuf;
    c->rcurr = c->rbuf;
    c->ritem = 0;
    c->icurr = c->ilist;
    c->suffixcurr = c->suffixlist;
    c->ileft = 0;
    c->suffixleft = 0;
    c->iovused = 0;
    c->msgcurr = 0;
    c->msgused = 0;
    c->authenticated = false;

    c->write_and_go = init_state;
    c->write_and_free = 0;
    c->item = 0;

    c->noreply = false;

    event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = event_flags;

    if (event_add(&c->event, 0) == -1)
    {
        perror("event_add");
        return NULL;
    }


    return c;
}

/**
 * Convert a state name to a human readable form.
 */
static const char *state_text(enum conn_states state)
{
    const char* const statenames[] = { "conn_listening",
                                       "conn_new_cmd",
                                       "conn_waiting",
                                       "conn_read",
                                       "conn_parse_cmd",
                                       "conn_write",
                                       "conn_nread",
                                       "conn_swallow",
                                       "conn_closing",
                                       "conn_mwrite",
                                       "conn_closed"
                                     };
    return statenames[state];
}



/*
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
static void conn_set_state(conn *c, enum conn_states state)
{
    assert(c != NULL);
    assert(state >= conn_listening && state < conn_max_state);

    if (state != c->state)
    {
        if (global_settings.verbose > 2)
        {
            fprintf(stderr, "%d: going from %s to %s\n",
                    c->sfd, state_text(c->state),
                    state_text(state));
        }

        c->state = state;
    }
}


/*
 * Shrinks a connection's buffers if they're too big.  This prevents
 * periodic large "get" requests from permanently chewing lots of server
 * memory.
 *
 * This should only be called in between requests since it can wipe output
 * buffers!
 */
static void conn_shrink(conn *c)
{
    assert(c != NULL);

    if (IS_UDP(c->transport))
        return;

    if (c->rsize > READ_BUFFER_HIGHWAT && c->rbytes < DATA_BUFFER_SIZE)
    {
        char *newbuf;

        if (c->rcurr != c->rbuf)
            memmove(c->rbuf, c->rcurr, (size_t)c->rbytes);

        newbuf = (char *)realloc((void *)c->rbuf, DATA_BUFFER_SIZE);

        if (newbuf)
        {
            c->rbuf = newbuf;
            c->rsize = DATA_BUFFER_SIZE;
        }
        /* TODO check other branch... */
        c->rcurr = c->rbuf;
    }

    if (c->isize > ITEM_LIST_HIGHWAT)
    {
        item **newbuf = (item**) realloc((void *)c->ilist, ITEM_LIST_INITIAL * sizeof(c->ilist[0]));
        if (newbuf)
        {
            c->ilist = newbuf;
            c->isize = ITEM_LIST_INITIAL;
        }
        /* TODO check error condition? */
    }

    if (c->msgsize > MSG_LIST_HIGHWAT)
    {
        struct msghdr *newbuf = (struct msghdr *) realloc((void *)c->msglist, MSG_LIST_INITIAL * sizeof(c->msglist[0]));
        if (newbuf)
        {
            c->msglist = newbuf;
            c->msgsize = MSG_LIST_INITIAL;
        }
        /* TODO check error condition? */
    }

    if (c->iovsize > IOV_LIST_HIGHWAT)
    {
        struct iovec *newbuf = (struct iovec *) realloc((void *)c->iov, IOV_LIST_INITIAL * sizeof(c->iov[0]));
        if (newbuf)
        {
            c->iov = newbuf;
            c->iovsize = IOV_LIST_INITIAL;
        }
        /* TODO check return value */
    }
}

static void conn_release_items(conn *c)
{
    assert(c != NULL);

    if (c->item)
    {
        item_remove(c->item);
        c->item = 0;
    }

    while (c->ileft > 0)
    {
        item *it = *(c->icurr);
        assert((it->it_flags & ITEM_SLABBED) == 0);
        item_remove(it);
        c->icurr++;
        c->ileft--;
    }

    if (c->suffixleft != 0)
    {
        /*
        for (; c->suffixleft > 0; c->suffixleft--, c->suffixcurr++) {
                   cache_free(c->thread->suffix_cache, *(c->suffixcurr));
               }
        */
        printf("c->suffixleft != 0\n");
    }

    c->icurr = c->ilist;
    c->suffixcurr = c->suffixlist;
}


static void conn_cleanup(conn *c)
{
    assert(c != NULL);

    conn_release_items(c);

    if (c->write_and_free)
    {
        free(c->write_and_free);
        c->write_and_free = 0;
    }


    if (IS_UDP(c->transport))
    {
        conn_set_state(c, conn_read);
    }
}

static void conn_close(conn *c)
{
    assert(c != NULL);

    /* delete the event, the socket and the conn */
    event_del(&c->event);

    if (global_settings.verbose > 1)
        fprintf(stderr, "<%d connection closed.\n", c->sfd);

    conn_cleanup(c);

    /*    MEMCACHED_CONN_RELEASE(c->sfd); */
    close(c->sfd);
    conn_set_state(c, conn_closed);

    pthread_mutex_lock(&conn_lock);
    allow_new_conns = true;
    pthread_mutex_unlock(&conn_lock);

    STATS_LOCK();
    stats.curr_conns--;
    STATS_UNLOCK();

    return;
}


static void reset_cmd_handler(conn *c)
{
    c->cmd = -1;
    c->substate = bin_no_state;
    if(c->item != NULL)
    {
        item_remove(c->item);
        c->item = NULL;
    }
    conn_shrink(c);
    if (c->rbytes > 0)
    {
        conn_set_state(c, conn_parse_cmd);
    }
    else
    {
        conn_set_state(c, conn_waiting);
    }
}


/*
 * Sets a socket's send buffer size to the maximum allowed by the system.
 */
static void maximize_sndbuf(const int sfd)
{
    socklen_t intsize = sizeof(int);
    int last_good = 0;
    int min, max, avg;
    int old_size;

    /* Start with the default size. */
    if (getsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &old_size, &intsize) != 0)
    {
        if (global_settings.verbose > 0)
            perror("getsockopt(SO_SNDBUF)");
        return;
    }

    /* Binary-search for the real maximum. */
    min = old_size;
    max = MAX_SENDBUF_SIZE;

    while (min <= max)
    {
        avg = ((unsigned int)(min + max)) / 2;
        if (setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&avg, intsize) == 0)
        {
            last_good = avg;
            min = avg + 1;
        }
        else
        {
            max = avg - 1;
        }
    }

    if (global_settings.verbose > 1)
        fprintf(stderr, "<%d send buffer was %d, now %d\n", sfd, old_size, last_good);
}

static bool update_event(conn *c, const int new_flags)
{
    assert(c != NULL);

    struct event_base *base = c->event.ev_base;
    if (c->ev_flags == new_flags)
        return true;
    if (event_del(&c->event) == -1) return false;
    event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;
    if (event_add(&c->event, 0) == -1) return false;
    return true;
}

// this is the event_handler of each thread
// it moniters all the conns it's responsible of
// so all the msgs in one conns would be handled only by the designated thread
// and, when the thread is locked to wait some mutex, it can't handle new msg that would unlock this mutex, which causes deadlock
void event_handler(const int fd, const short which, void *arg)
{
    conn *c;

    c = (conn *)arg;
    assert(c != NULL);

    c->which = which;

    /* sanity */
    if (fd != c->sfd)
    {
        if (global_settings.verbose > 0)
            fprintf(stderr, "Catastrophic: event fd doesn't match conn fd!\n");
        conn_close(c);
        return;
    }

    drive_machine(c);

    /* wait for next event */
    return;
}


/*
 * Sets whether we are listening for new connections or not.
 */
void do_accept_new_conns(const bool do_accept)
{
    conn *next;

    for (next = listen_conn; next; next = next->next)
    {
        if (do_accept)
        {
            update_event(next, EV_READ | EV_PERSIST);
            if (listen(next->sfd, global_settings.backlog) != 0)
            {
                perror("listen");
            }
        }
        else
        {
            update_event(next, 0);
            if (listen(next->sfd, 0) != 0)
            {
                perror("listen");
            }
        }
    }

    if (do_accept)
    {
        STATS_LOCK();
        stats.accepting_conns = true;
        STATS_UNLOCK();
    }
    else
    {
        STATS_LOCK();
        stats.accepting_conns = false;
        stats.listen_disabled_num++;
        STATS_UNLOCK();
        allow_new_conns = false;
        maxconns_handler(-42, 0, 0);
    }
}


/*
 * Constructs a set of UDP headers and attaches them to the outgoing messages.
 */
static int build_udp_headers(conn *c)
{
    int i;
    unsigned char *hdr;

    assert(c != NULL);

    if (c->msgused > c->hdrsize)
    {
        void *new_hdrbuf;
        if (c->hdrbuf)
        {
            new_hdrbuf = realloc(c->hdrbuf, c->msgused * 2 * UDP_HEADER_SIZE);
        }
        else
        {
            new_hdrbuf = malloc(c->msgused * 2 * UDP_HEADER_SIZE);
        }

        if (! new_hdrbuf)
        {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
        c->hdrbuf = (unsigned char *)new_hdrbuf;
        c->hdrsize = c->msgused * 2;
    }

    hdr = c->hdrbuf;
    for (i = 0; i < c->msgused; i++)
    {
        c->msglist[i].msg_iov[0].iov_base = (void*)hdr;
        c->msglist[i].msg_iov[0].iov_len = UDP_HEADER_SIZE;
        *hdr++ = c->request_id / 256;
        *hdr++ = c->request_id % 256;
        *hdr++ = i / 256;
        *hdr++ = i % 256;
        *hdr++ = c->msgused / 256;
        *hdr++ = c->msgused % 256;
        *hdr++ = 0;
        *hdr++ = 0;
        assert((void *) hdr == (caddr_t)c->msglist[i].msg_iov[0].iov_base + UDP_HEADER_SIZE);
    }

    return 0;
}


/*
 * Ensures that there is room for another struct iovec in a connection's
 * iov list.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int ensure_iov_space(conn *c)
{
    assert(c != NULL);

    if (c->iovused >= c->iovsize)
    {
        int i, iovnum;
        struct iovec *new_iov = (struct iovec *)realloc(c->iov,
                                (c->iovsize * 2) * sizeof(struct iovec));
        if (! new_iov)
        {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
        c->iov = new_iov;
        c->iovsize *= 2;

        /* Point all the msghdr structures at the new list. */
        for (i = 0, iovnum = 0; i < c->msgused; i++)
        {
            c->msglist[i].msg_iov = &c->iov[iovnum];
            iovnum += c->msglist[i].msg_iovlen;
        }
    }

    return 0;
}


/*
* Adds data to the list of pending data that will be be written out to a connection.
*
* Returns 0 on success, -1 on out-of-memory
*/
static int add_iov(conn *c, const void *buf, int len)
{
    assert(c != NULL);
    struct msghdr *m;
    int leftover;
    bool limit_to_mtu;

    do
    {
        m = &c->msglist[c->msgused - 1];

        /*
        	*  Limit UDP packets, and the first payloads of TCP replies, to
         	*  UDP_MAX_PAYLOAD_SIZE bytes.
        	*/
        limit_to_mtu = IS_UDP(c->transport) || (1 == c->msgused);

        if(m->msg_iovlen == IOV_MAX ||
                (limit_to_mtu && c->msgbytes >= UDP_MAX_PAYLOAD_SIZE))
        {
            add_msghdr(c);
            m = &c->msglist[c->msgused - 1];
        }

        if(ensure_iov_space(c) != 0)
            return -1;

        if(limit_to_mtu && len + c->msgbytes > UDP_MAX_PAYLOAD_SIZE)
        {
            leftover = len + c->msgbytes - UDP_MAX_PAYLOAD_SIZE;
            len -= leftover;
        }
        else
        {
            leftover = 0;
        }

        m = &c->msglist[c->msgused - 1];
        m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
        m->msg_iov[m->msg_iovlen].iov_len = len;

        m->msg_iovlen++;
        c->msgbytes += len;
        c->iovused++;

        buf = ((char*)buf) + len;
        len = leftover;

    }
    while(leftover != 0);

    return 0;
}


/* Add a message header to a connection
 *
 * Returns 0 on success, -1 on out-of-memory
 *
 */
static int add_msghdr(conn *c)
{
    assert(c != NULL);
    struct msghdr *msg;
    if(c->msgsize == c->msgused)
    {
        msg = realloc(c->msglist, c->msgsize * 2 * sizeof(struct msghdr));
        if(!msg)
        {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
        c->msglist = msg;
        c->msgsize *= 2;

    }

    msg = c->msglist + c->msgused;
    memset(msg, 0, sizeof(struct msghdr));
    msg->msg_iov = &c->iov[c->iovused];

    if(IS_UDP(c->transport) && c->request_addr_size > 0)
    {
        msg->msg_name = &c->request_addr;
        msg->msg_namelen = c->request_addr_size;
    }
    c->msgbytes = 0;
    c->msgused++;

    if(IS_UDP(c->transport))
    {
        /* Leave room for the UDP header, which we'll fill in later. */
        return add_iov(c, NULL, UDP_HEADER_SIZE);
    }

    return 0;

}


static void add_bin_header(conn *c, uint16_t err, uint8_t para_num, uint64_t body_len,
                           uint8_t response_id,	uint8_t is_frag, uint8_t frag_id, uint64_t response_len, uint64_t para_len,
                           uint64_t para1_len, uint64_t para2_len, uint64_t para3_len, uint64_t para4_len, uint64_t para5_len,
                           uint64_t para6_len, uint64_t frag_offset)
{

    protocol_binary_response_header* header;
    assert(c);

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0)
    {
        /* This should never run out of memory because iov and msg lists
            * have minimum sizes big enough to hold an error response.
            */
        out_of_memory(c, "SERVER_ERROR out of memory adding binary header");
        return;
    }

    header = (protocol_binary_response_header *)c->wbuf;

    header->response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    header->response.opcode = c->binary_header.request.opcode;
    header->response.isfrag = is_frag;
    header->response.frag_id = frag_id;
    header->response.response_id = response_id;
    header->response.para_num = para_num;
    header->response.reserved = 0;
    header->response.body_len = htonll(body_len);
    header->response.response_len = htonll(response_len);
    header->response.para_len = htonll(para_len);
    header->response.para1_len = htonll(para1_len);
    header->response.para2_len = htonll(para2_len);
    header->response.para3_len = htonll(para3_len);
    header->response.para4_len = htonll(para4_len);
    header->response.para5_len = htonll(para5_len);
    header->response.para6_len = htonll(para6_len);
    header->response.frag_offset = htonll(frag_offset);


    if(0)
        //if (global_settings.verbose > 1)
    {
        int ii;
        fprintf(stderr, ">%d Writing bin response:", c->sfd);
        for (ii = 0; ii < sizeof(header->bytes); ++ii)
        {
            if (ii % 4 == 0)
            {
                fprintf(stderr, "\n>%d  ", c->sfd);
            }
            fprintf(stderr, " 0x%02x", header->bytes[ii]);
        }
        fprintf(stderr, "\n");
    }


    add_iov(c, c->wbuf, sizeof(header->response));
}


static void add_jay_bin_header(conn *c, uint16_t err, uint8_t para_num, uint64_t body_len,
                               uint8_t response_id,	uint8_t is_frag, uint8_t frag_id, uint64_t response_len, uint64_t para_len,
                               uint64_t para1_len, uint64_t para2_len, uint64_t para3_len, uint64_t para4_len, uint64_t para5_len,
                               uint64_t para6_len, uint64_t frag_offset, uint8_t op_code)
{

    protocol_binary_response_header* header;
    assert(c);

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0)
    {
        /* This should never run out of memory because iov and msg lists
            * have minimum sizes big enough to hold an error response.
            */
        out_of_memory(c, "SERVER_ERROR out of memory adding binary header");
        return;
    }

    header = (protocol_binary_response_header *)c->wbuf;

    header->response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    header->response.opcode = op_code;
    header->response.isfrag = is_frag;
    header->response.frag_id = frag_id;
    header->response.response_id = response_id;
    header->response.para_num = para_num;
    header->response.reserved = 0;
    header->response.body_len = htonll(body_len);
    header->response.response_len = htonll(response_len);
    header->response.para_len = htonll(para_len);
    header->response.para1_len = htonll(para1_len);
    header->response.para2_len = htonll(para2_len);
    header->response.para3_len = htonll(para3_len);
    header->response.para4_len = htonll(para4_len);
    header->response.para5_len = htonll(para5_len);
    header->response.para6_len = htonll(para6_len);
    header->response.frag_offset = htonll(frag_offset);

    if(0)
        //if (global_settings.verbose > 1)
    {
        int ii;
        fprintf(stderr, ">%d Writing bin response:", c->sfd);
        for (ii = 0; ii < sizeof(header->bytes); ++ii)
        {
            if (ii % 4 == 0)
            {
                fprintf(stderr, "\n>%d  ", c->sfd);
            }
            fprintf(stderr, " 0x%02x", header->bytes[ii]);
        }
        fprintf(stderr, "\n");
    }

    add_iov(c, c->wbuf, sizeof(header->response));
}



/**
 * Writes a binary error response. If errstr is supplied, it is used as the
 * error text; otherwise a generic description of the error status code is
 * included.
 */
static void write_bin_error(conn *c, protocol_binary_response_status err,
                            const char *errstr, int swallow)
{
    size_t len;

    if (!errstr)
    {
        switch (err)
        {
        case PROTOCOL_BINARY_RESPONSE_ENOMEM:
            errstr = "Out of memory";
            break;
        case PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND:
            errstr = "Unknown command";
            break;
        case PROTOCOL_BINARY_RESPONSE_EINVAL:
            errstr = "Invalid arguments";
            break;
        default:
            assert(false);
            errstr = "UNHANDLED ERROR";
            fprintf(stderr, ">%d UNHANDLED ERROR: %d\n", c->sfd, err);
        }
    }

    if (global_settings.verbose > 1)
    {
        fprintf(stderr, ">%d Writing an error: %s\n", c->sfd, errstr);
    }

    len = strlen(errstr);
    add_bin_header(c, err, 0, len, (len + sizeof(protocol_binary_response_header))%256, 0, 1,
                   len + sizeof(protocol_binary_response_header), 0, 0, 0, 0, 0, 0, 0, 0);
    if (len > 0)
    {
        add_iov(c, errstr, len);
    }
    conn_set_state(c, conn_mwrite);
    if(swallow > 0)
    {
        c->sbytes = swallow;
        c->write_and_go = conn_swallow;
    }
    else
    {
        c->write_and_go = conn_new_cmd;
    }
}


/* Form and send a response to a command over the binary protocol */
static void write_bin_response(conn *c, void *d, uint16_t err, uint8_t para_num, uint64_t body_len,
                               uint8_t response_id, uint8_t is_frag, uint8_t frag_id, uint64_t response_len, uint64_t para_len,
                               uint64_t para1_len, uint64_t para2_len, uint64_t para3_len, uint64_t para4_len, uint64_t para5_len,
                               uint64_t para6_len, uint64_t frag_offset)
{

    add_bin_header(c, err, para_num, body_len, response_id, is_frag, frag_id, response_len, para_len,
                   para1_len, para2_len, para3_len, para4_len, para5_len, para6_len, frag_offset);
    if(body_len > 0)
    {
        add_iov(c, d, body_len);
    }
    conn_set_state(c, conn_mwrite);
    c->write_and_go = conn_new_cmd;
}


static void write_jay_bin_response(conn *c, void *d, uint16_t err, uint8_t para_num, uint64_t body_len,
                                   uint8_t response_id, uint8_t is_frag, uint8_t frag_id, uint64_t response_len, uint64_t para_len,
                                   uint64_t para1_len, uint64_t para2_len, uint64_t para3_len, uint64_t para4_len, uint64_t para5_len,
                                   uint64_t para6_len, uint64_t frag_offset,
                                   uint8_t op_code)
{

    add_jay_bin_header(c, err, para_num, body_len, response_id, is_frag, frag_id, response_len, para_len,
                       para1_len, para2_len, para3_len, para4_len, para5_len, para6_len, frag_offset, op_code);
    if(body_len > 0)
    {
        add_iov(c, d, body_len);
    }
    conn_set_state(c, conn_mwrite);
    c->write_and_go = conn_new_cmd;
}


static void out_string(conn *c, const char *str)
{
    size_t len;

    assert(c != NULL);

    if (c->noreply)
    {
        if (global_settings.verbose > 1)
            fprintf(stderr, ">%d NOREPLY %s\n", c->sfd, str);
        c->noreply = false;
        conn_set_state(c, conn_new_cmd);
        return;
    }

    if (global_settings.verbose > 1)
        fprintf(stderr, ">%d %s\n", c->sfd, str);

    /* Nuke a partial output... */
    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    add_msghdr(c);

    len = strlen(str);
    if ((len + 2) > c->wsize)
    {
        /* ought to be always enough. just fail for simplicity */
        str = "SERVER_ERROR output line too long";
        len = strlen(str);
    }

    memcpy(c->wbuf, str, len);
    memcpy(c->wbuf + len, "\r\n", 2);
    c->wbytes = len + 2;
    c->wcurr = c->wbuf;

    conn_set_state(c, conn_write);
    c->write_and_go = conn_new_cmd;
    return;
}



/*
 * Outputs a protocol-specific "out of memory" error. For ASCII clients,
 * this is equivalent to out_string().
 */
static void out_of_memory(conn *c, char *ascii_error)
{

    const static char error_prefix[] = "SERVER_ERROR ";
    const static int error_prefix_len = sizeof(error_prefix) - 1;

    if (c->protocol == binary_prot)
    {
        /* Strip off the generic error prefix; it's irrelevant in binary */
        if (!strncmp(ascii_error, error_prefix, error_prefix_len))
        {
            ascii_error += error_prefix_len;
        }
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, ascii_error, 0);
    }
    else
    {
        out_string(c, ascii_error);
    }
}



static void write_io_trace(conn *c)
{
    assert(c != NULL);
    if(c->thread->file_trace_m.count < c->thread->file_trace_m.max)
    {
        memcpy(&(c->thread->file_trace_m.file_trace[c->thread->file_trace_m.count]),
               &(c->request_trace),sizeof(c->request_trace));
        c->thread->file_trace_m.count++;
    }
    else
    {
        write_io_trace_to_file(c->thread, trace_file);
        write_io_trace(c);
    }
}

static void write_io_trace_to_file(LIBEVENT_THREAD * curr_thread, FILE *t_file)
{
    assert(curr_thread != NULL);
    if(t_file == NULL)
    {
        printf("File for trace did not opened success\n");
        return;
    }
    else
    {
        fwrite(curr_thread->file_trace_m.file_trace, sizeof(struct io_trace),
               curr_thread->file_trace_m.count, t_file);
        memset(curr_thread->file_trace_m.file_trace, 0,
               curr_thread->file_trace_m.count * sizeof(struct io_trace));
        curr_thread->file_trace_m.count = 0;
    }

}

static void pull_rdma_data(rdma_mem_block *mem_blk, block_data_req *blk_req, rdma_data_req *rdma_req)
{
    struct timeval pull_start_time, pull_end_time;
    if(DEBUG >= 2)	gettimeofday(&pull_start_time, NULL);
	
    glex_ret_t glex_ret;
    struct glex_rdma_req glex_req, *bad_glex_req;
    uint32_t cqe_num;
    struct glex_cqe cqe[4];

    glex_req.local_mh.v 	= mem_blk->endpoint->mh.v;
    glex_req.local_offset	= mem_blk->mem - mem_blk->endpoint->ep_mem + blk_req->start_offset;
    glex_req.len			= rdma_req->len;
    glex_req.rmt_mh.v		= rdma_req->mh.v;
    glex_req.rmt_offset 	= rdma_req->offset;
    glex_req.type			= GLEX_RDMA_TYPE_READ; 	/* GLEX_RDMA_TYPE_READ */
    //glex_req.rmt_key		= mem_blk->endpoint->ep_attr.key;
    glex_req.rmt_key		= 0x66;
    glex_req.flag			= GLEX_FLAG_SIGNALED;
    glex_req.next			= NULL;

    pthread_mutex_lock(&mem_blk->endpoint->trans_mutex);
    while ((glex_ret = glex_rdma(mem_blk->endpoint->ep, rdma_req->ep_addr, &glex_req, &bad_glex_req)) == GLEX_BUSY) {	}
    if (glex_ret != GLEX_SUCCESS)
    {
        fprintf(stderr,"error: _rdma(), return: %d\n", glex_ret);
        exit(1);
    }

    do
    {
        cqe_num = 1;
        glex_poll_cq(mem_blk->endpoint->ep, &cqe_num, cqe);
    }
    while (cqe_num < 1);
    pthread_mutex_unlock(&mem_blk->endpoint->trans_mutex);

	if(cqe[0].status != GLEX_CQE_STATUS_SUCCESS)
	{
		fprintf(stderr, "RDMA WARNING: wrong cq status!\n");
	}

	//fprintf(stderr, "cqe_num:%d\t status:%d\t opcode:%d\t signaled:%d\n", cqe_num, cqe[0].status, cqe[0].opcode, cqe[0].signaled);

	
	if(DEBUG >= 2) gettimeofday(&pull_end_time, NULL);
	   if(DEBUG >= 2) fprintf(stderr, "~~~ rdma time: %lu\n", 1000000 * ( pull_end_time.tv_sec - pull_start_time.tv_sec ) + pull_end_time.tv_usec - pull_start_time.tv_usec);
}

static void handle_open(protocol_binary_request_header *req, conn* c)
{
	
    assert(req->request.para_num == 2 || req->request.para_num == 3);
    int buf_offset = 0;
    buf_offset += sizeof(protocol_binary_request_header);

    char path[PATH_MAX];
    snprintf(path, req->request.para1_len, (char*)req + buf_offset);
    buf_offset += req->request.para1_len;

    int open_flags = 0;
    int open_mode = 0;
    memcpy(&open_flags, (char*)req + buf_offset, req->request.para2_len);
    buf_offset += req->request.para2_len;

    if(req->request.para_num == 3)
    {
        memcpy(&open_mode, (char*)req + buf_offset, req->request.para3_len);
        buf_offset += req->request.para3_len;
    }

    int mfd;
    if(req->request.para_num == 3)
        mfd = assign_mfd(3, path, open_flags, open_mode);
    else
        mfd = assign_mfd(2, path, open_flags, 0);




    // change the fd from MDS to unified cache fd
    int cfd = MFD_TO_CFD(MY_SERVER_ID, mfd);
    c->write_and_free = malloc(sizeof(int));
    memcpy(c->write_and_free, &cfd, sizeof(int));
    write_bin_response(c, c->write_and_free, 0, 1, sizeof(cfd), 0, 1, 1, sizeof(cfd), sizeof(cfd), sizeof(cfd), 0, 0, 0, 0, 0, 0);

	Ost_File *new_ost_file;
    // see if the file is opened in this OST
    //pthread_mutex_lock(&ost_file_table_mutex);
    if(check_if_ost_file_exists(cfd))	// exists
    {
    	//pthread_mutex_unlock(&ost_file_table_mutex);
    }
    else								// not exists
    {
            new_ost_file = calloc(1, sizeof(Ost_File));
            init_ost_file(new_ost_file);
            memcpy(new_ost_file->path, path, PATH_MAX);
            if(req->request.para_num == 2)
                new_ost_file->real_fd = open(path, open_flags);
            else if(req->request.para_num == 3)
                new_ost_file->real_fd = open(path, open_flags, open_mode);
            assert(new_ost_file->real_fd > 0);
			new_ost_file->size = lseek(new_ost_file->real_fd, 0, SEEK_END);
            insert_ost_file(cfd, new_ost_file);
    	//pthread_mutex_unlock(&ost_file_table_mutex);
    }


    if(DEBUG)
        fprintf(stderr, "OPEN FILE:\t mfd[%d]\n", mfd);
}

static void handle_close(protocol_binary_request_header *req, conn* c)
{
    assert(req->request.para_num == 1);
    int buf_offset = 0;
    buf_offset += sizeof(protocol_binary_request_header);
    int cfd = *(int *)((char *)req + buf_offset);


    // TODO: need notify OSTs to close the file!

    // TODO: need write back data?


    int mfd = CFD_TO_MFD(cfd);

    // TODO: temporaly not free this mfd before perfect this routine
    //free_mfd(mfd);

    // if successed return 0, if not return -1;
    int ret = 0;
    c->write_and_free = malloc(sizeof(ret));
    memcpy(c->write_and_free, &ret, sizeof(ret));
    write_bin_response(c, c->write_and_free, 0, 1, sizeof(ret), 0, 1, 1,
                       sizeof(ret), sizeof(ret), sizeof(ret), 0, 0, 0, 0, 0, 0);

    if(DEBUG)
        fprintf(stderr, "CLOSE FILE:\t mfd[%d]\n", mfd);
}

// delete file
static void handle_unlink(protocol_binary_request_header *req, conn* c)
{
}

static void handle_lseek(protocol_binary_request_header *req, conn* c)
{
    assert(req->request.para_num == 4 || req->request.para_num == 5);
    int buf_offset = 0;
    buf_offset += sizeof(protocol_binary_request_header);

    int cfd = *((int *)((char *)req + buf_offset));
    assert(req->request.para1_len == sizeof(int));
    buf_offset += req->request.para1_len;

    off64_t seek_offset = *((off64_t *)((char *)req + buf_offset));
    assert(req->request.para2_len == sizeof(off64_t));
    buf_offset += req->request.para2_len;

    int whence = *((int *)((char *)req + buf_offset));
    assert(req->request.para3_len == sizeof(int));
    buf_offset += req->request.para3_len;

    long curr_offset = *((long *)((char *)req + buf_offset));
    assert(req->request.para4_len == sizeof(long));
    buf_offset += req->request.para4_len;

    Ost_File *ost_file = NULL;
    // see if the file is opened in this OST
    if(check_if_ost_file_exists(cfd))	// exists
    {
        ost_file = get_ost_file(cfd);
    }
    else								// not exists
    {
        if(req->request.para_num == 5)
        {
            ost_open_arg *open_arg;
            assert(req->request.para5_len == sizeof(ost_open_arg));
            open_arg = (ost_open_arg *)((char*)req + buf_offset);
            buf_offset += req->request.para5_len;

            Ost_File *new_ost_file = calloc(1, sizeof(Ost_File));
            init_ost_file(new_ost_file);
            memcpy(new_ost_file->path, open_arg->path, PATH_MAX);
            if(open_arg->arg_num == 2)
                new_ost_file->real_fd = open(open_arg->path, open_arg->flags);
            else if(open_arg->arg_num == 3)
                new_ost_file->real_fd = open(open_arg->path, open_arg->flags, open_arg->modes);
            assert(new_ost_file->real_fd > 0);
			new_ost_file->size = lseek(new_ost_file->real_fd, 0, SEEK_END);
            insert_ost_file(cfd, new_ost_file);

            ost_file = new_ost_file;
        }

        // let the sender resend the req with file path
        else
        {
            if(DEBUG)
                fprintf(stderr, "LSEEK:\t\t cfd[%d], NEED RETRY WITH PATH\n", cfd);

            int write_and_free_len = sizeof(int) + sizeof(off64_t) + sizeof(int) + sizeof(long);
            c->write_and_free = calloc(1, write_and_free_len);
            memcpy(c->write_and_free, (char *)req + sizeof(protocol_binary_request_header), write_and_free_len);
            write_jay_bin_response(c, c->write_and_free, 0, 4, write_and_free_len, 0, 1, 1,
                                   write_and_free_len, write_and_free_len,
                                   sizeof(int), sizeof(off64_t), sizeof(int), sizeof(long), 0, 0, 0, PROTOCOL_BINARY_CMD_OST_PATH_REQ);
            // this will go to break;
        }
    }

    if(ost_file != NULL)
    {
        uint64_t seek_offset = 0;
        switch(whence)
        {
        case SEEK_END:
            seek_offset = lseek(ost_file->real_fd, seek_offset, SEEK_END);
            break;
        case SEEK_CUR:
        case SEEK_SET:
            printf("SEEK_CUR and SEEK_SET should be processed in local.\n");
        default:
            printf("illegal whence\n");
            exit(0);
        }
        c->write_and_free = malloc(sizeof(seek_offset));
        memcpy(c->write_and_free, &seek_offset, sizeof(seek_offset));
        write_bin_response(c, c->write_and_free, 0, 1, sizeof(seek_offset), 0, 1, 1,
                           sizeof(seek_offset), sizeof(seek_offset), sizeof(seek_offset), 0, 0, 0, 0, 0, 0);

        if(DEBUG)
            fprintf(stderr, "LSEEK:\t\t cfd[%d]\n", cfd);
    }

}

// in FIOF, this func never exec
static void handle_query_metadata(protocol_binary_request_header *req, conn* c)
{
    // 4 params: mfd, offset, size, MY_SERVER_ID
    assert(req->request.para_num == 5);
    int buf_offset = 0;
    buf_offset += sizeof(protocol_binary_request_header);

    int cfd = *((int *)((char *)req + buf_offset));
    assert(req->request.para1_len == sizeof(int));
    buf_offset += req->request.para1_len;

    size_t size = *((size_t *)((char *)req + buf_offset));
    assert(req->request.para2_len == sizeof(size_t));
    buf_offset += req->request.para2_len;

    long offset = *((long *)((char *)req + buf_offset));
    assert(req->request.para3_len == sizeof(long));
    buf_offset += req->request.para3_len;

    int query_server_id = *((int *)((char *)req + buf_offset));
    assert(req->request.para4_len == sizeof(int));
    buf_offset += req->request.para4_len;

    SuccessiveInfo * successive_info = (SuccessiveInfo *)((char *)req + buf_offset);
    assert(req->request.para5_len == sizeof(SuccessiveInfo));
    buf_offset += req->request.para5_len;

    int mfd = CFD_TO_MFD(cfd);
    if(DEBUG)
        fprintf(stderr, "QUERY MDS:\t srv[%d] query mfd[%d] range(%lld, %lld)\n",
                query_server_id, mfd, offset, size);

    assert(check_if_mfd_is_occupied(mfd));		// just to assure

    // transform the byte offset to block offset
    int start_block_index = offset >> G_BLOCK_SIZE_SHIFT;
    int end_block_index = (offset + size - 1) >> G_BLOCK_SIZE_SHIFT;
    int num_block_index = end_block_index - start_block_index + 1;
    assert(num_block_index >= 1);
    int bytes_block_metadata = sizeof(int) * num_block_index;
    c->write_and_free = calloc(1, bytes_block_metadata + sizeof(bytes_block_metadata));	// to store the metadata of each block and the len

    //struct timeval start_time, end_time;
    //gettimeofday(&start_time, NULL);
    query_block_metadata(mfd, start_block_index, num_block_index, c->write_and_free, query_server_id, successive_info, size);
    //gettimeofday(&end_time, NULL);
    
    memcpy((char *)c->write_and_free + bytes_block_metadata, &bytes_block_metadata, sizeof(bytes_block_metadata));

    write_bin_response(c, c->write_and_free, 0, 2, bytes_block_metadata + sizeof(bytes_block_metadata), 0, 1, 1,
                       bytes_block_metadata + sizeof(bytes_block_metadata), bytes_block_metadata + sizeof(bytes_block_metadata),
                       bytes_block_metadata, sizeof(bytes_block_metadata), 0, 0, 0, 0, 0);


    /*for trace: file_name, client name exc_time*/
    //c->request_trace.exc_time = (end_time.tv_sec - start_time.tv_sec) * 1000000 +
                                //end_time.tv_usec - start_time.tv_usec;

}

// in FIOF, this func never exec
static void handle_ost_prepare_data(protocol_binary_request_header *req, conn* c)
{
    // 4 params: mfd, offset, size, MY_SERVER_ID
    assert(req->request.para_num == 5 || req->request.para_num == 6);
	
    int buf_offset = 0;
    buf_offset += sizeof(protocol_binary_request_header);

    int cfd = *((int *)((char *)req + buf_offset));
    assert(req->request.para1_len == sizeof(int));
    buf_offset += req->request.para1_len;

    size_t size = *((size_t *)((char *)req + buf_offset));
    assert(req->request.para2_len == sizeof(size_t));
    buf_offset += req->request.para2_len;

    long offset = *((long *)((char *)req + buf_offset));
    assert(req->request.para3_len == sizeof(long));
    buf_offset += req->request.para3_len;


    int start_block_index = offset >> G_BLOCK_SIZE_SHIFT;
    int end_block_index = (offset + size - 1) >> G_BLOCK_SIZE_SHIFT;
    int num_block_index = end_block_index - start_block_index + 1;
    assert(num_block_index >= 1);

    int* metadata_array = (int *)((char *)req + buf_offset);
    assert(req->request.para4_len == sizeof(int) * num_block_index);
    buf_offset += sizeof(int) * num_block_index;

    SuccessiveInfo * successive_info = (SuccessiveInfo *)((char *)req + buf_offset);
    assert(req->request.para5_len == sizeof(SuccessiveInfo));
    buf_offset += req->request.para5_len;

	Ost_File *ost_file = NULL;
    // see if the file is opened in this OST
    if(check_if_ost_file_exists(cfd))	// exists
    {
        ost_file = get_ost_file(cfd);
    }
    else								// not exists
    {
        if(req->request.para_num == 6)
        {
            ost_open_arg *open_arg;
            assert(req->request.para6_len == sizeof(ost_open_arg));
            open_arg = (ost_open_arg *)((char*)req + buf_offset);
            buf_offset += req->request.para6_len;

            Ost_File *new_ost_file = calloc(1, sizeof(Ost_File));
            init_ost_file(new_ost_file);
            memcpy(new_ost_file->path, open_arg->path, PATH_MAX);
            if(open_arg->arg_num == 2)
                new_ost_file->real_fd = open(open_arg->path, open_arg->flags);
            else if(open_arg->arg_num == 3)
                new_ost_file->real_fd = open(open_arg->path, open_arg->flags, open_arg->modes);
            assert(new_ost_file->real_fd > 0);
			new_ost_file->size = lseek(new_ost_file->real_fd, 0, SEEK_END);
            insert_ost_file(cfd, new_ost_file);

            ost_file = new_ost_file;
        }

        // let the sender resend the req with file path
        else
        {
            if(DEBUG)
                fprintf(stderr, "PREP OST:\t  NEED RETRY WITH PATH\n");

			int result = 0;
			c->write_and_free = malloc(sizeof(int));
			memcpy(c->write_and_free, &result, sizeof(int));
			write_bin_response(c, c->write_and_free, 0, 1, sizeof(result), 0, 1, 1, sizeof(result), sizeof(result), sizeof(result), 0, 0, 0, 0, 0, 0);

            // this will go to break;
        }
    }

	if(ost_file != NULL)
    {
    	if(DEBUG)  fprintf(stderr, "PREP OST\n");
		
		prepare_for_first_cached_in_data(ost_file, metadata_array, num_block_index, start_block_index, offset, size, successive_info->is_read);

	   	int result = 1;
		c->write_and_free = malloc(sizeof(int));
		memcpy(c->write_and_free, &result, sizeof(int));
		write_bin_response(c, c->write_and_free, 0, 1, sizeof(result), 0, 1, 1, sizeof(result), sizeof(result), sizeof(result), 0, 0, 0, 0, 0, 0);
		   
    } 
    
   
}


static void handle_rdma_read_ost(protocol_binary_request_header *req, conn* c)
{

	
    assert(req->request.para_num ==  1 || req->request.para_num ==  2);
    //if(DEBUG) fprintf(stderr, "thread[%d] handling......\n", pthread_self());

    int buf_offset = 0;
    // normally just 1 param: block_data_req
    buf_offset += sizeof(protocol_binary_request_header);

    assert(req->request.para1_len == sizeof(block_data_req));
    block_data_req* blk_req = (block_data_req*)((char*)req + buf_offset);
    buf_offset += req->request.para1_len;

    Ost_File *ost_file = NULL;
    int file_fd = -1;
    // see if the file is opened in this OST
    if(check_if_ost_file_exists(blk_req->cfd))	// exists
    {
        ost_file = get_ost_file(blk_req->cfd);
        file_fd = ost_file->real_fd;
    }
    else								// not exists
    {
        if(req->request.para_num == 2)
        {
            ost_open_arg *open_arg;
            assert(req->request.para2_len == sizeof(ost_open_arg));
            open_arg = (ost_open_arg *)((char*)req + buf_offset);
            buf_offset += req->request.para2_len;

            Ost_File *new_ost_file = calloc(1, sizeof(Ost_File));
            init_ost_file(new_ost_file);
            memcpy(new_ost_file->path, open_arg->path, PATH_MAX);
            if(open_arg->arg_num == 2)
                new_ost_file->real_fd = open(open_arg->path, open_arg->flags);
            else if(open_arg->arg_num == 3)
                new_ost_file->real_fd = open(open_arg->path, open_arg->flags, open_arg->modes);
            assert(new_ost_file->real_fd > 0);
			new_ost_file->size = lseek(new_ost_file->real_fd, 0, SEEK_END);
            insert_ost_file(blk_req->cfd, new_ost_file);

            ost_file = new_ost_file;
            file_fd = new_ost_file->real_fd;
        }

        // let the sender resend the req with file path
        else
        {
            if(DEBUG)
                fprintf(stderr, "READ OST:\t cfd[%d] block[%d], NEED RETRY WITH PATH\n", blk_req->cfd, blk_req->file_block_id);

            int write_and_free_len = sizeof(block_data_req);
            c->write_and_free = calloc(1, write_and_free_len);
            memcpy(c->write_and_free, blk_req, write_and_free_len);
            write_jay_bin_response(c, c->write_and_free, 0, 1, write_and_free_len, 0, 1, 1,
                                   write_and_free_len, write_and_free_len,
                                   write_and_free_len, 0, 0, 0, 0, 0, 0, PROTOCOL_BINARY_CMD_OST_PATH_REQ);
            // this will go to break;
        }
    }

    if(ost_file != NULL)
    {
        rdma_mem_block *rdma_mem_blk = get_mem_block();
		//fprintf(stderr, "get_mem_block: %#X\n", rdma_mem_blk->mem);

        // no free mem_blk, need to retry
        // TODO: MAY RESULT IN POOR PERFORMANCE!!!!!
        if(rdma_mem_blk == NULL)
        {
            if(DEBUG)
                fprintf(stderr, "READ OST:\t cfd[%d] block[%d], NO FREE MEM BLOCK\n", blk_req->cfd, blk_req->file_block_id);

            int write_and_free_len = sizeof(block_data_req);
            c->write_and_free = calloc(1, write_and_free_len);
            memcpy(c->write_and_free, blk_req, write_and_free_len);
            write_jay_bin_response(c, c->write_and_free, 0, 1, write_and_free_len, 0, 1, 1,
                                   write_and_free_len, write_and_free_len,
                                   write_and_free_len, 0, 0, 0, 0, 0, 0, PROTOCOL_BINARY_CMD_OST_NO_FREE_MEM_BLK);
        }
        else
        {
            if(DEBUG)
                fprintf(stderr, "READ OST:\t cfd[%d] block[%d] %d bytes\n", blk_req->cfd, blk_req->file_block_id, blk_req->end_offset - blk_req->start_offset + 1);

            //struct timeval start_time, end_time;
            //if(DEBUG) gettimeofday(&start_time, NULL);
            read_block(ost_file, rdma_mem_blk->mem, blk_req);
            //if(DEBUG) gettimeofday(&end_time, NULL);
            //if(DEBUG) fprintf(stderr, "~~~~~~~io time: %lu\n", 1000000 * ( end_time.tv_sec - start_time.tv_sec ) + end_time.tv_usec - start_time.tv_usec);

            rdma_data_req rdma_req;
            rdma_req.mh = rdma_mem_blk->endpoint->mh;
            rdma_req.ep_addr = rdma_mem_blk->endpoint->ep_addr;
            rdma_req.offset = blk_req->start_offset + (rdma_mem_blk->mem - rdma_mem_blk->endpoint->ep_mem);
            rdma_req.len = blk_req->end_offset - blk_req->start_offset + 1;
            rdma_req.src_srv_id = MY_SERVER_ID;
            rdma_req.mem_blk_id = rdma_mem_blk->id;

            int write_and_free_len = sizeof(block_data_req) + sizeof(rdma_data_req);
            c->write_and_free = calloc(1, write_and_free_len);
            memcpy(c->write_and_free, blk_req, sizeof(block_data_req));
            memcpy(c->write_and_free + sizeof(block_data_req), &rdma_req, sizeof(rdma_data_req));
            write_bin_response(c, c->write_and_free, 0, 2, write_and_free_len,
                               0, 1, 1,
                               write_and_free_len, write_and_free_len,
                               sizeof(block_data_req), sizeof(rdma_data_req), 0, 0, 0, 0, 0);

			//fprintf(stderr, "mh:%#X\t ep:%#X\t src_id:%d\n", rdma_mem_blk->endpoint->mh.v, rdma_mem_blk->endpoint->ep_addr.v, rdma_req.src_srv_id);
        }
    }
}


static void handle_rdma_read_ost_complt(protocol_binary_request_header *req, conn* c)
{
    int buf_offset = 0;
    buf_offset += sizeof(protocol_binary_request_header);
    assert(req->request.para_num == 1);

    int recv_mem_blk_id;
    assert(req->request.para1_len == sizeof(recv_mem_blk_id));
    recv_mem_blk_id = *(int *)((char*)req + buf_offset);
    buf_offset += req->request.para1_len;

    release_mem_block(recv_mem_blk_id);
    if(DEBUG >= 3) fprintf(stderr, "release mem_blk[%d]\n", recv_mem_blk_id);

    // this need to be ensured!!!
    conn_set_state(c, conn_new_cmd);
}

static void handle_rdma_write_ost(protocol_binary_request_header *req, conn* c)
{
    assert(req->request.para_num ==  2 || req->request.para_num ==  3);
    int buf_offset = 0;
    // normally just 1 param: block_data_req
    buf_offset += sizeof(protocol_binary_request_header);

    assert(req->request.para1_len == sizeof(block_data_req));
    block_data_req* blk_req = (block_data_req*)((char*)req + buf_offset);
    buf_offset += req->request.para1_len;

    assert(req->request.para2_len == sizeof(rdma_data_req));
    rdma_data_req* rdma_req = (rdma_data_req*)((char*)req + buf_offset);
    buf_offset += req->request.para2_len;

    Ost_File *ost_file = NULL;
    int file_fd = -1;
    // see if the file is opened in this OST
    if(check_if_ost_file_exists(blk_req->cfd))	// exists
    {
        ost_file = get_ost_file(blk_req->cfd);
        file_fd = ost_file->real_fd;
    }
    else								// not exists
    {
        if(req->request.para_num == 3)
        {
            ost_open_arg *open_arg;
            assert(req->request.para3_len == sizeof(ost_open_arg));
            open_arg = (ost_open_arg *)((char*)req + buf_offset);
            buf_offset += req->request.para3_len;

            Ost_File *new_ost_file = calloc(1, sizeof(Ost_File));
            init_ost_file(new_ost_file);
            memcpy(new_ost_file->path, open_arg->path, PATH_MAX);
            if(open_arg->arg_num == 2)
                new_ost_file->real_fd = open(open_arg->path, open_arg->flags);
            else if(open_arg->arg_num == 3)
                new_ost_file->real_fd = open(open_arg->path, open_arg->flags, open_arg->modes);
            assert(new_ost_file->real_fd > 0);
			new_ost_file->size = lseek(new_ost_file->real_fd, 0, SEEK_END);
            insert_ost_file(blk_req->cfd, new_ost_file);

            if(DEBUG) fprintf(stderr, "open file to write: %s\n", open_arg->path);

            ost_file = new_ost_file;
            file_fd = new_ost_file->real_fd;
        }

        // let the sender resend the req with file path
        else
        {
            if(DEBUG)
                fprintf(stderr, "WRITE OST:\t cfd[%d] block[%d], NEED RETRY WITH PATH\n", blk_req->cfd, blk_req->file_block_id);

            int write_and_free_len = sizeof(block_data_req) + sizeof(rdma_data_req);
            c->write_and_free = calloc(1, write_and_free_len);
            memcpy(c->write_and_free, blk_req, sizeof(block_data_req));
            memcpy(c->write_and_free + sizeof(block_data_req), rdma_req, sizeof(rdma_data_req));
            write_jay_bin_response(c, c->write_and_free, 0, 2, write_and_free_len,
                                   0, 1, 1,
                                   write_and_free_len, write_and_free_len,
                                   sizeof(block_data_req), sizeof(rdma_data_req), 0, 0, 0, 0, 0, PROTOCOL_BINARY_CMD_OST_PATH_REQ);

            // this will go to break;
        }
    }

    if(ost_file != NULL)
    {

        rdma_mem_block *rdma_mem_blk = get_mem_block();
        // no free mem_blk, need to retry
        // MAY RESULT IN POOR PERFORMANCE!!!!!
        if(rdma_mem_blk == NULL)
        {
            if(DEBUG)
                fprintf(stderr, "WRITE OST:\t cfd[%d] block[%d], NO FREE MEM BLOCK\n", blk_req->cfd, blk_req->file_block_id);

            int write_and_free_len = sizeof(block_data_req) + sizeof(rdma_data_req);
            c->write_and_free = calloc(1, write_and_free_len);
            memcpy(c->write_and_free, blk_req, sizeof(block_data_req));
            memcpy(c->write_and_free + sizeof(block_data_req), rdma_req, sizeof(rdma_data_req));
            write_jay_bin_response(c, c->write_and_free, 0, 2, write_and_free_len,
                                   0, 1, 1,
                                   write_and_free_len, write_and_free_len,
                                   sizeof(block_data_req), sizeof(rdma_data_req), 0, 0, 0, 0, 0, PROTOCOL_BINARY_CMD_OST_NO_FREE_MEM_BLK);
        }
        else
        {
            if(DEBUG)
                fprintf(stderr, "WRITE OST:\t cfd[%d] block[%d] %d bytes\n", blk_req->cfd, blk_req->file_block_id, blk_req->end_offset - blk_req->start_offset + 1);

            // pull data by rdma
            pull_rdma_data(rdma_mem_blk, blk_req, rdma_req);

            //struct timeval io_start_time, io_end_time;
            //if(DEBUG) gettimeofday(&io_start_time, NULL);
            write_block(ost_file, rdma_mem_blk->mem, blk_req);
            //if(DEBUG) gettimeofday(&io_end_time, NULL);
            //if(DEBUG) fprintf(stderr, "~~~ io time: %lu\n", 1000000 * ( io_end_time.tv_sec - io_start_time.tv_sec ) + io_end_time.tv_usec - io_start_time.tv_usec);

            release_mem_block(rdma_mem_blk->id);
			
			update_ost_file_size(ost_file, blk_req->file_block_id * G_BLOCK_SIZE_IN_BYTES + blk_req->start_offset,
									blk_req->end_offset - blk_req->start_offset + 1);

            // ack msg has 3 params: write_len, mem_blk_id, file_block_id
            int write_and_free_len = sizeof(int) + sizeof(int) + sizeof(int);
            c->write_and_free = calloc(1, write_and_free_len);
            memcpy(c->write_and_free, &rdma_req->len, sizeof(int));
            memcpy(c->write_and_free + sizeof(int), &rdma_req->mem_blk_id, sizeof(int));
            memcpy(c->write_and_free + sizeof(int) * 2, &blk_req->file_block_id, sizeof(int));
            write_bin_response(c, c->write_and_free, 0, 3, write_and_free_len,
                               0, 1, 1,
                               write_and_free_len, write_and_free_len,
                               sizeof(int), sizeof(int), sizeof(int), 0, 0, 0, 0);
        }
    }
}

static void handle_rdma_write_ost_complt(protocol_binary_request_header *req, conn* c)
{
    // do nothing
    conn_set_state(c, conn_new_cmd);
    return;
}


static void  jay_complete_nread(conn *c)
{
    assert(c != NULL);
    assert(c->protocol == binary_prot);

	/*
    //FILE *ret;
    //int cfd;
    //int mfd;
    //char path[PATH_MAX];
    //char mode[8];
    //int open_flags = 0;
    //int open_mode = 0;
    //uint32_t hv;
    //struct timeval start, finish;
    //uint64_t offset, length, s_offset;
    //off_t seek_offset;
    //int whence = 0;
    //size_t size, nmemb, return_v;
    //uint64_t buf_offset = 0;
    //uint64_t data_length = 0;
    //int process_id;
    //void *data;
    //file_cache_t* file_head;

    // JAY
    //int query_server_id;
    int start_block_index;
    int end_block_index;
    int num_block_index;
    int bytes_block_metadata;
    uint64_t bytes_to_access;
    uint64_t bytes_already_access;
    struct timeval start_time, end_time;
    int tcp_cork_on;
    int write_and_free_len;

    // READ / WRITE params
    mfd_array_entry *mfd_entry;
    Mds_File *mds_file;

    // READ_OST / WRITE_OST params
    int num_block;
    int *block_array;
    int metadata_srv_id;
    int file_fd;
    Ost_File *ost_file;
    ost_block_offset ost_blk_off;
    char *data_buf = NULL;
    uint64_t data_buf_len = 0;

    // RDMA
    block_data_req *blk_req;
    rdma_mem_block *rdma_mem_blk;
    rdma_data_req rdma_req;
    int recv_mem_blk_id;

    //for param
    int cur_blk;
	*/

    if(c->frag_num == 1)
    {
        protocol_binary_request_header *req;
        req = (protocol_binary_request_header *)c->ritem;
        assert(req->request.opcode == c->cmd);
        switch(c->cmd)
        {
        case PROTOCOL_BINARY_CMD_CREAT:
            break;
        case PROTOCOL_BINARY_CMD_CREAT64:
            break;
        case PROTOCOL_BINARY_CMD_OPEN64:
            break;
        case PROTOCOL_BINARY_CMD_OPEN:
            handle_open(req, c);
            break;
        case PROTOCOL_BINARY_CMD_CLOSE:
            handle_close(req, c);
            break;
        case PROTOCOL_BINARY_CMD_QUERY_MDS:
            handle_query_metadata(req, c);
            break;
        case PROTOCOL_BINARY_CMD_WRITE:
            handle_query_metadata(req, c);
            break;
        case PROTOCOL_BINARY_CMD_READ:
            handle_query_metadata(req, c);
            break;
        case PROTOCOL_BINARY_CMD_LSEEK:
            handle_lseek(req, c);
            break;
        case PROTOCOL_BINARY_CMD_LSEEK64:
            break;
        case PROTOCOL_BINARY_CMD_RDMA_READ_OST:
            handle_rdma_read_ost(req, c);
            break;
        case PROTOCOL_BINARY_CMD_RDMA_READ_OST_COMPLT:
            handle_rdma_read_ost_complt(req, c);
            break;
        case PROTOCOL_BINARY_CMD_RDMA_WRITE_OST_COMPLT:
            handle_rdma_write_ost_complt(req, c);
            break;
        case PROTOCOL_BINARY_CMD_RDMA_WRITE_OST:
            handle_rdma_write_ost(req, c);
            break;
        case PROTOCOL_BINARY_CMD_OST_PREPARE_DATA:
            handle_ost_prepare_data(req, c);
            break;
        case PROTOCOL_BINARY_CMD_READ_OST:
            break;
        case PROTOCOL_BINARY_CMD_WRITE_OST:
            break;
        case PROTOCOL_BINARY_CMD_PREAD:
            break;
        case PROTOCOL_BINARY_CMD_PREAD64:
            break;
        case PROTOCOL_BINARY_CMD_PWRITE:
            break;
        case PROTOCOL_BINARY_CMD_PWRITE64:
            break;
        case PROTOCOL_BINARY_CMD_READV:
            break;
        case PROTOCOL_BINARY_CMD_WRITEV:
            break;
        case PROTOCOL_BINARY_CMD___FXSTAT:
            break;
        case PROTOCOL_BINARY_CMD___FXSTAT64:
            break;
        case PROTOCOL_BINARY_CMD___LXSTAT:
            break;
        case PROTOCOL_BINARY_CMD___LXSTAT64:
            break;
        case PROTOCOL_BINARY_CMD___XSTAT:
            break;
        case PROTOCOL_BINARY_CMD___XSTAT64:
            break;
        case PROTOCOL_BINARY_CMD_MMAP:
            break;
        case PROTOCOL_BINARY_CMD_MMAP64:
            break;
        case PROTOCOL_BINARY_CMD_FOPEN:
            break;
        case PROTOCOL_BINARY_CMD_FOPEN64:
            break;
        case PROTOCOL_BINARY_CMD_FCLOSE:
            break;
        case PROTOCOL_BINARY_CMD_FREAD:
            break;
        case PROTOCOL_BINARY_CMD_FWRITE:
            break;
        case PROTOCOL_BINARY_CMD_FSEEK:
            break;
        case PROTOCOL_BINARY_CMD_FSYNC:
            break;
        case PROTOCOL_BINARY_CMD_FDATASYNC:
            break;
        case PROTOCOL_BINARY_CMD_AIO_READ:
            break;
        case PROTOCOL_BINARY_CMD_AIO_READ64:
            break;
        case PROTOCOL_BINARY_CMD_AIO_WRITE:
            break;
        case PROTOCOL_BINARY_CMD_AIO_WRITE64:
            break;
        case PROTOCOL_BINARY_CMD_LIO_LISTIO:
            break;
        case PROTOCOL_BINARY_CMD_LIO_LISTIO64:
            break;
        case PROTOCOL_BINARY_CMD_AIO_RETURN:
            break;
        case PROTOCOL_BINARY_CMD_AIO_RETURN64:
            break;
        default:
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL, 0);
        }
    }
    if(c->frag_num > 1)
    {

        protocol_binary_request_header *req;
        req = (protocol_binary_request_header *)c->assemb_buf;
        assert(req->request.opcode == c->cmd);
    }


}




static enum try_read_result try_read_udp(conn *c)
{
    int res;

    assert(c != NULL);

    c->request_addr_size = sizeof(c->request_addr);
    res = recvfrom(c->sfd, c->rbuf, c->rsize,
                   0, (struct sockaddr *)&c->request_addr,
                   &c->request_addr_size);
    if (res > 70)
    {
        unsigned char *buf = (unsigned char *)c->rbuf;
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.bytes_read += res;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        /* Beginning of UDP packet is the request ID; save it. */
        c->request_id = buf[0] * 256 + buf[1];

        /* If this is a multi-packet request, drop it. */
        if (buf[4] != 0 || buf[5] != 1)
        {
            out_string(c, "SERVER_ERROR multi-packet request not supported");
            return READ_NO_DATA_RECEIVED;
        }

        /* Don't care about any of the rest of the header. */
        res -= 8;
        memmove(c->rbuf, c->rbuf + 8, res);

        c->rbytes = res;
        c->rcurr = c->rbuf;
        return READ_DATA_RECEIVED;
    }
    return READ_NO_DATA_RECEIVED;
}

/*
 * read from network as much as we can, handle buffer overflow and connection
 * close.
 * before reading, move the remaining incomplete fragment of a command
 * (if any) to the beginning of the buffer.
 *
 * To protect us from someone flooding a connection with bogus data causing
 * the connection to eat up all available memory, break out and start looking
 * at the data I've got after a number of reallocs...
 *
 * @return enum try_read_result
 */

static enum try_read_result try_read_network(conn *c)
{
    enum try_read_result gotdata = READ_NO_DATA_RECEIVED;
    int res;
    int num_allocs = 0;
    assert(c != NULL);

    if (c->rcurr != c->rbuf)
    {
        if (c->rbytes != 0) /* otherwise there's nothing to copy */
            memmove(c->rbuf, c->rcurr, c->rbytes);
        c->rcurr = c->rbuf;
    }

    while (1)
    {
        if (c->rbytes >= c->rsize)
        {
            if (num_allocs == 4)
            {
                return gotdata;
            }
            ++num_allocs;
            char *new_rbuf = realloc(c->rbuf, c->rsize * 2);
            if (!new_rbuf)
            {
                STATS_LOCK();
                stats.malloc_fails++;
                STATS_UNLOCK();
                if (global_settings.verbose > 0)
                {
                    fprintf(stderr, "Couldn't realloc input buffer\n");
                }
                c->rbytes = 0; /* ignore what we read */
                out_of_memory(c, "SERVER_ERROR out of memory reading request");
                c->write_and_go = conn_closing;
                return READ_MEMORY_ERROR;
            }
            c->rcurr = c->rbuf = new_rbuf;
            c->rsize *= 2;
        }

        int avail = c->rsize - c->rbytes;
        res = read(c->sfd, c->rbuf + c->rbytes, avail);
        if (res > 0)
        {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.bytes_read += res;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            gotdata = READ_DATA_RECEIVED;
            c->rbytes += res;
            if (res == avail)
            {
                continue;
            }
            else
            {
                break;
            }
        }
        if (res == 0)
        {
            return READ_ERROR;
        }
        if (res == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            return READ_ERROR;
        }
    }
    return gotdata;
}


/*
 * if we have a complete line in the buffer, process it.
 */
static int try_read_command(conn *c)
{
    assert(c != NULL);
    assert(c->rcurr <= (c->rbuf + c->rsize));
    assert(c->rbytes > 0);

    if (c->protocol == negotiating_prot || c->transport == udp_transport)
    {
        if ((unsigned char)c->rbuf[0] == (unsigned char)PROTOCOL_BINARY_REQ)
        {
            c->protocol = binary_prot;
        }
        else
        {
            c->protocol = ascii_prot;
        }

        if (global_settings.verbose > 1)
        {
            fprintf(stderr, "%d: Client using the %s protocol\n", c->sfd,
                    prot_text(c->protocol));
        }
    }

    if (c->protocol == binary_prot)
    {
        /* Do we have the complete packet header? */
        if (c->rbytes < sizeof(c->binary_header))
        {
            /* need more data! */
            return 0;
        }
        else
        {
#ifdef NEED_ALIGN
            if (((long)(c->rcurr)) % 8 != 0)
            {
                /* must realign input buffer */
                memmove(c->rbuf, c->rcurr, c->rbytes);
                c->rcurr = c->rbuf;
                if (global_settings.verbose > 1)
                {
                    fprintf(stderr, "%d: Realign input buffer\n", c->sfd);
                }
            }
#endif
            protocol_binary_request_header* req;
            req = (protocol_binary_request_header*)c->rcurr;

            if(0)
                //if (global_settings.verbose > 1)
            {
                /* Dump the packet before we convert it to host order */
                int ii;
                fprintf(stderr, "<%d Read binary protocol data:", c->sfd);
                for (ii = 0; ii < sizeof(req->bytes); ++ii)
                {
                    if (ii % 4 == 0)
                    {
                        fprintf(stderr, "\n<%d   ", c->sfd);
                    }
                    fprintf(stderr, " 0x%02x", req->bytes[ii]);
                }
                fprintf(stderr, "\n");
            }

            c->binary_header = *req;
            c->binary_header.request.reserved = ntohs(req->request.reserved);
            c->binary_header.request.body_len = ntohll(req->request.body_len);
            c->binary_header.request.request_len = ntohll(req->request.request_len);
            c->binary_header.request.para_len = ntohll(req->request.para_len);
            c->binary_header.request.para1_len = ntohll(req->request.para1_len);
            c->binary_header.request.para2_len = ntohll(req->request.para2_len);
            c->binary_header.request.para3_len = ntohll(req->request.para3_len);
            c->binary_header.request.para4_len = ntohll(req->request.para4_len);
            c->binary_header.request.para5_len = ntohll(req->request.para5_len);
            c->binary_header.request.para6_len = ntohll(req->request.para6_len);
            c->binary_header.request.frag_offset = ntohll(req->request.frag_offset);

            /*for trace*/
            c->request_trace.opcode = c->binary_header.request.opcode;
            c->request_trace.request_time = time(NULL);
            gethostname(c->request_trace.server_name,sizeof(c->request_trace.server_name));

            if (c->binary_header.request.magic != PROTOCOL_BINARY_REQ)
            {
                if (global_settings.verbose)
                {
                    fprintf(stderr, "Invalid magic:  %x\n",
                            c->binary_header.request.magic);
                }
                conn_set_state(c, conn_closing);
                return -1;
            }

            c->msgcurr = 0;
            c->msgused = 0;
            c->iovused = 0;
            if (add_msghdr(c) != 0)
            {
                out_of_memory(c, "SERVER_ERROR Out of memory allocating headers");
                return 0;
            }

            c->cmd = c->binary_header.request.opcode;
            /* clear the returned cas value */
            c->cas = 0;
            c->body_len = c->binary_header.request.body_len;
            c->r_len = c->binary_header.request.request_len;

            dispatch_bin_command(c);

            /*    c->rbytes -= sizeof(c->binary_header);
                c->rcurr += sizeof(c->binary_header);
                */
        }
    }
    else
    {
        return -1;
    }

    return 1;
}


static void request_network_to_host(conn *c, char* buffer)
{
    assert(c != NULL);


    protocol_binary_request_header* req;
    req = (protocol_binary_request_header*)buffer;

    req->request.reserved = ntohs(req->request.reserved);
    req->request.body_len = ntohll(req->request.body_len);
    req->request.request_len = ntohll(req->request.request_len);
    req->request.para_len = ntohll(req->request.para_len);
    req->request.para1_len = ntohll(req->request.para1_len);
    req->request.para2_len = ntohll(req->request.para2_len);
    req->request.para3_len = ntohll(req->request.para3_len);
    req->request.para4_len = ntohll(req->request.para4_len);
    req->request.para5_len = ntohll(req->request.para5_len);
    req->request.para6_len = ntohll(req->request.para6_len);
    req->request.frag_offset = ntohll(req->request.frag_offset);

    return;

}

static void bin_read_request(conn *c)
{
    assert(c != NULL);

    c->rlbytes = c->r_len;
    /*ensure the buffer can store a entire request*/
    ptrdiff_t offset = c->rcurr + sizeof(protocol_binary_request_header) - c->rbuf;
    if(c->rlbytes > c->rsize - offset)
    {
        size_t nsize = c->rsize;
        size_t size = c->rlbytes + sizeof(protocol_binary_request_header);
        while(size > nsize)
        {
            nsize *= 2;
        }

        if(nsize != size)
        {
            char *newm = realloc(c->rbuf, nsize);
            if(newm == NULL)
            {
                STATS_LOCK();
                stats.malloc_fails++;
                STATS_UNLOCK();
                if(global_settings.verbose)
                {
                    fprintf(stderr, "%d: Failed to grow buffer.. closing connection\n", c->sfd);
                }
                conn_set_state(c, conn_closing);
            }
            c->rbuf = newm;
            c->rcurr = c->rbuf + offset - sizeof(protocol_binary_request_header);
            c->rsize = nsize;
        }
        if(c->rbuf != c->rcurr)
        {
            memmove(c->rbuf, c->rcurr, c->rbytes);
            c->rcurr = c->rbuf;
        }
    }

    /*thinking about package assembly*/
    if(c->transport == tcp_transport)
    {
        if((c->binary_header.request.isfrag > 1) &&
                (c->binary_header.request.body_len != c->binary_header.request.request_len))
        {

            if(c->assemb_buf == NULL)
            {

                c->assemb_buf = (char *)calloc(1, c->binary_header.request.request_len + sizeof(c->binary_header.request));
                if(c->assemb_buf == NULL)
                {
                    STATS_LOCK();
                    stats.malloc_fails++;
                    STATS_UNLOCK();
                    if(global_settings.verbose)
                    {
                        fprintf(stderr, "%d: Failed to grow buffer.. closing connection\n", c->sfd);
                    }
                    conn_set_state(c, conn_closing);
                }
                c->assemb_size = c->binary_header.request.request_len + sizeof(c->binary_header.request);
                c->assemb_curr = c->assemb_buf;
            }
            else
            {

                c->assemb_curr = NULL;
                free(c->assemb_buf);
                c->assemb_size = 0;
                c->assemb_buf = (char *)calloc(1, c->binary_header.request.request_len + sizeof(c->binary_header.request));
                if(c->assemb_buf == NULL)
                {
                    STATS_LOCK();
                    stats.malloc_fails++;
                    STATS_UNLOCK();
                    if(global_settings.verbose)
                    {
                        fprintf(stderr, "%d: Failed to grow buffer.. closing connection\n", c->sfd);
                    }
                    conn_set_state(c, conn_closing);
                }
                c->assemb_size = c->binary_header.request.request_len + sizeof(c->binary_header.request);
                c->assemb_curr = c->assemb_buf;
            }

        }
    }

    c->frag_num = c->binary_header.request.isfrag;
    c->frag_lack = c->frag_num;

    conn_set_state(c, conn_nread);

}

static void dispatch_bin_command(conn * c)
{
    assert(c != NULL);

    bin_read_request(c);
}




/*
 * Transmit the next chunk of data from our list of msgbuf structures.
 *
 *   Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
 */
static enum transmit_result transmit(conn *c)
{
    assert(c != NULL);

    if (c->msgcurr < c->msgused && c->msglist[c->msgcurr].msg_iovlen == 0)
    {
        /* Finished writing the current msg; advance to the next. */
        c->msgcurr++;
    }
    if (c->msgcurr < c->msgused)
    {
        ssize_t res;
        struct msghdr *m = &c->msglist[c->msgcurr];

        res = sendmsg(c->sfd, m, 0);
        //struct timeval sendmsg_time;
        //gettimeofday(&sendmsg_time, NULL);
        //printf("~~~~~~sendmsg time: %llu,\tbytes: %d\n", 1000000 * sendmsg_time.tv_sec  + sendmsg_time.tv_usec, res);
        if (res > 0)
        {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.bytes_written += res;
            pthread_mutex_unlock(&c->thread->stats.mutex);

            /* We've written some of the data. Remove the completed iovec entries from the list of pending writes. */
            while (m->msg_iovlen > 0 && res >= m->msg_iov->iov_len)
            {
                res -= m->msg_iov->iov_len;
                m->msg_iovlen--;
                m->msg_iov++;
            }

            /* Might have written just part of the last iovec entry; adjust it so the next write will do the rest. */
            if (res > 0)
            {
                m->msg_iov->iov_base = (caddr_t)m->msg_iov->iov_base + res;
                m->msg_iov->iov_len -= res;
            }
            return TRANSMIT_INCOMPLETE;
        }
        else if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (!update_event(c, EV_WRITE | EV_PERSIST))
            {
                if (global_settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                return TRANSMIT_HARD_ERROR;
            }
            return TRANSMIT_SOFT_ERROR;
        }
        /* if res == 0 or res == -1 and error is not EAGAIN or EWOULDBLOCK,
           we have a real error, on which we close the connection */
        if (global_settings.verbose > 0)
            perror("Failed to write, and not due to blocking");

        if (IS_UDP(c->transport))
            conn_set_state(c, conn_read);
        else
            conn_set_state(c, conn_closing);
        return TRANSMIT_HARD_ERROR;
    }
    else
    {
        return TRANSMIT_COMPLETE;
    }
}


static void drive_machine(conn *c)
{
    bool stop = false;
    int sfd;
    socklen_t addrlen;
    struct sockaddr_storage addr;
    int nreqs = global_settings.reqs_per_event;
    int res;
    const char *str;
    int need_ntoh = 1;
    int tocopy_frag;
    int tocopy;

#ifdef HAVE_ACCEPT4
    static int  use_accept4 = 1;
#else
    static int  use_accept4 = 0;
#endif

    assert(c != NULL);

    while (!stop)
    {

        switch(c->state)
        {
        case conn_listening:
            addrlen = sizeof(addr);
#ifdef HAVE_ACCEPT4
            if (use_accept4)
            {
                sfd = accept4(c->sfd, (struct sockaddr *)&addr, &addrlen, SOCK_NONBLOCK);
            }
            else
            {
                sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
            }
#else
            sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
#endif
            if (sfd == -1)
            {
                if (use_accept4 && errno == ENOSYS)
                {
                    use_accept4 = 0;
                    continue;
                }
                perror(use_accept4 ? "accept4()" : "accept()");
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    /* these are transient, so don't log anything */
                    stop = true;
                }
                else if (errno == EMFILE)
                {
                    if (global_settings.verbose > 0)
                        fprintf(stderr, "Too many open connections\n");
                    accept_new_conns(false);
                    stop = true;
                }
                else
                {
                    perror("accept()");
                    stop = true;
                }
                break;
            }
            if (!use_accept4)
            {
                if (fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL) | O_NONBLOCK) < 0)
                {
                    perror("setting O_NONBLOCK");
                    close(sfd);
                    break;
                }
            }

            if (global_settings.maxconns_fast &&
                    stats.curr_conns + stats.reserved_fds >= global_settings.maxconns - 1)
            {
                str = "ERROR Too many open connections\r\n";
                res = write(sfd, str, strlen(str));
                close(sfd);
                STATS_LOCK();
                stats.rejected_conns++;
                STATS_UNLOCK();
            }
            else
            {
                dispatch_conn_new(sfd, conn_new_cmd, EV_READ | EV_PERSIST,
                                  DATA_BUFFER_SIZE, tcp_transport);
            }

            stop = true;
            break;

        case conn_waiting:
            if (!update_event(c, EV_READ | EV_PERSIST))
            {
                if (global_settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                break;
            }

            conn_set_state(c, conn_read);
            stop = true;
            break;

        case conn_read:
            res = IS_UDP(c->transport) ? try_read_udp(c) : try_read_network(c);

            switch (res)
            {
            case READ_NO_DATA_RECEIVED:
                conn_set_state(c, conn_waiting);
                break;
            case READ_DATA_RECEIVED:
                conn_set_state(c, conn_parse_cmd);
                break;
            case READ_ERROR:
                conn_set_state(c, conn_closing);
                break;
            case READ_MEMORY_ERROR: /* Failed to allocate more memory */
                /* State already set by try_read_network */
                break;
            }
            break;

        case conn_parse_cmd :
            if (try_read_command(c) == 0)
            {
                /* wee need more data! */
                conn_set_state(c, conn_waiting);
            }

            break;

        case conn_new_cmd:
            /* Only process nreqs at a time to avoid starving other
               connections */

            --nreqs;
            if (nreqs >= 0)
            {
                reset_cmd_handler(c);		// if no more req to handle, it'll jump to waiting
            }
            else		// JAY: hand over control to other conns in this thread
            {
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.conn_yields++;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                if (c->rbytes > 0)
                {
                    /* We have already read in data into the input buffer,
                      		 so libevent will most likely not signal read events
                     		 on the socket (unless more data is available. As a
                     		 hack we should just put in a request to write data,
                     		 because that should be possible ;-)
                    	 */
                    if (!update_event(c, EV_WRITE | EV_PERSIST))
                    {
                        if (global_settings.verbose > 0)
                            fprintf(stderr, "Couldn't update event\n");
                        conn_set_state(c, conn_closing);
                        break;
                    }
                }
                stop = true;
            }
            break;

        case conn_nread:

            if((c->frag_lack == 0) && (c->rlbytes == 0))
            {
                //complete_nread(c);
                jay_complete_nread(c);
                break;
            }

            /* Check if rbytes < 0, to prevent crash */
            if (c->rlbytes < 0)
            {
                if (global_settings.verbose)
                {
                    fprintf(stderr, "Invalid rlbytes to read: len %d\n", c->rlbytes);
                }
                conn_set_state(c, conn_closing);
                break;
            }

            protocol_binary_request_header *req;
            req = (protocol_binary_request_header *)c->rcurr;

            if(c->need_ntoh)
            {
                request_network_to_host(c, c->rcurr);
                c->need_ntoh = 0;
            }
            tocopy = req->request.body_len + sizeof(protocol_binary_request_header);
            tocopy_frag = req->request.body_len;
            //printf("Data need to copy is %d, already copy %d\n", tocopy, c->rbytes);
            if(c->rbytes >= tocopy)
            {
                /* This package is of integrity*/
                if(c->frag_num == 1)
                {
                    c->ritem = c->rcurr;
                    c->rcurr = c->rcurr + tocopy;
                    c->rbytes = c->rbytes - tocopy;
                    c->rlbytes -= tocopy_frag;
                    c->frag_lack--;

                    if(c->rlbytes == 0)
                    {
                        c->need_ntoh = 1;
                        break;
                    }
                    else
                    {
                        //printf("The failure thread is %u\n", pthread_self());
                        //printf("Data received is %d\n", c->rbytes);
                        fprintf(stderr, "Connection %d failed for unknown error \n", c->sfd);
                        break;
                    }
                }
                else
                {
                    if(c->frag_lack == c->frag_num)
                    {
                        memcpy(c->assemb_buf, c->rcurr, sizeof(protocol_binary_request_header));
                        c->rcurr += sizeof(protocol_binary_request_header);
                        c->rbytes -= sizeof(protocol_binary_request_header);
                        memmove(c->rbuf, c->rcurr, c->rbytes);
                        c->rcurr = c->rbuf;
                    }
                    if(req->request.request_id == c->binary_header.request.request_id)
                    {
                        if(c->frag_flag[req->request.frag_id - 1] == 0)
                        {
                            memcpy(c->assemb_buf + req->request.frag_offset +
                                   sizeof(protocol_binary_request_header),
                                   c->rcurr, req->request.body_len);
                            c->frag_flag[req->request.frag_id - 1] = 1;
                            c->frag_lack--;
                            c->rlbytes -= req->request.body_len;

                        }

                        c->rcurr += req->request.body_len;
                        c->rbytes -= req->request.body_len;

                        memmove(c->rbuf, c->rcurr, c->rbytes);
                        c->rcurr = c->rbuf;
                        if(c->rlbytes == 0 && c->frag_lack == 0)
                            break;
                    }
                    else
                    {
                        /*this is a disorder package*/
                    }
                }

            }
            /* first check if we have leftovers in the conn_read buffer */


            /*  now try reading from the socket */
            if(c->rcurr != c->rbuf)
            {
                //printf("c->rcurr != c->rbuf\n");
                memmove(c->rbuf, c->rcurr, c->rbytes);
                c->rcurr = c->rbuf;
            }
            int avail = c->rsize - c->rbytes;
            res = read(c->sfd, ((char *)c->rbuf + c->rbytes), avail);
            if (res > 0)
            {
                //printf("Res in nread is %d\n", res);
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.bytes_read += res;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                c->rbytes += res;
                break;
            }
            //printf("We arrived here, and res is %d, errno is %d\n", res, errno);
            if (res == 0)   /* end of stream */
            {
                conn_set_state(c, conn_closing);
                break;
            }
            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                //printf("We arrived here\n");
                if (!update_event(c, EV_READ | EV_PERSIST))
                {
                    if (global_settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
                //printf("we arrived here before stop = true\n");
                stop = true;
                //printf("we arrived here after stop = true\n");
                break;
            }

            /* otherwise we have a real error, on which we close the connection */
            if (global_settings.verbose > 0)
            {
                fprintf(stderr, "Failed to read, and not due to blocking:\n"
                        "errno: %d %s \n"
                        "rcurr=%lx ritem=%lx rbuf=%lx rlbytes=%d rsize=%d\n",
                        errno, strerror(errno),
                        (long)c->rcurr, (long)c->ritem, (long)c->rbuf,
                        (int)c->rlbytes, (int)c->rsize);
            }
            conn_set_state(c, conn_closing);
            break;

        case conn_write:
            /*
                 * We want to write out a simple response. If we haven't already,
                 * assemble it into a msgbuf list (this will be a single-entry
                 * list for TCP or a two-entry list for UDP).
                 */
            if (c->iovused == 0 || (IS_UDP(c->transport) && c->iovused == 1))
            {
                if (add_iov(c, c->wcurr, c->wbytes) != 0)
                {
                    if (global_settings.verbose > 0)
                        fprintf(stderr, "Couldn't build response\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
            }

            /* fall through... */

        case conn_mwrite:
            if (IS_UDP(c->transport) && c->msgcurr == 0 && build_udp_headers(c) != 0)
            {
                if (global_settings.verbose > 0)
                    fprintf(stderr, "Failed to build UDP headers\n");
                conn_set_state(c, conn_closing);
                break;
            }
            switch (transmit(c))
            {
            case TRANSMIT_COMPLETE:
                if (c->state == conn_mwrite)
                {
                    conn_release_items(c);
                    /* XXX:  I don't know why this wasn't the general case */
                    if (c->write_and_free)
                    {
                        free(c->write_and_free);
                        c->write_and_free = 0;
                    }
                    if(c->protocol == binary_prot)
                    {
                        conn_set_state(c, c->write_and_go);
                    }
                    else
                    {
                        conn_set_state(c, conn_new_cmd);
                    }
                }
                else if (c->state == conn_write)
                {
                    if (c->write_and_free)
                    {
                        free(c->write_and_free);
                        c->write_and_free = 0;
                    }
                    conn_set_state(c, c->write_and_go);
                }
                else
                {
                    if (global_settings.verbose > 0)
                        fprintf(stderr, "Unexpected state %d\n", c->state);
                    conn_set_state(c, conn_closing);
                }
                break;

            case TRANSMIT_INCOMPLETE:
            case TRANSMIT_HARD_ERROR:
                break;                   /* Continue in state machine. */

            case TRANSMIT_SOFT_ERROR:
                stop = true;
                break;
            }
            break;

        case conn_closing:
            if (IS_UDP(c->transport))
                conn_cleanup(c);
            else
                conn_close(c);
            stop = true;
            break;

        case conn_closed:
        case conn_max_state:
            assert(false);
            break;
        }
    }

    return;
}


void global_settings_init()
{
    global_settings.chunk_size = 4096;
    global_settings.factor = 2;
    global_settings.item_size_max = 1024*1024*16; /*16MB, 13*16MB = 208*/

    global_settings.slab_reassign = 1;
    global_settings.verbose = 2;
    global_settings.hash_algorithm = NULL;
    global_settings.num_threads = 8;
    global_settings.port = 11212;
    global_settings.udpport = 11212;
    global_settings.inter = NULL;
    global_settings.maxbytes = 1024*1024*1024;	/*1GB*/
    global_settings.maxconns = 4196;
    global_settings.max_open_files = 8192;

    global_settings.socketpath = NULL;


    global_settings.num_threads = 8;
    global_settings.num_threads_per_udp = 0;

    global_settings.reqs_per_event = 20;
    global_settings.backlog = 1024;
    global_settings.binding_protocol = binary_prot;


    // JAY
    global_settings.BLOCK_SIZE_IN_KB = G_BLOCK_SIZE_IN_KB;
    global_settings.BLOCK_SIZE_SHIFT = G_BLOCK_SIZE_SHIFT;
}


/********************************************************************/
/**********************  JAY FUNCS  ***********************************/

static hash_table open_mds_file_table;		// record the opened file, avoid return 2 different fds for the same file

static void open_mds_file_table_init()
{
    ht_init(&open_mds_file_table, HT_KEY_CONST | HT_VALUE_CONST, 0.05);	//CONST means ht won't malloc memory for the content
}

static int check_if_mds_file_is_opened(char *path, int path_len)
{
    return ht_contains(&open_mds_file_table, path, path_len);
}

static void insert_open_mds_file(char *path, int path_len, int mfd)
{
    ht_insert(&open_mds_file_table, path, path_len, (void *)mfd, sizeof(mfd));
}

static int get_open_mds_file_mfd(char *path, int path_len)
{
    size_t dummy;
    return (int) ht_get(&open_mds_file_table, path, path_len, &dummy);
}

static void remove_open_mds_file(char *path, int path_len)
{
    ht_remove(&open_mds_file_table, path, path_len);
}



static hash_table ost_file_table;		// link cfd to a real file in ost, this is the ost layer thing

static void ost_file_table_init()
{
    ht_init(&ost_file_table, HT_VALUE_CONST, 0.05);	//CONST means ht won't malloc memory for the content
}

static int check_if_ost_file_exists(int cfd)
{
    return ht_contains(&ost_file_table, &cfd, sizeof(cfd));
}

static void insert_ost_file(int cfd, Ost_File *calloc_file)
{
    ht_insert(&ost_file_table, &cfd, sizeof(cfd), calloc_file, sizeof(calloc_file));
}

static Ost_File * get_ost_file(int cfd)
{
    size_t dummy;
    return (Ost_File*) ht_get(&ost_file_table, &cfd, sizeof(cfd), &dummy);
}

static void remove_ost_file(int cfd)
{
    ht_remove(&ost_file_table, &cfd, sizeof(cfd));
}

static void init_ost_file(Ost_File * file)
{
    pthread_mutex_init(&file->cache_tree_mutex, NULL);
    INIT_RADIX_TREE(&file->cache_tree);
    memset(file->path, 0, PATH_MAX);
    file->real_fd = -1;
}

static void update_ost_file_size(Ost_File * file, size_t offset, size_t count)
{
	if(offset + count <= file->size) 
	{
		return;
	}
	else if(offset >= file->size)
	{
		file->size = offset + count;
	}
	else
	{
		file->size += offset + count - file->size;
	}
}



static mfd_array_entry 		*mfd_array;
static int					last_assign_mfd;

static pthread_mutex_t mfd_array_mutex = PTHREAD_MUTEX_INITIALIZER;

static void init_mds_file(Mds_File* file)
{
    pthread_mutex_init(&file->block_tree_mutex, NULL);
    INIT_RADIX_TREE(&file->block_tree);
    file->access_time = 0;
    file->dirty_time = 0;
    file->f_count = 0;
    file->being_writeback = false;
}

static void mfd_array_init()
{	
    if ((mfd_array = calloc(MAX_FDS, sizeof(mfd_array_entry))) == NULL)
    {
        fprintf(stderr, "Failed to allocate mfd array structures\n");
        exit(1);
    }

    // avoid confusion, do not assign 0,1,2 as fd
    mfd_array[0].is_occupied = 1;
    mfd_array[1].is_occupied = 1;
    mfd_array[2].is_occupied = 1;

    last_assign_mfd = 0;
}

static void mfd_array_finalize()
{
    int i=0;
    for(i=3; i<MAX_FDS; i++)
    {
        if(mfd_array[i].is_occupied == 1)
        {
            free(mfd_array[i].file);
        }
    }

    free(mfd_array);
}

static inline int check_if_mfd_is_occupied(int mfd)
{
    return mfd_array[mfd].is_occupied;
}

static int assign_mfd(int arg_num, char *path, int flag, int mode)
{
    //  Make sure different client opened the same file, get the same mfd!!
    //  It's important. It seems Dong haven't deal with this.
    if(check_if_mds_file_is_opened(path, PATH_MAX))
    {
        // opened already
        return get_open_mds_file_mfd(path, PATH_MAX);
    }

    // else, not opened

    pthread_mutex_lock(&mfd_array_mutex);
    int assign_mfd;
    int i = 1;
    for(; i<MAX_FDS; i++)
    {
        assign_mfd = (last_assign_mfd + i) % MAX_FDS;
		
        if(mfd_array[assign_mfd].is_occupied == 1)
        {
            continue;
        }
        else
        {
            mfd_array[assign_mfd].is_occupied = 1;
            last_assign_mfd = assign_mfd;
            break;
        }
    }
    pthread_mutex_unlock(&mfd_array_mutex);

    if(i == MAX_FDS)
    {
        fprintf(stderr, "ERROR: cannot find a free mfd, mfd_array is full!\n");
        exit(-1);
    }
    else
    {
        mfd_array[assign_mfd].arg_num = arg_num;
	    memcpy(mfd_array[assign_mfd].path_name, path, PATH_MAX);	// need add the '\0' char
	    mfd_array[assign_mfd].flag = flag;
        mfd_array[assign_mfd].mode = mode;
        mfd_array[assign_mfd].file = calloc(1, sizeof(Mds_File));
        init_mds_file(mfd_array[assign_mfd].file);

        // it's essential that use mfd_array[assign_mfd].path_name to insert_open_mds_file
        // since the hash_table won't calloc new memory for the path, the path parm should be enduring it self
        insert_open_mds_file(mfd_array[assign_mfd].path_name, PATH_MAX, assign_mfd);
        return assign_mfd;
    }
}

static void free_mfd(int mfd)
{
    pthread_mutex_lock(&mfd_array_mutex);

    assert(mfd_array[mfd].is_occupied == true);

    mfd_array[mfd].is_occupied = false;
    mfd_array[mfd].arg_num = 0;
    memset(mfd_array[mfd].path_name, 0, PATH_MAX);
    mfd_array[mfd].flag = 0;
    mfd_array[mfd].mode = 0;
    free(mfd_array[mfd].file);

    pthread_mutex_unlock(&mfd_array_mutex);
}

static mfd_array_entry * get_mfd_entry(int mfd)		// who call this func cannot change the achieved variable
{
    assert(mfd_array[mfd].is_occupied == true);
    return &mfd_array[mfd];
}



// the first ost has id 0, it equals to NULL, so we set this transformation
#define OST_ID_TO_RDX_ID(ost_id) (void*)((1UL << 32) | (unsigned long) ost_id)
#define RDX_ID_TO_OST_ID(rdx_id) (((int)rdx_id))

static void query_block_metadata(int mfd, int start_block_index, int num_block, int *buf,
                                 int query_server_id, SuccessiveInfo *successive_info, size_t size)
{
    mfd_array_entry *mfd_entry = get_mfd_entry(mfd);
    Mds_File *file = mfd_entry->file;

    int end_block_index = start_block_index + num_block - 1;
    int query_index = start_block_index;
    void *rdx_id = NULL;
    int ost_id;

    if(config_param.LASIOD == 1 && successive_info->accumulated_data_size > config_param.LASIOD_LARGE_IO_SIZE)
    {
        pthread_mutex_lock(&file->block_tree_mutex);
        for(; query_index <= end_block_index; query_index++)
        {
            rdx_id = radix_tree_lookup(&file->block_tree, query_index);
            if(rdx_id != NULL)
            {
                ost_id = RDX_ID_TO_OST_ID((int)rdx_id);
                buf[query_index - start_block_index] = ost_id;		// convert void* to int just for convenience
                if(DEBUG)
                    fprintf(stderr, "QUERY MDS:\t file_block[%d] is at ost[%d]\n", query_index, ost_id);
            }
            else
            {                
                buf[query_index - start_block_index] = -1;
                if(DEBUG)
                    fprintf(stderr, "QUERY MDS:\t file_block[%d] is NOT cached\n", query_index);
            }
        }
		
		size_t current_pos_in_successive_data_range = successive_info->accumulated_data_size - size;

		for(query_index = start_block_index; query_index <= end_block_index; query_index++)
		{
			if(buf[query_index - start_block_index] == -1)
			{
				int server_offset = (current_pos_in_successive_data_range + (query_index - start_block_index) * G_BLOCK_SIZE_IN_BYTES)
									/ config_param.LASIOD_LARGE_IO_SIZE;
				server_offset = server_offset % config_param.LASIOD_SCATTER_NUM;
				buf[query_index - start_block_index] = (query_server_id + server_offset) % G_SERVER_NUM;
				radix_tree_insert(&file->block_tree, query_index, (void *)OST_ID_TO_RDX_ID(buf[query_index - start_block_index]));	// convert int to void* just for convenience
   			}
		}
		
        pthread_mutex_unlock(&file->block_tree_mutex);
    }
    else
    {
        pthread_mutex_lock(&file->block_tree_mutex);
        for(; query_index <= end_block_index; query_index++)
        {
            rdx_id = radix_tree_lookup(&file->block_tree, query_index);
            if(rdx_id != NULL)
            {
                ost_id = RDX_ID_TO_OST_ID((int)rdx_id);
                buf[query_index - start_block_index] = ost_id;		// convert void* to int just for convenience
                if(DEBUG)
                    fprintf(stderr, "QUERY MDS:\t file_block[%d] is at ost[%d]\n", query_index, ost_id);
            }
            else
            {
                // not cached?
                // then we should decide where to cache it
                radix_tree_insert(&file->block_tree, query_index, (void *)OST_ID_TO_RDX_ID(query_server_id));	// convert int to void* just for convenience
                buf[query_index - start_block_index] = query_server_id;	// -1 means the qeury ost should cache it by it self
                if(DEBUG)
                    fprintf(stderr, "QUERY MDS:\t file_block[%d] is NOT cached\n", query_index);
            }
        }
        pthread_mutex_unlock(&file->block_tree_mutex);
    }

}

static int read_blocks_from_lustre(int fd, char*buf, int *block_array, int num_block,
                                   int first_block_start_offset, int last_block_end_offset)
{
    int offset_in_buf = 0;
    int cur_blk;
    for(cur_blk=0; cur_blk<num_block; cur_blk++)
    {
        if(cur_blk == 0)
        {
            lseek(fd, block_array[cur_blk] * G_BLOCK_SIZE_IN_BYTES + first_block_start_offset, SEEK_SET);
            if(cur_blk == num_block - 1)
            {
                read(fd, buf, last_block_end_offset - first_block_start_offset);
                offset_in_buf += last_block_end_offset - first_block_start_offset + 1;
            }
            else
            {
                read(fd, buf, G_BLOCK_SIZE_IN_BYTES - first_block_start_offset);
                offset_in_buf += G_BLOCK_SIZE_IN_BYTES - first_block_start_offset;
            }
        }
        else if(cur_blk == num_block - 1)
        {
            lseek(fd, block_array[cur_blk] * G_BLOCK_SIZE_IN_BYTES, SEEK_SET);
            read(fd, buf + offset_in_buf, last_block_end_offset + 1);
            offset_in_buf += last_block_end_offset + 1;
        }
        else
        {
            lseek(fd, block_array[cur_blk] * G_BLOCK_SIZE_IN_BYTES, SEEK_SET);
            read(fd, buf + offset_in_buf, G_BLOCK_SIZE_IN_BYTES);
            offset_in_buf += G_BLOCK_SIZE_IN_BYTES;
        }
    }
    return offset_in_buf;
}

static void server_name_array_init()
{
    FILE *fd;
    fd = fopen(SERVER_LIST_PATH, "a+");
    if(fd == NULL)
    {
        perror("open server name file failed!");
        return ;
    }

    char buf[1024];
    int line = 0;
    while(fgets(buf, 1024, fd))
    {
        line++;
    }
    fseek(fd, 0, SEEK_SET);

    G_SERVER_NUM = line;
    G_SERVER_NAME_ARRAY = calloc(G_SERVER_NUM, sizeof(server_name));
    int count = 0;
    int i;
    while(fgets(buf, 1024, fd))
    {
        for(i=0; i<1024; i++)
        {
            if(buf[i] != '\0')
            {
                if(buf[i] == 13)	// return
                {
                    buf[i] = '\0';
                }
                else if(buf[i] == '\n')
                {
                    buf[i] = '\0';
                }
            }
            else
            {
                break;
            }
        }
        assert(strlen(buf) < MAX_SERVER_NAME);
        strcpy(G_SERVER_NAME_ARRAY[count].name, buf);
        count ++;
    }

    printf("%d server name init complete: ", G_SERVER_NUM);
    for(i=0; i<G_SERVER_NUM; i++)
        printf("%s ", G_SERVER_NAME_ARRAY[i].name);
    printf("\n");

	if(config_param.LASIOD_SCATTER_NUM < 0) config_param.LASIOD_SCATTER_NUM = G_SERVER_NUM;
	assert(config_param.LASIOD_SCATTER_NUM <= G_SERVER_NUM);

    fclose(fd);
}

static void server_name_array_finalize()
{
    free(G_SERVER_NAME_ARRAY);
}

static void my_server_id_init()
{
	// 0,1,2,3,4,5,6,7,8,9,10
	// ION: 0,4,8...
	// CN:   ,1,2,3,  ,5,6,7, ,9,10...
	
    server_name_array_init();

    char server_name[10];
    gethostname(server_name, sizeof(server_name));

    int i;
    for(i=0; i<G_SERVER_NUM; i++)
    {
        if(strcmp(server_name, G_SERVER_NAME_ARRAY[i].name) == 0)
        {
            MY_SERVER_ID = i;
            fprintf(stderr, "MY_SERVER_ID = %d\n", MY_SERVER_ID);
            return;
        }
    }

    fprintf(stderr, "error: can't find my name(%s) in server_list\n", server_name);
    exit(0);
}



static char DEVICE_PATH[OST_CACHE_DEVICE_PATH_LEN];
static int device_fd;
static unsigned long device_size;
static uint8_t *device_space;
static int NUM_DEVICE_BLOCKS;
static int last_allocated_block_id;

int DEVICE_SEM_ID = -1;

static void cache_device_init()
{
	if(config_param.use_cache == 0) return;
	
    char my_server_name[10];
    gethostname(my_server_name, sizeof(my_server_name));

    // lustre file device path
    //sprintf(DEVICE_PATH, "%s%s", OST_CACHE_DEICE_PATH_PREFIX, my_server_name);

    // tmpfs file device path
    if(config_param.cache_device_tmpfs == 1)
    {
		strcpy(DEVICE_PATH, config_param.tmpfs_path);

	}
	else
	{
		strcpy(DEVICE_PATH, config_param.ssd_path);
	}

    // TODO: compare the performance of fopen()
    device_fd = open(DEVICE_PATH, O_RDWR);
    if(device_fd < 0)
    {
        perror("Cannot open device");
		fprintf(stderr, "device path: %s\n", DEVICE_PATH);
        return ;
    }

    unsigned long start_pos = lseek(device_fd, 0, SEEK_SET);
    if(start_pos < 0)
    {
        fprintf(stderr, "Cannot lseek device file: %s\n", DEVICE_PATH);
        return ;
    }
    unsigned long end_pos = lseek(device_fd, 0, SEEK_END);
    if(end_pos < 0)
    {
        fprintf(stderr, "Cannot lseek device file: %s\n", DEVICE_PATH);
        return ;
    }
    device_size = end_pos - start_pos;
    lseek(device_fd, 0, SEEK_SET);

    assert(device_size > 0);
    NUM_DEVICE_BLOCKS = device_size / G_BLOCK_SIZE_IN_BYTES;
    assert(NUM_DEVICE_BLOCKS > 0);

    device_space = calloc(NUM_DEVICE_BLOCKS, sizeof(uint8_t));	// 0 means empty
    last_allocated_block_id = -1;

    if(DEBUG)
        fprintf(stderr, "DEVICE_SIZE: %lld GB\n", device_size / 1024 / 1024 / 1024);

	// FOR SIMULATE SSD

	/*
	if(config_param.simulate_ssd == 0) return;
	
	DEVICE_SEM_ID = semget((key_t)SIMULATE_SSD_SEM_KEY, 1, 0666 | IPC_CREAT); 
	union semun sem_union;    
    sem_union.val = 1;  
    if(semctl(DEVICE_SEM_ID, 0, SETVAL, sem_union) == -1)  
    {
		fprintf(stderr, "Error: failed to init device semaphore\n");
		exit(1);
    }
	*/
	
}

static void cache_device_finalize()
{
    free(device_space);
    close(device_fd);
	
	// FOR SIMULATE SSD
	/*
	union semun sem_union;  
    if(semctl(DEVICE_SEM_ID, 0, IPC_RMID, sem_union) == -1)  
        fprintf(stderr, "Error: Failed to delete semaphore\n");
	*/
}

static void device_semaphore_p()  
{   
    struct sembuf sem_b;  
    sem_b.sem_num = 0;  
    sem_b.sem_op = -1;//P()  
    sem_b.sem_flg = SEM_UNDO;  
    if(semop(DEVICE_SEM_ID, &sem_b, 1) == -1)  
    {  
        fprintf(stderr, "device_semaphore_p failed\n");  
		exit(1);
    }  
} 

static void device_semaphore_v()  
{    
    struct sembuf sem_b;  
    sem_b.sem_num = 0;  
    sem_b.sem_op = 1;//V()  
    sem_b.sem_flg = SEM_UNDO;  
    if(semop(DEVICE_SEM_ID, &sem_b, 1) == -1)  
    {  
        fprintf(stderr, "device_semaphore_v failed\n");  
		exit(1);
    }  
}  

/*
struct timeval local_delay_start, local_delay_end;
static void simulate_ssd_start()
{
	if(config_param.cache_device_tmpfs == 0 || config_param.simulate_ssd == 0) return;
	
	device_semaphore_p();
	gettimeofday(&local_delay_start, NULL);
}

static void simulate_ssd_end()
{
	if(config_param.cache_device_tmpfs == 0 || config_param.simulate_ssd == 0) return;
	
	gettimeofday(&local_delay_end, NULL);
	usleep(config_param.latency_delay_ratio * (1000000 * ( local_delay_end.tv_sec - local_delay_start.tv_sec ) + local_delay_end.tv_usec - local_delay_start.tv_usec));
	device_semaphore_v();
}
*/


/*
static void keep_tmpfs_busy_by_writing(char* buf, unsigned long offset, int size, double num_times)
{		
	if(config_param.cache_device_tmpfs == 1 && config_param.simulate_ssd == 1)
	{
		if(size < 5000) num_times *= 0.5;
		int count = (int)num_times;
		double digit_times = num_times - count;
		if(size * digit_times != 0)
		{
			if(lseek(device_fd, offset, SEEK_SET) < 0)
    		{
        		perror("lseek error in keep_tmpfs_busy_by_writing()");
       			return;
    		}
			int bytes = write(device_fd, buf, size * digit_times);
			if(bytes <= 0)
			{
				perror("write error in keep_tmpfs_busy_by_writing()\n");
    		}
		}

		int i;
		for(i=0; i<count; i++)
		{
			if(lseek(device_fd, offset, SEEK_SET) < 0)
    		{
        		perror("lseek error in keep_tmpfs_busy_by_writing()");
       			return;
    		}
			int bytes = write(device_fd, buf, size);
			if(bytes <= 0)
			{
				perror("write error in keep_tmpfs_busy_by_writing()\n");
    		}
		}
	}
}
*/



static void write_partial_block_in_device(int device_block_id, int start_offset, int end_offset, char* buf)
{
    if(lseek(device_fd, ((unsigned long)device_block_id) * G_BLOCK_SIZE_IN_BYTES + start_offset, SEEK_SET) < 0)
    {
        perror("lseek error in write_partial_block_in_device()");
        return;
    };
	//device_semaphore_p();
    int bytes = write(device_fd, buf, end_offset - start_offset + 1);
    if(bytes <= 0)
    {
        perror("write error in write_partial_block_in_device()\n");
    }
    assert(bytes == end_offset - start_offset + 1);
	/*
	if(config_param.cache_device_tmpfs == 1 && config_param.simulate_ssd == 1)
		keep_tmpfs_busy_by_writing(buf, ((unsigned long)device_block_id) * G_BLOCK_SIZE_IN_BYTES + start_offset, 
			(end_offset - start_offset + 1), config_param.simulate_write_latency);
	device_semaphore_v();
	*/
}

static void read_partial_block_from_device(int device_block_id, int start_offset, int end_offset, char* buf)
{
    if(lseek(device_fd, ((unsigned long)device_block_id) * G_BLOCK_SIZE_IN_BYTES + start_offset, SEEK_SET) < 0)
    {
        perror("lseek error in read_partial_block_from_device()");
        return;
    }
	//device_semaphore_p();
    int bytes = read(device_fd, buf, end_offset - start_offset + 1);
    if(bytes <= 0)
    {
        perror("read error in read_partial_block_from_device()\n");
    }
    assert(bytes == end_offset - start_offset + 1);

	/*
	if(config_param.cache_device_tmpfs == 1 && config_param.simulate_ssd == 1)
		keep_tmpfs_busy_by_writing(buf, ((unsigned long)device_block_id) * G_BLOCK_SIZE_IN_BYTES + start_offset, 
			(end_offset - start_offset + 1), config_param.simulate_read_latency);
	device_semaphore_v();
	*/
}

static void write_block_in_device(int device_block_id, char* buf)
{
    if(lseek(device_fd, ((unsigned long)device_block_id) * G_BLOCK_SIZE_IN_BYTES, SEEK_SET) < 0)
    {
        perror("lseek error in write_block_in_device()");
        return;
    }
	//device_semaphore_p();
    int bytes = write(device_fd, buf, G_BLOCK_SIZE_IN_BYTES);
    if(bytes <= 0)
    {
        perror("write error in write_block_in_device()\n");
    }
    assert(bytes == G_BLOCK_SIZE_IN_BYTES);

	/*
	if(config_param.cache_device_tmpfs == 1 && config_param.simulate_ssd == 1)
		keep_tmpfs_busy_by_writing(buf, ((unsigned long)device_block_id) * G_BLOCK_SIZE_IN_BYTES, 
			G_BLOCK_SIZE_IN_BYTES, config_param.simulate_write_latency);
	device_semaphore_v();
	*/
}

static void read_block_from_device(int device_block_id, char* buf)
{
    if(lseek(device_fd, ((unsigned long)device_block_id) * G_BLOCK_SIZE_IN_BYTES, SEEK_SET) < 0)
    {
        perror("lseek error in read_block_from_device()");
        return;
    }
	//device_semaphore_p();
    int bytes = read(device_fd, buf, G_BLOCK_SIZE_IN_BYTES);
    if(bytes <= 0)
    {
        perror("read error in read_block_from_device()\n");
    }
    assert(bytes == G_BLOCK_SIZE_IN_BYTES);

	/*
	if(config_param.cache_device_tmpfs == 1 && config_param.simulate_ssd == 1)
		keep_tmpfs_busy_by_writing(buf, ((unsigned long)device_block_id) * G_BLOCK_SIZE_IN_BYTES, 
			G_BLOCK_SIZE_IN_BYTES, config_param.simulate_read_latency);
	device_semaphore_v();
	*/
}

static void write_blocks_in_device(int start_device_block_id, int num_blocks, char* buf)
{
    if(lseek(device_fd, ((unsigned long)start_device_block_id) * G_BLOCK_SIZE_IN_BYTES, SEEK_SET) < 0)
    {
        perror("lseek error in write_blocks_in_device()");
        return;
    }
	//device_semaphore_p();
    int bytes = write(device_fd, buf, G_BLOCK_SIZE_IN_BYTES * num_blocks);
    if(bytes <= 0)
    {
        perror("write error in write_blocks_in_device()\n");
    }
    assert(bytes == G_BLOCK_SIZE_IN_BYTES * num_blocks);

	/*
	if(config_param.cache_device_tmpfs == 1 && config_param.simulate_ssd == 1)
		keep_tmpfs_busy_by_writing(buf, ((unsigned long)start_device_block_id) * G_BLOCK_SIZE_IN_BYTES, 
			G_BLOCK_SIZE_IN_BYTES * num_blocks, config_param.simulate_write_latency);
	device_semaphore_v();
	*/
}

static void read_blocks_from_device(int start_device_block_id, int num_blocks, char* buf)
{
    if(lseek(device_fd, ((unsigned long)start_device_block_id) * G_BLOCK_SIZE_IN_BYTES, SEEK_SET) < 0)
    {
        perror("lseek error in read_blocks_from_device()");
        return;
    }
	//device_semaphore_p();
    int bytes = read(device_fd, buf, G_BLOCK_SIZE_IN_BYTES * num_blocks);
    assert(bytes == G_BLOCK_SIZE_IN_BYTES * num_blocks);

	/*
	if(config_param.cache_device_tmpfs == 1 && config_param.simulate_ssd == 1)
		keep_tmpfs_busy_by_writing(buf, ((unsigned long)start_device_block_id) * G_BLOCK_SIZE_IN_BYTES, 
			G_BLOCK_SIZE_IN_BYTES * num_blocks, config_param.simulate_read_latency);
	device_semaphore_v();
	*/
}

static void put_blocks_in_device(int *device_block_array, int num_device_block, char* buf)
{
    int i,j;
    int successive_start = -1;
    int successive_end = -1;
    int already_written_bytes = 0;
    i = 0;
    while(i<num_device_block)
    {
        successive_start = device_block_array[i];
        successive_end = successive_start;
        // push the loop, find the last successive block
        for(j = i + 1; j<num_device_block; j++)
        {
            // if this file_bock is adjacent with last file_block
            if(device_block_array[j] - device_block_array[j-1] == 1)
            {
                successive_end++;
                continue;
            }
            else
            {
                break;
            }
        }

        // if not adjacent or is adjacent but also the last block
        // write current successive blocks
        int num_successive_block = successive_end - successive_start + 1;
        write_blocks_in_device(successive_start, num_successive_block, buf + already_written_bytes);
        already_written_bytes += num_successive_block * G_BLOCK_SIZE_IN_BYTES;

        i = j;	// to the start of while
    }
}

static void get_blocks_from_device(int *device_block_array, int num_device_block, char* buf)
{
    int i,j;
    int successive_start = -1;
    int successive_end = -1;
    int already_written_bytes = 0;
    i = 0;
    while(i<num_device_block)
    {
        successive_start = device_block_array[i];
        successive_end = successive_start;
        // push the loop, find the last successive block
        for(j = i + 1; j<num_device_block; j++)
        {
            // if this file_bock is adjacent with last file_block
            if(device_block_array[j] - device_block_array[j-1] == 1)
            {
                successive_end++;
                continue;
            }
            else
            {
                break;
            }
        }

        // if not adjacent or is adjacent but also the last block
        // write current successive blocks
        int num_successive_block = successive_end - successive_start + 1;
        read_blocks_from_device(successive_start, num_successive_block, buf + already_written_bytes);
        already_written_bytes += num_successive_block * G_BLOCK_SIZE_IN_BYTES;

        i = j;	// to the start of while
    }
}

static int put_blocks_in_fs(int file_fd, int start_file_block_id, int num_blocks, char* buf)
{
    if(lseek(file_fd, ((unsigned long)start_file_block_id) * G_BLOCK_SIZE_IN_BYTES, SEEK_SET) < 0)
    {
        perror("lseek error in put_blocks_in_fs()");
        return -1;
    }
    int bytes = write(file_fd, buf, G_BLOCK_SIZE_IN_BYTES * num_blocks);
    if(bytes <= 0)
    {
        perror("read error in put_blocks_in_fs()\n");
    }
    assert(bytes == G_BLOCK_SIZE_IN_BYTES * num_blocks);
    return bytes;
}

static int get_blocks_from_fs(int file_fd, int start_file_block_id, int num_blocks, char* buf)
{
    if(lseek(file_fd, ((unsigned long)start_file_block_id) * G_BLOCK_SIZE_IN_BYTES, SEEK_SET) < 0)
    {
        perror("lseek error in get_blocks_from_fs()");
        return -1;
    }
    int bytes = read(file_fd, buf, G_BLOCK_SIZE_IN_BYTES * num_blocks);
    if(bytes < 0)
    {
        perror("read error in get_blocks_from_fs()\n");
    }
    else if(bytes < G_BLOCK_SIZE_IN_BYTES * num_blocks)
    {
        if(DEBUG >= 3) fprintf(stderr, "WARN: can not read in a whole block from fs\n");
    }
    // the read bytes may be smaller than what you want
    // ex. when write to a empty file
    return bytes;
}

pthread_mutex_t device_space_allocator_mutex = PTHREAD_MUTEX_INITIALIZER;
static int allocate_device_space(int *block_id_array, int num_block)
{
    // TODO: for now, do not consider the replacement machanism, just allocate the space linearly
    pthread_mutex_lock(&device_space_allocator_mutex);
    int i, j;
    for(i=0; i<num_block; i++)
    {
        for(j=0; j<NUM_DEVICE_BLOCKS; j++)
        {
            if(device_space[(last_allocated_block_id + 1 + j) % NUM_DEVICE_BLOCKS] == 1)
            {
                continue;
            }
            else
            {
                block_id_array[i] = (last_allocated_block_id + 1 + j) % NUM_DEVICE_BLOCKS;
                device_space[(last_allocated_block_id + 1 + j) % NUM_DEVICE_BLOCKS] = 1;
                last_allocated_block_id = last_allocated_block_id + 1 + j;
                break;
            }
        }
        // look one round, and cannot find free block to allocate
        if(j == NUM_DEVICE_BLOCKS)
        {
            fprintf(stderr, "look one round, and cannot find free block to allocate \n");
            exit(-1);
        }
    }
    pthread_mutex_unlock(&device_space_allocator_mutex);
    return 1;
}

static void reclaim_device_space(int *block_id_array, int num_block)
{
    pthread_mutex_lock(&device_space_allocator_mutex);
    int i;
    for(i=0; i<num_block; i++)
    {
        assert(device_space[block_id_array[i]] == 1);
        device_space[block_id_array[i]] = 0;
    }
    pthread_mutex_unlock(&device_space_allocator_mutex);
}

static void set_conn_cork(int sfd)
{
    int tcp_cork_on = 1;
    if(setsockopt(sfd, SOL_TCP, TCP_CORK, &tcp_cork_on, sizeof(tcp_cork_on)) != 0)
    {
        perror("set_conn_cork error");
    }
}

static void set_conn_uncork(int sfd)
{
    int tcp_cork_on = 0;
    if(setsockopt(sfd, SOL_TCP, TCP_CORK, &tcp_cork_on, sizeof(tcp_cork_on)) != 0)
    {
        perror("set_conn_uncork error");
    }
}

static void set_conn_snd_buf(int sfd)
{
    int sock_buf_size = MAX_SOCKET_SND_BUFFER;
    if(setsockopt( sfd, SOL_SOCKET, SO_SNDBUF, (char *)&sock_buf_size, sizeof(sock_buf_size)) != 0)
    {
        perror("set_conn_snd_buf error");
    }
    if(DEBUG) fprintf(stderr, "set sfd[%d] a bigger buffer: %lu\n", sfd, MAX_SOCKET_SND_BUFFER);
}

// the first ost has id 0, it equals to NULL, so we set this transformation
// blk_id has 32 bits, rdx_id has 64 bits
#define BLK_ID_TO_RDX_ID(blk_id) (void*)((1UL << 32) | (unsigned long) blk_id)
#define RDX_ID_TO_BLK_ID(rdx_id) (((int)rdx_id))

static uint64_t read_ost_data(Ost_File *file, char* buf, int* file_block_array, int num_file_block,
                              int first_block_start_offset, int last_block_end_offset)
{
    // inquire in the radix tree of file
    int *device_block_array = calloc(num_file_block, sizeof(int));
    void *rdx_id = NULL;

    pthread_mutex_lock(&file->cache_tree_mutex);
    // TODO: gang_lookup
    int i;
    for(i=0; i<num_file_block; i++)
    {
        rdx_id = radix_tree_lookup(&file->cache_tree, file_block_array[i]);
        if(rdx_id != NULL)
        {
            device_block_array[i] = RDX_ID_TO_BLK_ID(rdx_id);		// convert void* to int just for convenience
        }
        else
        {
            // not cached?
            device_block_array[i] = -1;
        }
    }
    pthread_mutex_unlock(&file->cache_tree_mutex);

    uint64_t already_read_bytes = 0;

    // read the blocks into cache device
    int successive_start = -1;
    int successive_end = -1;
    int j;
    i = 0;
    while(i<num_file_block)
    {
        // handle the first partial block
        if(i == 0 && first_block_start_offset != 0)		// the first block is partial
        {
            // the first partial block is missing
            if(device_block_array[i] == -1)
            {
                // read in the missing partial block
                char read_partial_block_buf[G_BLOCK_SIZE_IN_BYTES];
                get_blocks_from_fs(file->real_fd, file_block_array[i], 1, read_partial_block_buf);

                //
                if(i == num_file_block - 1)				// it also is the last block
                {
                    memcpy(buf, read_partial_block_buf + first_block_start_offset, last_block_end_offset - first_block_start_offset + 1);
                    already_read_bytes += last_block_end_offset - first_block_start_offset + 1;
                }
                else
                {
                    memcpy(buf, read_partial_block_buf + first_block_start_offset, G_BLOCK_SIZE_IN_BYTES - first_block_start_offset);
                    already_read_bytes += G_BLOCK_SIZE_IN_BYTES - first_block_start_offset;
                }

                // cache the block
                allocate_device_space(&device_block_array[i], 1);
                put_blocks_in_device(&device_block_array[i], 1, read_partial_block_buf);
                pthread_mutex_lock(&file->cache_tree_mutex);
                radix_tree_insert(&file->cache_tree, file_block_array[i], BLK_ID_TO_RDX_ID(device_block_array[i]));
                pthread_mutex_unlock(&file->cache_tree_mutex);
            }
            // the first partial block is cached
            else
            {
                if(i == num_file_block - 1)				// it also is the last block
                {
                    read_partial_block_from_device(device_block_array[i], first_block_start_offset, last_block_end_offset, buf);
                    already_read_bytes += last_block_end_offset - first_block_start_offset + 1;
                }
                else
                {
                    read_partial_block_from_device(device_block_array[i], first_block_start_offset, G_BLOCK_SIZE_IN_BYTES - 1, buf);
                    already_read_bytes += G_BLOCK_SIZE_IN_BYTES - first_block_start_offset;
                }
            }

            i++;
        }
        // handle the last partial block
        else if(i == num_file_block - 1 && last_block_end_offset != G_BLOCK_SIZE_IN_BYTES - 1)
        {
            // the last partial block is missing
            if(device_block_array[i] == -1)
            {
                // read in the missing partial block
                char read_partial_block_buf[G_BLOCK_SIZE_IN_BYTES];
                get_blocks_from_fs(file->real_fd, file_block_array[i], 1, read_partial_block_buf);

                // read the needed data to buf
                memcpy(buf + G_BLOCK_SIZE_IN_BYTES - first_block_start_offset + G_BLOCK_SIZE_IN_BYTES * (i - 1),
                       read_partial_block_buf, last_block_end_offset + 1);
                already_read_bytes += last_block_end_offset + 1;

                // cache the block
                allocate_device_space(&device_block_array[i], 1);
                put_blocks_in_device(&device_block_array[i], 1, read_partial_block_buf);
                pthread_mutex_lock(&file->cache_tree_mutex);
                radix_tree_insert(&file->cache_tree, file_block_array[i], BLK_ID_TO_RDX_ID(device_block_array[i]));
                pthread_mutex_unlock(&file->cache_tree_mutex);
            }
            // the last partial block is cached
            else
            {
                read_partial_block_from_device(device_block_array[i], 0, last_block_end_offset,
                                               buf + G_BLOCK_SIZE_IN_BYTES - first_block_start_offset + G_BLOCK_SIZE_IN_BYTES * (i - 1));
                already_read_bytes += last_block_end_offset + 1;
            }

            i++;
        }

        // handle the integral block
        else
        {
            // the missing blocks
            if(device_block_array[i] == -1)
            {
                successive_start = file_block_array[i];
                successive_end = successive_start;
                // push the loop, find the last successive block
                for(j = i + 1; j<num_file_block; j++)
                {
                    // if this file_bock isn't cached and is adjacent with last file_block
                    if(device_block_array[j] == -1 && file_block_array[j] - file_block_array[j-1] == 1)
                    {
                        if(j == num_file_block - 1 && last_block_end_offset != G_BLOCK_SIZE_IN_BYTES - 1)	// the last block
                        {
                            // this last block is partial and need to handle differently
                            // can be handled by next while loop in if()
                            break;
                        }
                        else
                        {
                            successive_end++;
                            continue;
                        }
                    }
                    else
                    {
                        break;
                    }
                }
                int num_successive_block = successive_end - successive_start + 1;
                // read in current successive blocks
                get_blocks_from_fs(file->real_fd, successive_start, num_successive_block, buf + G_BLOCK_SIZE_IN_BYTES - first_block_start_offset + G_BLOCK_SIZE_IN_BYTES * (i - 1));
                already_read_bytes += G_BLOCK_SIZE_IN_BYTES * num_successive_block;
                // allocate device_block_id
                allocate_device_space(&device_block_array[i], num_successive_block);
                // cache these blocks in device
                put_blocks_in_device(&device_block_array[i], num_successive_block, buf + G_BLOCK_SIZE_IN_BYTES - first_block_start_offset + G_BLOCK_SIZE_IN_BYTES * (i - 1));

                // insert the cache radix tree
                int rdx_i;
                pthread_mutex_lock(&file->cache_tree_mutex);
                for(rdx_i=0; rdx_i<num_successive_block; rdx_i++)
                {
                    radix_tree_insert(&file->cache_tree, file_block_array[i+rdx_i], BLK_ID_TO_RDX_ID(device_block_array[i+rdx_i]));
                }
                pthread_mutex_unlock(&file->cache_tree_mutex);

                i = j;
            }
            // the cached blocks
            else
            {
                successive_start = file_block_array[i];
                successive_end = successive_start;
                // push the loop, find the last successive block
                for(j = i + 1; j<num_file_block; j++)
                {
                    // if this file_bock isn't cached and is adjacent with last file_block
                    if(device_block_array[j] != -1 && file_block_array[j] - file_block_array[j-1] == 1)
                    {
                        if(j == num_file_block - 1 && last_block_end_offset != G_BLOCK_SIZE_IN_BYTES - 1)	// the last block
                        {
                            // this last block is partial and need to handle differently
                            // can be handled by next while loop in if()
                            break;
                        }
                        else
                        {
                            successive_end++;
                            continue;
                        }
                    }
                    else
                    {
                        break;
                    }
                }


                int num_successive_block = successive_end - successive_start + 1;
                // read the cached blocks
                get_blocks_from_device(&device_block_array[i], num_successive_block, buf + G_BLOCK_SIZE_IN_BYTES - first_block_start_offset + G_BLOCK_SIZE_IN_BYTES * (i - 1));
                already_read_bytes += G_BLOCK_SIZE_IN_BYTES * num_successive_block;

                i = j;
            }
        }

    }


    free(device_block_array);
    return already_read_bytes;
}


static uint64_t write_ost_data(Ost_File *file, char* data_buf, uint64_t data_buf_len,
                               int* file_block_array, int num_file_block,
                               int first_block_start_offset, int last_block_end_offset)
{
    // inquire in the radix tree of file
    int *device_block_array = calloc(num_file_block, sizeof(int));
    void *rdx_id = NULL;
    //int blk_id;

    pthread_mutex_lock(&file->cache_tree_mutex);
    // TODO: gang_lookup
    int i;
    for(i=0; i<num_file_block; i++)
    {
        rdx_id = radix_tree_lookup(&file->cache_tree, file_block_array[i]);
        if(rdx_id != NULL)
        {
            device_block_array[i] = RDX_ID_TO_BLK_ID(rdx_id);
        }
        else
        {
            // not cached?
            device_block_array[i] = -1;
        }
    }
    pthread_mutex_unlock(&file->cache_tree_mutex);

    uint64_t already_written_bytes = 0;
    // handle the data
    int successive_start = -1;
    int successive_end = -1;
    int j;
    i = 0;
    while(i<num_file_block)
    {
        // handle the first partial block
        if(i == 0 && first_block_start_offset != 0)		// the first block is partial
        {
            // the first partial block is missing
            if(device_block_array[i] == -1)
            {
                // read in the missing partial block
                char read_partial_block_buf[G_BLOCK_SIZE_IN_BYTES] = {0};
                get_blocks_from_fs(file->real_fd, file_block_array[i], 1, read_partial_block_buf);

                // write updtate to the partial block
                if(i == num_file_block - 1)				// it also is the last block
                {
                    memcpy(read_partial_block_buf + first_block_start_offset, data_buf, last_block_end_offset - first_block_start_offset + 1);
                    already_written_bytes += last_block_end_offset - first_block_start_offset + 1;
                }
                else
                {
                    memcpy(read_partial_block_buf + first_block_start_offset, data_buf, G_BLOCK_SIZE_IN_BYTES - first_block_start_offset);
                    already_written_bytes += G_BLOCK_SIZE_IN_BYTES - first_block_start_offset;
                }

                // cache the block
                allocate_device_space(&device_block_array[i], 1);
                put_blocks_in_device(&device_block_array[i], 1, read_partial_block_buf);
                pthread_mutex_lock(&file->cache_tree_mutex);
                radix_tree_insert(&file->cache_tree, file_block_array[i], BLK_ID_TO_RDX_ID(device_block_array[i]));
                pthread_mutex_unlock(&file->cache_tree_mutex);
            }
            // the first partial block is cached
            else
            {
                if(i == num_file_block - 1)				// it also is the last block
                {
                    write_partial_block_in_device(device_block_array[i], first_block_start_offset, last_block_end_offset, data_buf);
                    already_written_bytes += last_block_end_offset - first_block_start_offset + 1;
                }
                else
                {
                    write_partial_block_in_device(device_block_array[i], first_block_start_offset, G_BLOCK_SIZE_IN_BYTES - 1, data_buf);
                    already_written_bytes += G_BLOCK_SIZE_IN_BYTES - first_block_start_offset;
                }
            }

            i++;
        }
        // handle the last partial block
        else if(i == num_file_block - 1 && last_block_end_offset != G_BLOCK_SIZE_IN_BYTES - 1)
        {
            // the last partial block is missing
            if(device_block_array[i] == -1)
            {
                // read in the missing partial block
                char read_partial_block_buf[G_BLOCK_SIZE_IN_BYTES];
                get_blocks_from_fs(file->real_fd, file_block_array[i], 1, read_partial_block_buf);

                // write updtate to the partial block
                memcpy(read_partial_block_buf, data_buf + G_BLOCK_SIZE_IN_BYTES - first_block_start_offset + G_BLOCK_SIZE_IN_BYTES * (i - 1),
                       last_block_end_offset + 1);
                already_written_bytes += last_block_end_offset + 1;

                // cache the block
                allocate_device_space(&device_block_array[i], 1);
                put_blocks_in_device(&device_block_array[i], 1, read_partial_block_buf);
                pthread_mutex_lock(&file->cache_tree_mutex);
                radix_tree_insert(&file->cache_tree, file_block_array[i], BLK_ID_TO_RDX_ID(device_block_array[i]));
                pthread_mutex_unlock(&file->cache_tree_mutex);
            }
            // the last partial block is cached
            else
            {
                write_partial_block_in_device(device_block_array[i], 0, last_block_end_offset,
                                              data_buf + G_BLOCK_SIZE_IN_BYTES - first_block_start_offset + G_BLOCK_SIZE_IN_BYTES * (i - 1));
                already_written_bytes += last_block_end_offset + 1;
            }

            i++;
        }

        // handle the integral block
        else
        {
            // the missing blocks
            if(device_block_array[i] == -1)
            {
                successive_start = file_block_array[i];
                successive_end = successive_start;
                // push the loop, find the last successive block
                for(j = i + 1; j<num_file_block; j++)
                {
                    // if this file_bock isn't cached and is adjacent with last file_block
                    if(device_block_array[j] == -1 && file_block_array[j] - file_block_array[j-1] == 1)
                    {
                        if(j == num_file_block - 1 && last_block_end_offset != G_BLOCK_SIZE_IN_BYTES - 1)	// the last block
                        {
                            // this last block is partial and need to handle differently
                            // can be handled by next while loop in if()
                            break;
                        }
                        else
                        {
                            successive_end++;
                            continue;
                        }
                    }
                    else
                    {
                        break;
                    }
                }
                int num_successive_block = successive_end - successive_start + 1;
                // allocate device_block_id
                allocate_device_space(&device_block_array[i], num_successive_block);
                // cache these blocks in device
                put_blocks_in_device(&device_block_array[i], num_successive_block, data_buf + G_BLOCK_SIZE_IN_BYTES - first_block_start_offset + G_BLOCK_SIZE_IN_BYTES * (i - 1));
                already_written_bytes += G_BLOCK_SIZE_IN_BYTES * num_successive_block;

                // insert the cache radix tree
                int rdx_i;
                pthread_mutex_lock(&file->cache_tree_mutex);
                for(rdx_i=0; rdx_i<num_successive_block; rdx_i++)
                {
                    radix_tree_insert(&file->cache_tree, file_block_array[i+rdx_i], BLK_ID_TO_RDX_ID(device_block_array[i+rdx_i]));
                }
                pthread_mutex_unlock(&file->cache_tree_mutex);

                i = j;
            }
            // the cached blocks
            else
            {
                successive_start = file_block_array[i];
                successive_end = successive_start;
                // push the loop, find the last successive block
                for(j = i + 1; j<num_file_block; j++)
                {
                    // if this file_bock isn't cached and is adjacent with last file_block
                    if(device_block_array[j] != -1 && file_block_array[j] - file_block_array[j-1] == 1)
                    {
                        if(j == num_file_block - 1 && last_block_end_offset != G_BLOCK_SIZE_IN_BYTES - 1)	// the last block
                        {
                            // this last block is partial and need to handle differently
                            // can be handled by next while loop in if()
                            break;
                        }
                        else
                        {
                            successive_end++;
                            continue;
                        }
                    }
                    else
                    {
                        break;
                    }
                }


                int num_successive_block = successive_end - successive_start + 1;
                // write updates to the blocks
                put_blocks_in_device(&device_block_array[i], num_successive_block, data_buf + G_BLOCK_SIZE_IN_BYTES - first_block_start_offset + G_BLOCK_SIZE_IN_BYTES * (i - 1));
                already_written_bytes += G_BLOCK_SIZE_IN_BYTES * num_successive_block;

                i = j;
            }
        }
    }

    return already_written_bytes;
}

static void read_ost_block_cache(Ost_File *file, char* buf, block_data_req *blk_req)		//buf must be of block size
{
    void *rdx_id = NULL;
    int device_block_id;
    pthread_mutex_lock(&file->cache_tree_mutex);
    rdx_id = radix_tree_lookup(&file->cache_tree, blk_req->file_block_id);
    if(rdx_id != NULL)
    {
        device_block_id = RDX_ID_TO_BLK_ID(rdx_id);		// convert void* to int just for convenience
    }
    else
    {
        // not cached
        device_block_id = -1;
    }
    pthread_mutex_unlock(&file->cache_tree_mutex);

    // cache miss
    if(device_block_id == -1)
    {
        get_blocks_from_fs(file->real_fd, blk_req->file_block_id, 1, buf);
        allocate_device_space(&device_block_id, 1);
        put_blocks_in_device(&device_block_id, 1, buf);

        pthread_mutex_lock(&file->cache_tree_mutex);
        radix_tree_insert(&file->cache_tree, blk_req->file_block_id, BLK_ID_TO_RDX_ID(device_block_id));
        pthread_mutex_unlock(&file->cache_tree_mutex);
    }
    // cache hit
    else
    {
        read_partial_block_from_device(device_block_id, blk_req->start_offset, blk_req->end_offset, buf + blk_req->start_offset);
    }
/*
	// once the request is from my own CN, then cache the ssd metadata.
	// because any request coming here didn't find the ssd metadata locally 
	if(config_param.metadata_caching == 1 && blk_req->requesting_srv_id == MY_SERVER_ID)
	{
		MetadataCachingEntry *get_entry = get_entry_metadata_cache(file->path, blk_req->file_block_id);			
		if(get_entry != NULL)
		{
			get_entry->device_block_id = device_block_id;
		}
		else
		{
			MetadataCachingEntry entry;
			entry.server_id = blk_req->requesting_srv_id;
			entry.device_block_id = device_block_id;
			put_entry_metadata_cache(file->path, blk_req->file_block_id, &entry);
		}
	}
*/
}

static void read_ost_block_direct(Ost_File *file, char* buf, block_data_req *blk_req)		//buf must be of block size
{
	int64_t offset = ((int64_t)blk_req->file_block_id) << G_BLOCK_SIZE_SHIFT + blk_req->start_offset;
	int64_t count = blk_req->end_offset - blk_req->start_offset + 1;
	//fprintf(stderr, "buf: %#X\t offset: %lld count: %lld\n", buf, offset, count);
	if(pread(file->real_fd, buf, count, offset) == -1)
	{
		perror("read_ost_block_direct error");
		exit(1);
	}
}


static void write_ost_block_cache(Ost_File *file, char* buf, block_data_req *blk_req)		//buf must be of block size
{
    void *rdx_id = NULL;
    int device_block_id;
    pthread_mutex_lock(&file->cache_tree_mutex);
    rdx_id = radix_tree_lookup(&file->cache_tree, blk_req->file_block_id);
    if(rdx_id != NULL)
    {
        device_block_id = RDX_ID_TO_BLK_ID(rdx_id);		// convert void* to int just for convenience
    }
    else
    {
        // not cached
        device_block_id = -1;
    }
    pthread_mutex_unlock(&file->cache_tree_mutex);

    // cache miss
    if(device_block_id == -1)
    {
        if(blk_req->start_offset == 0 && blk_req->end_offset == G_BLOCK_SIZE_IN_BYTES - 1)
        {
            allocate_device_space(&device_block_id, 1);
            put_blocks_in_device(&device_block_id, 1, buf);
        }
        else
        {
            // the read bytes may be smaller than what you want
            // ex. when write to a empty file
            uint64_t cur_offset = blk_req->file_block_id * G_BLOCK_SIZE_IN_BYTES + blk_req->start_offset;
			uint64_t cur_count = blk_req->end_offset - blk_req->start_offset + 1;
            if(cur_offset >= file->size)
            {
				// no need to read in, just write
				allocate_device_space(&device_block_id, 1);
				write_partial_block_in_device(device_block_id, blk_req->start_offset, blk_req->end_offset, buf + blk_req->start_offset);
            }
			else
			{
            	get_blocks_from_fs(file->real_fd, blk_req->file_block_id, 1, tmp_block_buf);
            	memcpy(tmp_block_buf + blk_req->start_offset, buf + blk_req->start_offset, blk_req->end_offset - blk_req->start_offset + 1);
            	allocate_device_space(&device_block_id, 1);
            	put_blocks_in_device(&device_block_id, 1, tmp_block_buf);
			}
        }
        pthread_mutex_lock(&file->cache_tree_mutex);
        radix_tree_insert(&file->cache_tree, blk_req->file_block_id, BLK_ID_TO_RDX_ID(device_block_id));
        pthread_mutex_unlock(&file->cache_tree_mutex);
    }
    // cache hit
    else
    {
        write_partial_block_in_device(device_block_id, blk_req->start_offset, blk_req->end_offset, buf + blk_req->start_offset);
    }
/*
	// once the request is from my own CN, then cache the ssd metadata.
	// because any request coming here didn't find the ssd metadata locally 
	if(config_param.metadata_caching == 1 && blk_req->requesting_srv_id == MY_SERVER_ID)
	{
		MetadataCachingEntry *get_entry = get_entry_metadata_cache(file->path, blk_req->file_block_id);			
		if(get_entry != NULL)
		{
			get_entry->device_block_id = device_block_id;
		}
		else
		{
			MetadataCachingEntry entry;
			entry.server_id = blk_req->requesting_srv_id;
			entry.device_block_id = device_block_id;
			put_entry_metadata_cache(file->path, blk_req->file_block_id, &entry);
		}
	}
*/
}

static void write_ost_block_direct(Ost_File *file, char* buf, block_data_req *blk_req)		//buf must be of block size
{
	int64_t offset = ((int64_t)blk_req->file_block_id) << G_BLOCK_SIZE_SHIFT + blk_req->start_offset;
	int64_t count = blk_req->end_offset - blk_req->start_offset + 1;
	if(pwrite(file->real_fd, buf, count, offset) == -1)
	{
		perror("write_ost_block_direct error");
		exit(1);
	}
}

static void write_block(Ost_File *file, char* buf, block_data_req *blk_req)		//buf must be of block size
{
	if(config_param.use_cache)
	{
		write_ost_block_cache(file, buf, blk_req);
	}
	else
	{
		write_ost_block_direct(file, buf, blk_req);
	}
}

static void read_block(Ost_File *file, char* buf, block_data_req *blk_req)		//buf must be of block size
{
	if(config_param.use_cache)
	{
		read_ost_block_cache(file, buf, blk_req);
	}
	else
	{
		read_ost_block_direct(file, buf, blk_req);
	}
}


// to support the first read in of uncached local data can be fast
static void prepare_for_first_cached_in_data(Ost_File *ost_file, int *metadata_array, int num_block, int start_block_id,
															long offset, size_t size, int is_read)
{
	int i;
	for(i=0; i<num_block; i++)
	{
		if(metadata_array[i] != MY_SERVER_ID) continue;
		
		int file_block_id = start_block_id + i;
		void *rdx_id = NULL;
    	int device_block_id;
    	pthread_mutex_lock(&ost_file->cache_tree_mutex);
    	rdx_id = radix_tree_lookup(&ost_file->cache_tree, file_block_id);
		if(rdx_id != NULL)
		{
        	device_block_id = RDX_ID_TO_BLK_ID(rdx_id);		// convert void* to int just for convenience
    	}
    	else
    	{
        	// not cached
        	device_block_id = -1;
    	}
    	pthread_mutex_unlock(&ost_file->cache_tree_mutex);

		if(is_read)
		{
			// cache miss
    		if(device_block_id == -1)
    		{
        		get_blocks_from_fs(ost_file->real_fd, file_block_id, 1, tmp_block_buf);
        		allocate_device_space(&device_block_id, 1);
        		put_blocks_in_device(&device_block_id, 1, tmp_block_buf);

        		pthread_mutex_lock(&ost_file->cache_tree_mutex);
        		radix_tree_insert(&ost_file->cache_tree, file_block_id, BLK_ID_TO_RDX_ID(device_block_id));
        		pthread_mutex_unlock(&ost_file->cache_tree_mutex);
    		}
    		// cache hit
    		else
    		{
				// let the client read
    		}
		}
		// write
		else
		{
			// cache miss
    		if(device_block_id == -1)
    		{
				int blk_start_offset = i == 0 ?
                           	offset % G_BLOCK_SIZE_IN_BYTES : 0;
        		int blk_end_offset = i == num_block - 1?
                         	(offset + size - 1) % G_BLOCK_SIZE_IN_BYTES : G_BLOCK_SIZE_IN_BYTES - 1;
        		if(blk_start_offset == 0 && blk_end_offset == G_BLOCK_SIZE_IN_BYTES - 1)
        		{
					// do nothing, let client write in the whole block
            		allocate_device_space(&device_block_id, 1);
        		}
        		else
        		{				
            		uint64_t cur_offset = file_block_id * G_BLOCK_SIZE_IN_BYTES + blk_start_offset;
            		if(cur_offset >= ost_file->size)
            		{
            			allocate_device_space(&device_block_id, 1);						
            		}
					else
					{
            			get_blocks_from_fs(ost_file->real_fd, file_block_id, 1, tmp_block_buf);
            			allocate_device_space(&device_block_id, 1);
            			put_blocks_in_device(&device_block_id, 1, tmp_block_buf);
					}
        		}
        		pthread_mutex_lock(&ost_file->cache_tree_mutex);
       	 		radix_tree_insert(&ost_file->cache_tree, file_block_id, BLK_ID_TO_RDX_ID(device_block_id));
        		pthread_mutex_unlock(&ost_file->cache_tree_mutex);
    		}
    		// cache hit
    		else
    		{
				// do nothing, let client do what he want
        	}
			update_ost_file_size(ost_file, offset, size);
		}
/*
		// cache the ssd metadata
		if(1 || config_param.metadata_caching == 1)
		{
			assert(device_block_id != -1);
			MetadataCachingEntry *get_entry = get_entry_metadata_cache(ost_file->path, file_block_id);			
			if(get_entry != NULL)
			{
				get_entry->device_block_id = device_block_id;
			}
			else
			{
				MetadataCachingEntry entry;
				entry.server_id = MY_SERVER_ID;
				entry.device_block_id = device_block_id;
				put_entry_metadata_cache(ost_file->path, file_block_id, &entry);
			}
		}
*/
	}
    
}


// RDMA
static void rdma_init()
{
    srv_rdmas = calloc(1, sizeof(rdma_endpoint) * config_param.SRV_RDMA_EP_NUM);

    glex_ret_t ret;
    size_t page_size = sysconf(_SC_PAGESIZE);
    uint32_t ep_mem_size = ((uint32_t) SIZE_RDMA_MEM_BLK) * config_param.SRV_MEM_BLK_PER_EP;

    int i;
    for(i=0; i<config_param.SRV_RDMA_EP_NUM; i++)
    {
        if (posix_memalign((void **)&srv_rdmas[i].ep_mem, page_size, ep_mem_size))
        {
            printf("error: cannot allocate memory\n");
            exit(1);
        }
        memset(srv_rdmas[i].ep_mem, 0, ep_mem_size);
        if(DEBUG >= 2) fprintf(stderr, "Init RDMA Memory: [%d Bytes]\n", ep_mem_size);

        ret = glex_init();
        if (ret != GLEX_SUCCESS)
        {
            printf("error: rdma_init(), return: %d\n", ret);
            exit(1);
        }

        srv_rdmas[i].ep_attr.key				= 0x66;
        srv_rdmas[i].ep_attr.cap.dq_capacity	= GLEX_EP_CAP_DQ_CAPACITY_DEFAULT;
        srv_rdmas[i].ep_attr.cap.cq_capacity	= GLEX_EP_CAP_CQ_CAPACITY_DEFAULT;
        srv_rdmas[i].ep_attr.cap.mpq_capacity	= GLEX_EP_CAP_MPQ_CAPACITY_DEFAULT;

        uint32_t local_ep_num = GLEX_ANY_EP_NUM;

        ret = glex_create_ep(0, local_ep_num, &srv_rdmas[i].ep_attr, &srv_rdmas[i].ep);
        if (ret != GLEX_SUCCESS)
        {
            printf("error: _create_ep(), return: %d\n", ret);
            exit(1);
        }

        glex_get_ep_addr(srv_rdmas[i].ep, &srv_rdmas[i].ep_addr);

        ret = glex_register_mem(srv_rdmas[i].ep, srv_rdmas[i].ep_mem, ep_mem_size, &srv_rdmas[i].mh);
        if (ret != GLEX_SUCCESS)
        {
            printf("error: _register_mem(), return: %d\n", ret);
            exit(1);
        }

        pthread_mutex_init(&srv_rdmas[i].trans_mutex, NULL);
    }

    MAX_NUM_SRV_RDMA_MEM_BLOCK = config_param.SRV_MEM_BLK_PER_EP * config_param.SRV_RDMA_EP_NUM;
    srv_mem_blocks = calloc(1, sizeof(rdma_mem_block) * MAX_NUM_SRV_RDMA_MEM_BLOCK);

	
	for(i=0; i<MAX_NUM_SRV_RDMA_MEM_BLOCK; i++)
    {
        srv_mem_blocks[i].is_free = 1;
        srv_mem_blocks[i].id = i;
        srv_mem_blocks[i].endpoint = &srv_rdmas[i / config_param.SRV_MEM_BLK_PER_EP];
        srv_mem_blocks[i].mem = srv_mem_blocks[i].endpoint->ep_mem + (i % config_param.SRV_MEM_BLK_PER_EP) * SIZE_RDMA_MEM_BLK;
        assert(srv_mem_blocks[i].mem);
		
    }
    num_free_srv_rdma_mem_block = MAX_NUM_SRV_RDMA_MEM_BLOCK;
}

static void rdma_finalize()
{
    int i;
    for(i=0; i<config_param.SRV_RDMA_EP_NUM; i++)
    {
        free(srv_rdmas[i].ep_mem);
    }
    free(srv_rdmas);
    free(srv_mem_blocks);
}

static int last_assign_mem_blk = -1;
static rdma_mem_block* get_mem_block()
{
    rdma_mem_block* ret = NULL;
    pthread_mutex_lock(&srv_mem_blocks_mutex);

    if(num_free_srv_rdma_mem_block == 0)
    {
        ret = NULL;
    }
    else
    {
        int count, cur_blk;
        for(count=0; count<MAX_NUM_SRV_RDMA_MEM_BLOCK; count++)
        {
            cur_blk = (last_assign_mem_blk + 1 + count) % MAX_NUM_SRV_RDMA_MEM_BLOCK;
            if(srv_mem_blocks[cur_blk].is_free)
            {
                ret = &srv_mem_blocks[cur_blk];
                srv_mem_blocks[cur_blk].is_free = 0;
                num_free_srv_rdma_mem_block --;
                last_assign_mem_blk = cur_blk;
			    break;
            }
        }
    }
    pthread_mutex_unlock(&srv_mem_blocks_mutex);

    return ret;
}

static void release_mem_block(int mem_block_id)
{
    assert(mem_block_id >= 0 && mem_block_id < MAX_NUM_SRV_RDMA_MEM_BLOCK);

    pthread_mutex_lock(&srv_mem_blocks_mutex);
    assert(srv_mem_blocks[mem_block_id].is_free == 0);
    srv_mem_blocks[mem_block_id].is_free = 1;
    num_free_srv_rdma_mem_block ++;
    if(num_free_srv_rdma_mem_block == 1)
    {
        //pthread_cond_signal(&srv_mem_blocks_cond);
    }
    pthread_mutex_unlock(&srv_mem_blocks_mutex);
}

/********************** Metadata Caching Start *******************************/

SHF * metadata_caching_shf;
void metadata_caching_init()
{
    char metadata_caching_file_folder[] = "/dev/shm";
    char metadata_caching_file_name[256];

    pid_t pid = getpid();
    SHF_SNPRINTF(1, metadata_caching_file_name, "Metadata_Caching", pid);
    shf_init  ();
    metadata_caching_shf = shf_attach(metadata_caching_file_folder, metadata_caching_file_name, 1 /* delete upon process exit */);
    shf_set_data_need_factor(250);	// ?
}

void metadata_caching_finalize()
{
    shf_detach(metadata_caching_shf);
}

static char key_buf[PATH_MAX + 8];
static void put_entry_metadata_cache(char * path, int file_block_id, MetadataCachingEntry *entry)
{
    memset(key_buf, 0, PATH_MAX + 8);
    strcpy(key_buf, path);
    memcpy(key_buf + PATH_MAX, &file_block_id, sizeof(file_block_id));

    shf_make_hash(key_buf, PATH_MAX + 8);
    shf_put_key_val(metadata_caching_shf, SHF_CAST(const char *, entry), sizeof(MetadataCachingEntry));
}

static MetadataCachingEntry * get_entry_metadata_cache(char * path, int file_block_id)
{
    memset(key_buf, 0, PATH_MAX + 8);
    strcpy(key_buf, path);
    memcpy(key_buf + PATH_MAX, &file_block_id, sizeof(file_block_id));

    shf_make_hash(key_buf, PATH_MAX + 8);
    MetadataCachingEntry *entry = shf_get_key_val_addr(metadata_caching_shf);

    if(entry == NULL)
    {
        //fprintf(stderr, "Error: Can not find the metadata of %s, file_block[%d] in cache\n", path, file_block_id);
        return NULL;
    }
    else
        return entry;
}

static void del_entry_metadata_cache(char * path, int file_block_id)
{
    memset(key_buf, 0, PATH_MAX + 8);
    strcpy(key_buf, path);
    memcpy(key_buf + PATH_MAX, &file_block_id, sizeof(file_block_id));

    shf_make_hash(key_buf, PATH_MAX + 8);
    shf_del_key_val(metadata_caching_shf);
}


/********************** Metadata Caching End *******************************/



void main_finalize()
{
    cache_device_finalize();
    mfd_array_finalize();
    rdma_finalize();
}

int main(int argc, char **argv)
{
    // JAY CODE
    read_config_file(&config_param);
	DEBUG = config_param.server_debug_level;
	// JAY CODE END
	
    int retval = EXIT_SUCCESS;
    /* initialize main thread libevent instance */

    global_settings_init();
    //printf("global settings init ok\n");
    //assoc_init(0);
    //printf("assoc init ok\n");
    //slabs_init(global_settings.maxbytes,global_settings.factor,NULL);
    //printf("slabs_init ok\n");
    //file_table_init();
    //printf("file table init ok\n");
    hash_init(hash_type);
    //printf("hash init ok\n");

    //file_mapping_init();
    //printf("file mapping init finished!\n");

    // JAY CODE
    mfd_array_init();
    open_mds_file_table_init();
    ost_file_table_init();
    rdma_init();

    my_server_id_init();
	if(server_is_ion(MY_SERVER_ID, config_param.ion_modulus))
	{
    	cache_device_init();
	}

    //metadata_caching_init();
    // JAY CODE END

    bool tcp_specified = false;
    bool udp_specified = false;


    if (tcp_specified && !udp_specified)
    {
        global_settings.udpport = global_settings.port;
    }
    else if (udp_specified && !tcp_specified)
    {
        global_settings.port = global_settings.udpport;
    }


    struct rlimit rlim;
    /*
       * If needed, increase rlimits to allow as many connections
       * as needed.
       */

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0)
    {
        fprintf(stderr, "failed to getrlimit number of files\n");
        exit(EX_OSERR);
    }
    else
    {
        rlim.rlim_cur = global_settings.maxconns;
        rlim.rlim_max = global_settings.maxconns;
        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0)
        {
            fprintf(stderr, "failed to set rlimit for open files. Try starting as root or requesting smaller maxconns value.\n");
            exit(EX_OSERR);
        }
    }


    main_base = event_init();
    conn_init();
    thread_init(global_settings.num_threads, main_base);
	
    char domain_name[8];
    gethostname(domain_name, sizeof(domain_name));

	/*
    char *trace_filename = getenv("AFAC_TRACE_FILE");
    char temp_trace_filename[PATH_MAX];
    if(trace_filename != NULL)
    {
        snprintf(temp_trace_filename, sizeof(temp_trace_filename),"%s",trace_filename);
        strncat(temp_trace_filename,".",1);
        strncat(temp_trace_filename, domain_name, strlen(domain_name));
        printf("The trace file name is %s\n", temp_trace_filename);
        trace_file = fopen(temp_trace_filename,"a");
        if(trace_file == NULL)
        {
            printf("Failed to open \"%s\": %s\n", temp_trace_filename, strerror(errno));
        }
    }
	*/

    if(global_settings.socketpath == NULL)
    {
        const char *portnumber_filename = getenv("AFAC_PORT_NUMBER");
        char temp_portnumber_filename[PATH_MAX];
        FILE *portnumber_file = NULL;
        if(portnumber_filename != NULL)
        {
            /*.lck is file extension name */
            snprintf(temp_portnumber_filename, sizeof(temp_portnumber_filename),
                     "%s.lck", portnumber_filename);

            portnumber_file = fopen(temp_portnumber_filename, "a");
            if(portnumber_file == NULL)
            {
                fprintf(stderr, "Failed to open \"%s\": %s\n",
                        temp_portnumber_filename, strerror(errno));
            }
        }

        errno = 0;
        if(global_settings.port && server_sockets(global_settings.port,tcp_transport,
                portnumber_file))
        {
            /**/
            fprintf(stderr, "failed to listen on TCP port %d\n",global_settings.port);
            exit(EX_OSERR);
        }


        errno = 0;
        if(global_settings.udpport && server_sockets(global_settings.port,udp_transport,
                portnumber_file))
        {
            /**/
            fprintf(stderr, "failed to listen on UDP port %d\n",global_settings.udpport);
            exit(EX_OSERR);
        }

        if(portnumber_file)
        {
            fclose(portnumber_file);
            rename(temp_portnumber_filename, portnumber_filename);
        }
    }
    usleep(1000);

    /* enter the event loop */
    if (event_base_loop(main_base, 0) != 0)
    {
        retval = EXIT_FAILURE;
    }
    fclose(trace_file);

    main_finalize();
    return 0;
}
