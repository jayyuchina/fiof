#include "afac.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <event.h>
#include "assoc.h"
#include "thread.h"
#include "file_table.h"
#include <event2/event.h>
#include "hash.h"

#define ITEMS_PER_ALLOC 64


/* An item in the connection queue. */
typedef struct conn_queue_item CQ_ITEM;
struct conn_queue_item
{
    int               sfd;
    enum conn_states  init_state;
    int               event_flags;
    int               read_buffer_size;
    enum network_transport     transport;
    CQ_ITEM          *next;
};


/* A connection queue. */
typedef struct conn_queue CQ;
struct conn_queue
{
    CQ_ITEM *head;
    CQ_ITEM *tail;
    pthread_mutex_t lock;
};

extern file_table_t file_item_table;
extern pthread_mutex_t *slabs_lock;

/* Lock for cache operations (item_*, assoc_*) */
pthread_mutex_t cache_lock;

/* Connection lock around accepting new connections */
pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;

/* Free list of CQ_ITEM structs */
static CQ_ITEM *cqi_freelist;
static pthread_mutex_t cqi_freelist_lock;


static LIBEVENT_DISPATCHER_THREAD dispatcher_thread;

/*
 * Each libevent instance has a wakeup pipe, which other threads
 * can use to signal that they've put a new connection on its queue.
 */
static LIBEVENT_THREAD *threads;


static pthread_mutex_t *item_locks;

static uint32_t item_lock_count;
#define hashsize(n) ((unsigned long int)1<<(n))
#define hashmask(n) (hashsize(n)-1)
/* this lock is temporarily engaged during a hash table expansion, may be not used by myself---dwr */
static pthread_mutex_t item_global_lock;
/* thread-specific variable for deeply finding the item lock type */
static pthread_key_t item_lock_type_key;


/* Lock for global stats */
static pthread_mutex_t stats_lock;

/*
 * Number of worker threads that have finished setting themselves up.
 */
static int init_count = 0;
static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;

/*used for assoc_insert and assoc_delete*/
pthread_mutex_t file_lock;

static pthread_mutex_t *file_table_locks;
static uint32_t file_lock_count;

/*used for file find operations*/
pthread_mutex_t *file_find_open_clean_locks;
static uint64_t file_find_lock_count;

pthread_mutex_t *item_heads_tails_locks = NULL;
static uint32_t item_heads_tails_locks_count;



static void thread_libevent_process(int fd, short which, void *arg);
static void cleaner_thread_libevent_process(int fd, short which, void *arg);

extern conn *conn_new(const int sfd, enum conn_states init_state,
                      const int event_flags,
                      const int read_buffer_size, enum network_transport transport,
                      struct event_base *base);

