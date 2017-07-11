/*
 * Copyright (C) 2004-2010
 * 	Computer Department,
 * 	National University of Defense Technology (NUDT)
 *
 * 	Xie Min (xiemin@nudt.edu.cn)
 *
 * $Id$
 */

/* GLEX user interface */

#ifndef	__GLEX_H__
#define	__GLEX_H__

#include <stdint.h>
//#include <pthread.h>
#include <asm/byteorder.h>


/* return value of all glex functions */
typedef enum {
	GLEX_SUCCESS = 0,
	GLEX_SYS_ERR,
	GLEX_INVALID_PARAM,
	GLEX_NO_EP_RESOURCE,
	GLEX_NO_MEM_RESOURCE,
	GLEX_BUSY,
	GLEX_NO_MP,
	GLEX_NO_EVENT,
	GLEX_TIMEDOUT,
	GLEX_INTR,
	GLEX_CQ_OVERFLOW,
	GLEX_NOT_IMPLEMENTED
} glex_ret_t;


struct glex_info {
	uint32_t	num_devs;	/* number of devices installed */
	uint32_t	ver;		/* glex version */
};

struct glex_device_attr {
	uint32_t	nic_id;
	uint32_t	max_ep_num;		/* max number of endpoints which can be mmapped to user space */
	uint32_t	att_units;		/* how many units in ATT, each unit map one physical page */
	uint32_t	max_mp_data_len;	/* there are macro definitions too */
	uint32_t	max_rdma_data_len;
	uint32_t	max_imm_rdma_data_len;
	uint32_t	default_ep_dq_capacity;
	uint32_t	default_ep_cq_capacity;
	uint32_t	default_ep_mpq_capacity;
	uint32_t	max_ep_dq_capacity;
	uint32_t	max_ep_cq_capacity;
	uint32_t	max_ep_mpq_capacity;
	uint32_t	device_id;
};

/* synchronize with eni_if.h (ENI_ID_V1/ENI_ID_V2) */
#define	GLEX_DEVICE_ID_V1	0x6c7f
#define	GLEX_DEVICE_ID_V2	0x7c7f

#define	GLEX_EP_CAP_DQ_CAPACITY_DEFAULT		0
#define	GLEX_EP_CAP_CQ_CAPACITY_DEFAULT		0
#define	GLEX_EP_CAP_MPQ_CAPACITY_DEFAULT	0

struct glex_ep_cap {
	uint32_t	dq_capacity;
	uint32_t	cq_capacity;
	uint32_t	mpq_capacity;
};

struct glex_ep_attr {	/* XXX, TODO, a flag to indicate if it is the collective endpoint */
	uint32_t		key;
	struct glex_ep_cap	cap;
};

#define	GLEX_ANY_EP_NUM		0xffffffff

struct glex_ni_dev;

typedef struct glex_ep {
	struct glex_ni_dev	*ni_dev;
	//pthread_mutex_t		mutex;	//XXX, TODO
} glex_ep_t;

/* XXX, use bit mask to extract every fields, so field can be of any bits long */
typedef union {
	struct {
		uint16_t ep_num;	/* maximum 64K endpoints */
		uint16_t nic_id;
		uint32_t resv;		/* reserved */
	} s;
	uint64_t v;
} glex_ep_addr_t;

/* XXX, TODO, other ways to manage registered memory segment */
typedef union {
	struct {
		uint16_t umem_tbl_idx;
		uint16_t att_base_off;
		uint32_t att_index;
	} s;
	uint64_t v;
} glex_mh_t;

/*
 * maximum data_len should be synchronized with those defined in kernel/eni_if.h:
 * ENI_MP_MAX_DATA_LEN, ENI_RDMA_MAX_IMM_DATA_LEN, ENI_RDMA_MAX_DATA_LEN
 */
#define	GLEX_MAX_MP_DATA_LEN			112
#define	GLEX_MAX_IMM_RDMA_DATA_LEN		128	/* ENI_V1: 64, ENI_V2(PDP): 128 */
#define	GLEX_MAX_IMM_RDMA_DATA_LEN_HOST_DQ	112
#define	GLEX_MAX_RDMA_DATA_LEN			0x1000000


