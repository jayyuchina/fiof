#ifndef _AFAC_H
#define _AFAC_H

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <event.h>
#include <netdb.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <linux/limits.h>

#include "radix-tree.h"
#include "../include/protocol_binary.h"
#include "util.h"

/* Slab sizing definitions */
#define POWER_SMALLEST 1
#define POWER_LARGEST  200
#define CHUNK_ALIGN_BYTES 4096 /*one page, smaller than one page is not used*/
#define MAX_NUMBER_OF_SLAB_CLASSES (POWER_LARGEST + 1 + 1)

#define ITEM_LINKED 1
#define ITEM_CAS 2
/* temp */
#define ITEM_SLABBED 4

#define ITEM_FETCHED 8

#define HASHPOWER_DEFAULT 20
#define ENDIAN_LITTLE 1

#ifndef RADIX_GRA
#define RADIX_GRA 4096
#endif


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

/*Used for file cached in memory*/
#define FILE_DELAY_TIME 15*60

/** Initial size of list of items being returned by "get". */
#define ITEM_LIST_INITIAL 200

/** Initial size of list of CAS suffixes appended to "gets" lines. */
#define SUFFIX_LIST_INITIAL 20

/** Initial size of the sendmsg() scatter/gather array. */
#define IOV_LIST_INITIAL 400

/** Initial number of sendmsg() argument structures to allocate. */
#define MSG_LIST_INITIAL 10




typedef unsigned int rel_time_t;

extern struct settings global_settings;
extern enum hashfunc_type hash_type;

enum protocol
{
    ascii_prot = 3, /* arbitrary value. */
    binary_prot,
    negotiating_prot /* Discovering the protocol */
};

typedef struct data_item
{
    struct data_item *next;
    struct data_item *prev;
    struct data_item *f_next;
    struct data_item *f_prev;
    struct data_item *radix_next;
    rel_time_t	time; /* least recent access */
    rel_time_t	exptime; /* expire time */
    unsigned short refcount;
    int			nbytes; /* size of data */
    size_t		offset; /* offset of this data item in file */
    size_t		length;
    size_t		block_offset;
    int			io_flags;
    uint8_t		slabs_clsid; /* which slab class we are in */
    uint8_t		it_flags;	/* ITEM_* above */
    void * file_owner;
    void * data;
} item;

typedef struct file_cache
{
    unsigned long fd;			/* file hander or ... file identification, when file closed this is not right any more */
    item* f_header;			/* list header of cached item */
    item* f_tail;			/* list tail of cached item*/
    struct radix_tree_root root;		/* radix tree root for searching */
    struct file_cache * h_next;		/* two files have the same hash value*/
    struct file_cache *prev; /*all the cached file will be listed together*/
    struct file_cache *next;

    int file_cache_flags;
    char file_name[PATH_MAX];
    int path_length;
    int file_opened;
    struct timeval l_time;
    unsigned int exp_time;

    pthread_mutex_t file_object_lock;
} file_cache_t;

struct file_mapping
{
    FILE *fptr;
    int count;
    int flag;

    char file_name[PATH_MAX];
    int path_length;
    pthread_mutex_t file_unit_lock; /*when a file is openned by multiple client or process,
									this struct may be modified*/
};

struct settings
{
    size_t maxbytes; 		/* max memory allocate for caching */
    int item_size_max; 		/* Maximum item size, and upper end for slabs */
    bool slab_reassign;     /* Whether or not slab reassignment is allowed */
    int chunk_size;
    double factor;
    int verbose;

    char *hash_algorithm;
    char *inter;			/*ip address should be binded*/
    int backlog;			/*used to communicate  listen(sfd, settings.backlog) == -1 */
    int num_threads_per_udp; /* number of worker threads serving each udp socket */
    enum protocol binding_protocol;
    int num_threads;        /* number of worker (without dispatcher) libevent threads to run */
    int reqs_per_event;     /* Maximum number of io to process on each io-event. */
    int maxconns;			/*used, maximum TCP connections*/
    int max_open_files;
    bool maxconns_fast;     /* Whether or not to early close connections */
    char *socketpath;		/* path to unix socket if using local socket */
    int port;
    int udpport;

	// JAY
	int BLOCK_SIZE_IN_KB;	// my block size for ssd
	int BLOCK_SIZE_SHIFT;	// shift for block
};