static void wait_for_thread_registration(int nthreads)
{
    while (init_count < nthreads)
    {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}

static void register_thread_initialized(void)
{
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
}



void *item_trylock(uint32_t hv)
{
    pthread_mutex_t *lock = &item_locks[(hv & hashmask(hashpower)) % item_lock_count];
    if(pthread_mutex_trylock(lock) == 0)
    {
        return lock;
    }
    return NULL;
}

#if !defined(HAVE_GCC_ATOMICS) && !defined(__sun)
pthread_mutex_t atomics_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif


unsigned short refcount_incr(unsigned short *refcount)
{
#ifdef HAVE_GCC_ATOMICS
    return __sync_add_and_fetch(refcount, 1);
#elif defined(__sun)
    return atomic_inc_ushort_nv(refcount);
#else
    unsigned short res;
    mutex_lock(&atomics_mutex);
    (*refcount)++;
    res = *refcount;
    mutex_unlock(&atomics_mutex);
    return res;
#endif
}

unsigned short refcount_decr(unsigned short *refcount)
{
#ifdef HAVE_GCC_ATOMICS
    return __sync_sub_and_fetch(refcount, 1);
#elif defined(__sun)
    return atomic_dec_ushort_nv(refcount);
#else
    unsigned short res;
    mutex_lock(&atomics_mutex);
    (*refcount)--;
    res = *refcount;
    mutex_unlock(&atomics_mutex);
    return res;
#endif
}



/*
 * Initializes a connection queue.
 */
static void cq_init(CQ *cq)
{
    pthread_mutex_init(&cq->lock, NULL);
    cq->head = NULL;
    cq->tail = NULL;
}

/*
 * Looks for an item on a connection queue, but doesn't block if there isn't
 * one.
 * Returns the item, or NULL if no item is available
 */
static CQ_ITEM *cq_pop(CQ *cq)
{
    CQ_ITEM *item;

    pthread_mutex_lock(&cq->lock);
    item = cq->head;
    if (NULL != item)
    {
        cq->head = item->next;
        if (NULL == cq->head)
            cq->tail = NULL;
    }
    pthread_mutex_unlock(&cq->lock);

    return item;
}

/*
 * Adds an item to a connection queue.
 */
static void cq_push(CQ *cq, CQ_ITEM *item)
{
    item->next = NULL;

    pthread_mutex_lock(&cq->lock);
    if (NULL == cq->tail)
        cq->head = item;
    else
        cq->tail->next = item;
    cq->tail = item;
    pthread_mutex_unlock(&cq->lock);
}


/*
 * Returns a fresh connection queue item.
 */
static CQ_ITEM *cqi_new(void)
{
    CQ_ITEM *item = NULL;
    pthread_mutex_lock(&cqi_freelist_lock);
    if (cqi_freelist)
    {
        item = cqi_freelist;
        cqi_freelist = item->next;
    }
    pthread_mutex_unlock(&cqi_freelist_lock);

    if (NULL == item)
    {
        int i;

        /* Allocate a bunch of items at once to reduce fragmentation */
        item = malloc(sizeof(CQ_ITEM) * ITEMS_PER_ALLOC);
        if (NULL == item)
        {
            return NULL;
        }

        /*
             * Link together all the new items except the first one
             * (which we'll return to the caller) for placement on
             * the freelist.
             */
        for (i = 2; i < ITEMS_PER_ALLOC; i++)
            item[i - 1].next = &item[i];

        pthread_mutex_lock(&cqi_freelist_lock);
        item[ITEMS_PER_ALLOC - 1].next = cqi_freelist;
        cqi_freelist = &item[1];
        pthread_mutex_unlock(&cqi_freelist_lock);
    }

    return item;
}

/*
 * Frees a connection queue item (adds it to the freelist.)
 */
static void cqi_free(CQ_ITEM *item)
{
    pthread_mutex_lock(&cqi_freelist_lock);
    item->next = cqi_freelist;
    cqi_freelist = item;
    pthread_mutex_unlock(&cqi_freelist_lock);
}


/*
 * Creates a worker thread.
 */
static void create_worker(void *(*func)(void *), void *arg)
{
    pthread_t       thread;
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&thread, &attr, func, arg)) != 0)
    {
        fprintf(stderr, "Can't create thread: %s\n",
                strerror(ret));
        exit(1);
    }
}

/*
 * Sets whether or not we accept new connections.
 */
void accept_new_conns(const bool do_accept)
{
    pthread_mutex_lock(&conn_lock);
    do_accept_new_conns(do_accept);
    pthread_mutex_unlock(&conn_lock);
}

/* Which thread we assigned a connection to most recently. */
static int last_thread = -1;

/****************************** LIBEVENT THREADS *****************************/

/*
 * Set up a thread's information.
 */