/* XXX, TODO, add FLAG_BLOCKING? */
enum glex_flag {
	GLEX_FLAG_LOCAL_INTR		= 1 << 0,	/* XXX, TODO */
	GLEX_FLAG_REMOTE_INTR		= 1 << 1,
	GLEX_FLAG_FENCE			= 1 << 2,
	GLEX_FLAG_SIGNALED		= 1 << 3,
	GLEX_FLAG_ACK			= 1 << 4,
	GLEX_FLAG_LOCAL_EVT		= 1 << 5,
	GLEX_FLAG_REMOTE_EVT		= 1 << 6,
	GLEX_FLAG_EVT_LOCAL_WAIT	= 1 << 7,
	GLEX_FLAG_EVT_REMOTE_WAIT	= 1 << 8,
	GLEX_FLAG_COLL_SEQ_START	= 1 << 9,	/* start of collective request sequence */
	GLEX_FLAG_COLL_SEQ_TAIL		= 1 << 10,	/* tail of collective request sequence */
	GLEX_FLAG_COLL_CP		= 1 << 11,	/* collective control packet */
	GLEX_FLAG_COLL_CP_DATA		= 1 << 12,	/* data of CP will be put into MPQ */
	GLEX_FLAG_COLL_CP_SWAP		= 1 << 13	/* swap data in MP request */
};

/* definition of MP request */
struct glex_mp_req {
	void			*data;
	uint32_t		len;
	uint32_t		cp_counter;
	int			flag;
	struct glex_mp_req	*next;
};


/* immediate RDMA can only do RDMA WRITE */
struct glex_imm_rdma_req {
	void				*data;
	uint32_t			len;
	glex_mh_t			rmt_mh;
	uint32_t			rmt_offset;
	glex_mh_t			local_evt_mh;
	glex_mh_t			rmt_evt_mh;
	uint32_t			local_evt_offset;
	uint32_t			rmt_evt_offset;
	uint64_t			local_evt_cookie;
	uint64_t			rmt_evt_cookie;
	uint32_t			rmt_key;		/* key of remote endpoint */
	uint32_t			cp_counter;
	int				flag;
	struct glex_imm_rdma_req	*next;
};

/*
 * single RDMA_READ:  local_mh <==== rmt_mh;
 * single RDMA_WRITE: local_mh ====> rmt_mh;
 */
enum glex_rdma_type {
	GLEX_RDMA_TYPE_WRITE	= 0,
	GLEX_RDMA_TYPE_READ	= 1
};

struct glex_rdma_req {
	glex_mh_t		local_mh;
	uint32_t		local_offset;
	uint32_t		len;
	glex_mh_t		rmt_mh;
	uint32_t		rmt_offset;
	enum glex_rdma_type	type;		//XXX, TODO, change to flag?
	glex_mh_t		local_evt_mh;
	glex_mh_t		rmt_evt_mh;
	uint32_t		local_evt_offset;
	uint32_t		rmt_evt_offset;
	uint64_t		local_evt_cookie;
	uint64_t		rmt_evt_cookie;
	uint32_t		rmt_key;	/* key of remote endpoint */
	uint32_t		cp_counter;
	int			flag;
	struct glex_rdma_req	*next;
};


enum glex_cqe_opcode {
	GLEX_CQE_OP_MP,
	GLEX_CQE_OP_CP,
	GLEX_CQE_OP_IMM_RDMA,
	GLEX_CQE_OP_RDMA_WRITE,
	GLEX_CQE_OP_RDMA_READ
};