struct stats_t
{
    pthread_mutex_t mutex;
    unsigned int  curr_items;
    unsigned int  total_items;
    uint64_t      curr_bytes;
    unsigned int  curr_conns;		/*used*/
    unsigned int  total_conns;
    uint64_t      rejected_conns;	/*used*/
    uint64_t      malloc_fails;
    unsigned int  reserved_fds;		/*used*/
    unsigned int  conn_structs;
    uint64_t      get_cmds;
    uint64_t      set_cmds;
    uint64_t      touch_cmds;
    uint64_t      get_hits;
    uint64_t      get_misses;
    uint64_t      touch_hits;
    uint64_t      touch_misses;
    uint64_t      evictions;
    uint64_t      reclaimed;
    time_t        started;          /* when the process was started */
    bool          accepting_conns;  /* whether we are currently accepting */
    uint64_t      listen_disabled_num;
    unsigned int  hash_power_level; /* Better hope it's not over 9000 */
    uint64_t      hash_bytes;       /* size used for hash tables */
    bool          hash_is_expanding; /* If the hash table is being expanded */
    uint64_t      expired_unfetched; /* items reclaimed but never touched */
    uint64_t      evicted_unfetched; /* items evicted but never touched */
    bool          slab_reassign_running; /* slab reassign in progress */
    uint64_t      slabs_moved;       /* times slabs were moved around */
    bool          lru_crawler_running; /* crawl in progress */
};




enum network_transport
{
    local_transport, /* Unix sockets*/
    tcp_transport,
    udp_transport
};

enum item_lock_types
{
    ITEM_LOCK_GRANULAR = 0,
    ITEM_LOCK_GLOBAL
};


enum conn_states
{
    conn_listening,  /**< the socket which listens for connections */
    conn_new_cmd,    /**< Prepare connection for next command */
    conn_waiting,    /**< waiting for a readable socket */
    conn_read,       /**< reading in a command line */
    conn_parse_cmd,  /**< try to parse a command from the input buffer */
    conn_write,      /**< writing out a simple response */
    conn_nread,      /**< reading in a fixed number of bytes */
    conn_swallow,    /**< swallowing unnecessary bytes w/o storing */
    conn_closing,    /**< closing this connection */
    conn_mwrite,     /**< writing out many items sequentially */
    conn_closed,     /**< connection is closed */
    conn_max_state   /**< Max state value (used for assertion) */
};


enum bin_substates
{
    bin_no_state,
    bin_reading_set_header,
    bin_reading_cas_header,
    bin_read_set_value,
    bin_reading_get_key,
    bin_reading_stat,
    bin_reading_del_header,
    bin_reading_incr_header,
    bin_read_flush_exptime,
    bin_reading_sasl_auth,
    bin_reading_sasl_auth_data,
    bin_reading_touch_key,
};

struct thread_stats
{
    pthread_mutex_t   mutex;
    uint64_t          get_cmds;
    uint64_t          get_misses;
    uint64_t          touch_cmds;
    uint64_t          touch_misses;
    uint64_t          delete_misses;
    uint64_t          incr_misses;
    uint64_t          decr_misses;
    uint64_t          cas_misses;
    uint64_t          bytes_read;
    uint64_t          bytes_written;
    uint64_t          flush_cmds;
    uint64_t          conn_yields; /* # of yields for connections (-R option)*/
    uint64_t          auth_cmds;
    uint64_t          auth_errors;
    /* struct slab_stats slab_stats[MAX_NUMBER_OF_SLAB_CLASSES]; */
};


typedef struct
{
    pthread_t thread_id;        			/* unique ID of this thread */
    struct event_base *base;    			/* libevent handle this thread uses */
    struct event notify_event;  			/* listen event for notify pipe */
    int notify_receive_fd;      			/* receiving end of notify pipe */
    int notify_send_fd;         			/* sending end of notify pipe */
    struct thread_stats stats;  			/* Stats generated by this thread */
    struct conn_queue *new_conn_queue; 		/* queue of new connections to handle */
    /*cache_t *suffix_cache;       suffix cache */

    uint8_t item_lock_type;    				/* use fine-grained or global item lock */
    struct thread_file_trace file_trace_m;	/* memory allocated for file trace*/
} LIBEVENT_THREAD;

typedef struct
{
    pthread_t thread_id;        /* unique ID of this thread */
    struct event_base *base;    /* libevent handle this thread uses */
} LIBEVENT_DISPATCHER_THREAD;




#define IS_UDP(x) (x == udp_transport)

typedef struct conn_t conn;
struct conn_t
{
    int    sfd;
    /* sasl_conn_t *sasl_conn; */
    bool authenticated;
    enum conn_states  state;
    enum bin_substates substate;
    rel_time_t last_cmd_time;
    struct event event;
    short  ev_flags;
    short  which;   /** which events were just triggered */