static void setup_thread(LIBEVENT_THREAD *me)
{
    me->base = event_init();
    if (! me->base)
    {
        fprintf(stderr, "Can't allocate event base\n");
        exit(1);
    }

    if(me->file_trace_m.file_trace == NULL)
    {
        int trace_num = TRACE_MEMORY_PER_THREAD/sizeof(struct io_trace);
        me->file_trace_m.file_trace = calloc(trace_num, sizeof(struct io_trace));
        if(me->file_trace_m.file_trace == NULL)
        {
            printf("Memory used for file trace alloc failed at thread %d\n", pthread_self());
            me->file_trace_m.max = 0;
        }
        me->file_trace_m.max = trace_num;
        me->file_trace_m.count = 0;
    }

    /* Listen for notifications from other threads */
    event_set(&me->notify_event, me->notify_receive_fd,
              EV_READ | EV_PERSIST, thread_libevent_process, me);
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1)
    {
        fprintf(stderr, "Can't monitor libevent notify pipe\n");
        exit(1);
    }

    me->new_conn_queue = malloc(sizeof(struct conn_queue));
    if (me->new_conn_queue == NULL)
    {
        perror("Failed to allocate memory for connection queue");
        exit(EXIT_FAILURE);
    }
    cq_init(me->new_conn_queue);

    if (pthread_mutex_init(&me->stats.mutex, NULL) != 0)
    {
        perror("Failed to initialize mutex");
        exit(EXIT_FAILURE);
    }

}
/*cleaner thread variable and set*/
static struct event cleaner_timer;
static struct timeval tv;
static setup_cleaner_thread(LIBEVENT_THREAD *me)
{
    me->base = event_init();

    if (! me->base)
    {
        fprintf(stderr, "Can't allocate event base\n");
        exit(1);
    }

    tv.tv_sec = 15*60;
    tv.tv_usec = 0;

    evtimer_set(&cleaner_timer, cleaner_thread_libevent_process, 0);
    event_base_set(me->base, &cleaner_timer);
    evtimer_add(&cleaner_timer, &tv);
}

static void clean_file(file_cache_t *search)
{
    uint64_t hv = hash(search->file_name, search->path_length);
    hv = hv & hashmask(hashpower + 1);

    pthread_mutex_lock(&file_find_open_clean_locks[hv]);

    item *it, *ptr;
    int fd;
    it = search->f_header;
    fd = open(search->file_name, O_RDWR);
    while(it)
    {
        ptr = it;
        if(ptr->io_flags == 1)
        {
            write(fd, ptr->data + ptr->offset - ptr->block_offset, ptr->length);

        }
        it = it->f_next;
        do_item_unlink(ptr, search);
    }
    fsync(fd);
    close(fd);
    file_item_delete_with_lock(search);

    pthread_mutex_unlock(&file_find_open_clean_locks[hv]);

}
static void cleaner_thread_libevent_process(int fd, short which, void *arg)
{
    file_cache_t * search = file_item_table.alloc_list_tail;
    struct timeval cur_time;
    gettimeofday(&cur_time, NULL);
    while (search)
    {
        if(search->file_opened == 0 &&
                (cur_time.tv_sec - search->l_time.tv_sec >= search->exp_time))
        {
            clean_file(search);
        }
        else
        {
            search = search->prev;
        }
    }
}

/*
 * Worker thread: main event loop
 */
static void *worker_libevent(void *arg)
{
    LIBEVENT_THREAD *me = arg;

    /* Any per-thread setup can happen here; thread_init() will block until
       * all threads have finished initializing.
       */

    /* set an indexable thread-specific memory item for the lock type.
       * this could be unnecessary if we pass the conn *c struct through
       * all item_lock calls...
       */
    me->item_lock_type = ITEM_LOCK_GRANULAR;
    pthread_setspecific(item_lock_type_key, &me->item_lock_type);

    register_thread_initialized();

    event_base_loop(me->base, 0);
    return NULL;
}

static void *cleaner_libevent(void *arg)
{
    LIBEVENT_THREAD *me = arg;

    event_base_loop(me->base, 0);
    return NULL;
}

/*
 * Processes an incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */
static void thread_libevent_process(int fd, short which, void *arg)
{
    LIBEVENT_THREAD *me = (LIBEVENT_THREAD *)arg;
    CQ_ITEM *item;
    char buf[1];

    if (read(fd, buf, 1) != 1)
        if (global_settings.verbose > 0)
            fprintf(stderr, "Can't read from libevent pipe\n");

    switch (buf[0])
    {
    case 'c':
        item = cq_pop(me->new_conn_queue);

        if (NULL != item)
        {
            conn *c = conn_new(item->sfd,
                               item->init_state,
                               item->event_flags,
                               item->read_buffer_size,
                               item->transport,
                               me->base);
            if (c == NULL)
            {
                if (IS_UDP(item->transport))
                {
                    fprintf(stderr, "Can't listen for events on UDP socket\n");
                    exit(1);
                }
                else
                {
                    if (global_settings.verbose > 0)
                    {
                        fprintf(stderr, "Can't listen for events on fd %d\n",
                                item->sfd);
                    }
                    close(item->sfd);
                }
            }
            else
            {
                c->thread = me;
            }
            cqi_free(item);
        }
        break;
        /* we were told to flip the lock type and report in */
    case 'l':
        me->item_lock_type = ITEM_LOCK_GRANULAR;
        register_thread_initialized();
        break;
    case 'g':
        me->item_lock_type = ITEM_LOCK_GLOBAL;
        register_thread_initialized();
        break;
    }
}


/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, either during initialization (for UDP) or because
 * of an incoming connection.
 */
void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags,
                       int read_buffer_size, enum network_transport transport)
{
    CQ_ITEM *item = cqi_new();
    char buf[1];
    if (item == NULL)
    {
        close(sfd);
        /* given that malloc failed this may also fail, but let's try */
        fprintf(stderr, "Failed to allocate memory for connection object\n");
        return ;
    }

    int tid = (last_thread + 1) % global_settings.num_threads;

    LIBEVENT_THREAD *thread = threads + tid;

    last_thread = tid;

    item->sfd = sfd;
    item->init_state = init_state;
    item->event_flags = event_flags;
    item->read_buffer_size = read_buffer_size;
    item->transport = transport;

    cq_push(thread->new_conn_queue, item);

    buf[0] = 'c';
    if (write(thread->notify_send_fd, buf, 1) != 1)
    {
        perror("Writing to thread notify pipe");
    }
}



/*
 * Initializes the thread subsystem, creating various worker threads.
 *
 * nthreads  Number of worker event handler threads to spawn
 * main_base Event base for main thread
 */