enum glex_cqe_status {
	GLEX_CQE_STATUS_SUCCESS,
	GLEX_CQE_STATUS_TIMEOUT,
	GLEX_CQE_STATUS_DEST_DISCARD,
	GLEX_CQE_STATUS_DQ_ERR,
	GLEX_CQE_STATUS_STOP_VI,
	GLEX_CQE_STATUS_MPQ_FULL,
	GLEX_CQE_STATUS_HOST_DEAD,
	GLEX_CQE_STATUS_RMT_VI_INVAL,
	GLEX_CQE_STATUS_PCIE_RD,
	GLEX_CQE_STATUS_SRC_ECC,
	GLEX_CQE_STATUS_DEST_ECC,
	GLEX_CQE_STATUS_SRC_KEY,
	GLEX_CQE_STATUS_DEST_KEY,
	GLEX_CQE_STATUS_SRC_READ_ACCESS,
	GLEX_CQE_STATUS_DEST_READ_ACCESS,
	GLEX_CQE_STATUS_SRC_WRITE_ACCESS,
	GLEX_CQE_STATUS_DEST_WRITE_ACCESS,
	GLEX_CQE_STATUS_OVERFLOW		/* CQ is overflowed */
};

enum glex_cqe_signal {
	GLEX_CQE_SIGNALED_BY_REQ,
	GLEX_CQE_SIGNALED_BY_SYS
};

struct glex_cqe {
	enum glex_cqe_status	status;
	enum glex_cqe_opcode	opcode;
	enum glex_cqe_signal	signaled;
	uint32_t		rmt_nic_id;
	uint64_t		err_data0;
	uint64_t		err_data1;
	uint64_t		err_data2;
	/* uint64_t		err_data3; */
	/* uint64_t		err_data4; */
	/* uint64_t		err_data5; */
	/* uint64_t		err_data6; */
};


struct glex_ni_dev {
	struct glex_device_attr	attr;

	/* device specific functions */
	glex_ret_t (*glex_send_mp)(glex_ep_t *, glex_ep_addr_t,
				   struct glex_mp_req *, struct glex_mp_req **);
	glex_ret_t (*glex_receive_mp)(glex_ep_t *, int32_t, glex_ep_addr_t *,
				      void *, uint32_t *);
	glex_ret_t (*glex_probe_mp)(glex_ep_t *, int32_t,
				    glex_ep_addr_t *, void **, uint32_t *);
	glex_ret_t (*glex_peek_mp)(glex_ep_t *, uint32_t *);
	glex_ret_t (*glex_imm_rdma)(glex_ep_t *, glex_ep_addr_t,
				    struct glex_imm_rdma_req *, struct glex_imm_rdma_req **);
	glex_ret_t (*glex_rdma)(glex_ep_t *, glex_ep_addr_t,
				struct glex_rdma_req *, struct glex_rdma_req **);
	glex_ret_t (*glex_poll_cq)(glex_ep_t *, uint32_t *, struct glex_cqe[]);
};


/**
 * glex_init - Initialize device for use
 */
glex_ret_t glex_init(void);

/**
 * glex_finalize - Release device
 */
glex_ret_t glex_finalize(void);

/**
 * glex_get_info - Get information about glex
 */
glex_ret_t glex_get_info(struct glex_info *info);

/**
 * glex_query_device - Query device related attribute
 */
glex_ret_t glex_query_device(uint32_t dev_num, struct glex_device_attr *device_attr);

/**
 * glex_query_ep - Query endpoint related attribute
 */
glex_ret_t glex_query_ep(glex_ep_t *ep, struct glex_ep_attr *ep_attr);

/**
 * glex_create_ep - Create an endpoint with specified attribute
 */
glex_ret_t glex_create_ep(uint32_t dev_num, uint32_t ep_num,
			  struct glex_ep_attr *ep_attr, glex_ep_t **ep);

/**
 * glex_destroy_ep - Destroy an endpoint
 */
glex_ret_t glex_destroy_ep(glex_ep_t *ep);

/**
 * glex_get_ep_addr - Get endpoint address
 */
glex_ret_t glex_get_ep_addr(glex_ep_t *ep, glex_ep_addr_t *ep_addr);

/**
 * glex_register_mem - Register a memory region
 */
glex_ret_t glex_register_mem(glex_ep_t *ep,
			     void *addr, uint32_t len,
			     glex_mh_t *mh);

/**
 * ibv_deregister_mem - Deregister a memory region
 */