    char   *rbuf;   /** buffer to read commands into */
    char   *rcurr;  /** but if we parsed some already, this is where we stopped */
    int    rsize;   /** total allocated size of rbuf */
    int    rbytes;  /** how much data, starting from rcur, do we have unparsed */
    int    need_ntoh;

    char   *wbuf;
    char   *wcurr;
    int    wsize;
    int    wbytes;
    /** which state to go into after finishing current write */
    enum conn_states  write_and_go;
    void   *write_and_free; /** free this memory after finishing writing */

    char   *ritem;  /** when we read in an item's value, it goes here */
    int    rlbytes;

    /* data for the nread state */

    /**
     * item is used to hold an item structure created after reading the command
     * line of set/add/replace commands, but before we finished reading the actual
     * data. The data is read into ITEM_data(item) to avoid extra copying.
     */

    void   *item;     /* for commands set/add/replace  */

    /* data for the swallow state */
    int    sbytes;    /* how many bytes to swallow */

    /* data for the mwrite state */
    struct iovec *iov;
    int    iovsize;   /* number of elements allocated in iov[] */
    int    iovused;   /* number of elements used in iov[] */

    struct msghdr *msglist;
    int    msgsize;   /* number of elements allocated in msglist[] */
    int    msgused;   /* number of elements used in msglist[] */
    int    msgcurr;   /* element in msglist[] being transmitted now */
    int    msgbytes;  /* number of bytes in current msg */

    item   **ilist;   /* list of items to write out */
    int    isize;
    item   **icurr;
    int    ileft;

    char   **suffixlist;
    int    suffixsize;
    char   **suffixcurr;
    int    suffixleft;

    enum protocol protocol;   /* which protocol this connection speaks */
    enum network_transport transport; /* what transport is used by this connection */

    /* data for UDP clients */
    int    request_id; /* Incoming UDP request ID, if this is a UDP "connection" */
    struct sockaddr_in request_addr; /* udp: Who sent the most recent request */
    socklen_t request_addr_size;
    unsigned char *hdrbuf; /* udp packet headers */
    int    hdrsize;   /* number of headers' worth of space is allocated */

    bool   noreply;   /* True if the reply should not be sent. */
    /* current stats command */
    struct
    {
        char *buffer;
        size_t size;
        size_t offset;
    } stats;

    /* Binary protocol stuff */
    /* This is where the binary header goes */
    protocol_binary_request_header binary_header;
    uint64_t cas; /* the cas to return */
    short cmd; /* current command being processed */
    uint64_t body_len;
    uint64_t r_len;

    char * assemb_buf;
    char * assemb_curr;
    int assemb_size;

    uint8_t frag_flag[256];
    int frag_num;
    int frag_lack;

    struct io_trace request_trace;
    conn   *next;     /* Used for generating a list of conn structures */
    LIBEVENT_THREAD *thread; /* Pointer to the thread object serving this connection */
};





int item_read(unsigned long fd, unsigned long offset, uint64_t length, void *data_buffer);
int item_write(unsigned long fd, unsigned long offset, uint64_t length, void *data_buffer);

void global_settings_init();

static inline int mutex_lock(pthread_mutex_t *mutex)
{
    while (pthread_mutex_trylock(mutex));
    return 0;
}

#define mutex_unlock(x) pthread_mutex_unlock(x)




/********************************************************************/
/**************************   JAY   ***********************************/


typedef struct _file_open_args
{
	int		is_occupied;

	int		arg_num;
    char	path_name[PATH_MAX];
	int		flag;
	int		mode;
} file_open_args;

typedef struct
{
	pthread_mutex_t block_tree_mutex;
	struct radix_tree_root block_tree;		// store the block metadata

	size_t access_time;			/* time of last file accesse */	
	size_t dirty_time;	/* time of last write */
	size_t f_count;			/* reference count */	

	int being_writeback;
	
} Mds_File;

typedef struct
{
	int		is_occupied;

	// file open args
	int		arg_num;
    char	path_name[PATH_MAX];
	int		flag;
	int		mode;

	// File
	Mds_File *file;
} mfd_array_entry;

typedef struct
{
	pthread_mutex_t cache_tree_mutex;
	struct radix_tree_root cache_tree;		// store the block metadata
	
	char	path[PATH_MAX];
	int		real_fd;
	size_t	size;
} Ost_File;

#endif