void thread_init(int nthreads, struct event_base *main_base)
{
    int         i;
    int         power;

    pthread_mutex_init(&cache_lock, NULL);
    pthread_mutex_init(&stats_lock, NULL);

    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);
    pthread_mutex_init(&file_lock, NULL);
    pthread_mutex_init(&cqi_freelist_lock, NULL);
    cqi_freelist = NULL;

    /* Want a wide lock table, but don't waste memory */
    if (nthreads < 3)
    {
        power = 10;
    }
    else if (nthreads < 4)
    {
        power = 11;
    }
    else if (nthreads < 5)
    {
        power = 12;
    }
    else
    {
        /* 8192 buckets, and central locks don't scale much past 5 threads */
        power = 13;
    }
    /* file_table_locks are used for assoc_insert*/
    file_lock_count = hashsize(hashpower);
    file_table_locks = calloc(file_lock_count, sizeof(pthread_mutex_t));
    if(!file_table_locks)
    {
        perror("Can't allocate file_table locks");
        exit(1);
    }
    for (i = 0; i < file_lock_count; i++)
    {
        pthread_mutex_init(&file_table_locks[i], NULL);
    }

    file_find_lock_count = hashsize(hashpower + 1);
    file_find_open_clean_locks = calloc(file_find_lock_count, sizeof(pthread_mutex_t));
    if(!file_find_open_clean_locks)
    {
        perror("Can't allocate file_find_open_clean_locks");
        exit(1);
    }
    for(i = 0; i < file_find_lock_count; i++)
    {
        pthread_mutex_init(&file_find_open_clean_locks[i], NULL);
    }

    /*slab locks, every slabclass per lock*/
    slabs_lock = calloc(MAX_NUMBER_OF_SLAB_CLASSES, sizeof(pthread_mutex_t));
    if(!slabs_lock)
    {
        perror("Can't allocate slab locks");
        exit(1);
    }
    for(i = 0; i < MAX_NUMBER_OF_SLAB_CLASSES; i++)
    {
        pthread_mutex_init(&slabs_lock[i], NULL);
    }

    /* item_heads_tails_locks for heads and tails of each slabs */
    item_heads_tails_locks_count = POWER_LARGEST + 1 + 1;
    item_heads_tails_locks = calloc(item_heads_tails_locks_count, sizeof(pthread_mutex_t));
    if(!item_heads_tails_locks)
    {
        perror("Can't allocate file_table locks");
        exit(1);
    }
    for(i = 0; i < item_heads_tails_locks_count; i++)
    {
        pthread_mutex_init(&item_heads_tails_locks[i], NULL);
    }

    item_lock_count = hashsize(power);
    item_locks = calloc(item_lock_count, sizeof(pthread_mutex_t));
    if (! item_locks)
    {
        perror("Can't allocate item locks");
        exit(1);
    }
    for (i = 0; i < item_lock_count; i++)
    {
        pthread_mutex_init(&item_locks[i], NULL);
    }


    pthread_key_create(&item_lock_type_key, NULL);
    pthread_mutex_init(&item_global_lock, NULL);

    threads = calloc(nthreads + 1, sizeof(LIBEVENT_THREAD));
    if (! threads)
    {
        perror("Can't allocate thread descriptors");
        exit(1);
    }

    dispatcher_thread.base = main_base;
    dispatcher_thread.thread_id = pthread_self();

    for (i = 0; i < nthreads; i++)
    {
        int fds[2];
        if (pipe(fds))
        {
            perror("Can't create notify pipe");
            exit(1);
        }

        threads[i].notify_receive_fd = fds[0];
        threads[i].notify_send_fd = fds[1];

        setup_thread(&threads[i]);
        /* Reserve three fds for the libevent base, and two for the pipe */
        stats.reserved_fds += 5;
    }

    /* Create threads after we've done all the libevent setup. */
    for (i = 0; i < nthreads; i++)
    {
        create_worker(worker_libevent, &threads[i]);
    }

    /*Create clean thread*/
    setup_cleaner_thread(&threads[nthreads]);
    create_worker(cleaner_libevent,&threads[nthreads]);
    /* Wait for all the threads to set themselves up before returning. */
    pthread_mutex_lock(&init_lock);
    wait_for_thread_registration(nthreads);
    pthread_mutex_unlock(&init_lock);
}

void item_remove(item *item)
{
    uint32_t hv;
    hv = hash(NULL, item->offset);

    /*item_lock(hv);*/
    do_item_remove(item);
    /*item_unlock(hv);*/
}

void STATS_LOCK()
{
    pthread_mutex_lock(&stats_lock);
}

void STATS_UNLOCK()
{
    pthread_mutex_unlock(&stats_lock);
}


file_cache_t *file_item_alloc_with_lock(unsigned long fd)
{
    file_cache_t *it = NULL;
    pthread_mutex_lock(&file_lock);
    it = file_item_alloc(fd);
    pthread_mutex_unlock(&file_lock);

    return it;
}

int file_item_delete_with_lock(file_cache_t *file_head)
{
    int re;

    pthread_mutex_lock(&file_lock);
    re = file_item_delete(file_head);
    pthread_mutex_unlock(&file_lock);
    return re;
}

int assoc_insert_with_lock(file_cache_t *file, uint32_t hv)
{
    int re;
    pthread_mutex_lock(&file_table_locks[hv]);
    re = assoc_insert(file, hv);
    pthread_mutex_unlock(&file_table_locks[hv]);

    return re;
}

int assoc_delete_with_lock(const unsigned long fd, uint32_t hv)
{
    int re;

    pthread_mutex_lock(&file_table_locks[hv]);
    re = assoc_delete(fd, hv);
    pthread_mutex_unlock(&file_table_locks[hv]);
    return re;
}