glex_ret_t glex_deregister_mem(glex_ep_t *ep, glex_mh_t mh);

/**
 * glex_send_mp - Post a list of MP send requests to an endpoint
 */
static inline glex_ret_t glex_send_mp(glex_ep_t *ep, glex_ep_addr_t rmt_ep_addr,
				      struct glex_mp_req *mp_req,
				      struct glex_mp_req **bad_mp_req)
{
	return ep->ni_dev->glex_send_mp(ep, rmt_ep_addr, mp_req, bad_mp_req);
}

/**
 * glex_receive_mp - Receive a MP from endpoint
 */
static inline glex_ret_t glex_receive_mp(glex_ep_t *ep, int32_t timeout,
					 glex_ep_addr_t *src_ep_addr,
					 void *data, uint32_t *len)
{
	return ep->ni_dev->glex_receive_mp(ep, timeout, src_ep_addr, data, len);
}

/**
 * glex_probe_mp - Probe a MP in MPQ, don't remove MP from MPQ
 */
static inline glex_ret_t glex_probe_mp(glex_ep_t *ep, int32_t timeout,
				       glex_ep_addr_t *src_ep_addr,
				       void **p_data, uint32_t *len)
{
	return ep->ni_dev->glex_probe_mp(ep, timeout, src_ep_addr, p_data, len);
}

/**
 * glex_peek_mp - Check if there are new MPs in endpoint
 */
static inline glex_ret_t glex_peek_mp(glex_ep_t *ep, uint32_t *mp_num)
{
	return ep->ni_dev->glex_peek_mp(ep, mp_num);
}

/**
 * glex_imm_rdma - Post a list of immediate RDMA WRITE requests to an endpoint
 */
static inline glex_ret_t glex_imm_rdma(glex_ep_t *ep, glex_ep_addr_t rmt_ep_addr,
					struct glex_imm_rdma_req *rdma_req,
					struct glex_imm_rdma_req **bad_rdma_req)
{
	return ep->ni_dev->glex_imm_rdma(ep, rmt_ep_addr, rdma_req, bad_rdma_req);
}

/**
 * glex_rdma - Post a list of RDMA requests to an endpoint
 */
static inline glex_ret_t glex_rdma(glex_ep_t *ep, glex_ep_addr_t rmt_ep_addr,
				   struct glex_rdma_req *rdma_req,
				   struct glex_rdma_req **bad_rdma_req)
{
	return ep->ni_dev->glex_rdma(ep, rmt_ep_addr, rdma_req, bad_rdma_req);
}

/**
 * glex_check_event - Check if event is triggered
 */
glex_ret_t glex_check_event(glex_ep_t *ep, uint64_t *evt,
			    glex_mh_t local_evt_mh, uint32_t local_evt_offset,
			    uint64_t cookie, int32_t timeout);

/**
 * glex_compose_ep_addr - Construct an endpoint address
 */
glex_ret_t glex_compose_ep_addr(uint32_t nic_id, uint32_t ep_num,
				glex_ep_addr_t *ep_addr);

/**
 * glex_decompose_ep_addr - Extract information from endpoint address
 */
glex_ret_t glex_decompose_ep_addr(glex_ep_addr_t ep_addr,
				  uint32_t *nic_id, uint32_t *ep_num);

/**
 * glex_poll_cq - Poll CQ entries
 */
static inline glex_ret_t glex_poll_cq(glex_ep_t *ep,
				      uint32_t *num_cqe, struct glex_cqe *cqe_list)
{
	return ep->ni_dev->glex_poll_cq(ep, num_cqe, cqe_list);
}


/**
 * glex_error_str - Show information about the error code
 */
char * glex_error_str(glex_ret_t ret);


/**
 * glex_cqe_status_str - show information about the CQE status
 */
char * glex_cqe_status_str(const int cqe_status);


/**
 * glex_cqe_opcode_str - show information about the CQE opcode
 */
char * glex_cqe_opcode_str(const int cqe_opcode);


#endif	/* __GLEX_H__ */
