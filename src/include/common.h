#ifndef COMMON_H
#define COMMON_H
#include <infiniband/verbs.h>
#include <infiniband/arch.h>

#define DEBUF_INFO

struct  pingpong_context {
	struct ibv_context	*context;
	struct ibv_comp_channel *channel;
	struct ibv_pd		*pd;
	struct ibv_mr		*mr;
	struct ibv_cq		*cq;
	struct ibv_qp		*qp;
	void			*buf;
	int			 size;
	int			 send_flags;
	int			 rx_depth;
	int			 pending;
	struct ibv_port_attr     portinfo;
	uint64_t		 completion_timestamp_mask;
};


 struct pingpong_dest {
	uint32_t lid;
	uint32_t qpn;
	uint32_t psn;
	//单边操作的额外信息
	uint64_t buf_addr;
	uint32_t buf_rkey;
	union ibv_gid gid;
};


/*
User-specified wr_id
*/
enum WR_ID{
	RECV_WRID = 1,
    SEND_WRID = 2,
    SEND_IMMDT_WRID,
	WRITE_WRID,
    WRITE_IMMDT_WRID,
	READ_WRID,
	ATOMIC_COMSWAP_WRID,
    ATOMIC_FETCHADD_WRID
};


enum RDMA_OPS{
    SEND = 1,
    SEND_IMMDT,
	WRITE,
    WRITE_IMMDT,
	READ,
	ATOMIC_COMSWAP,
    ATOMIC_FETCHADD
};



void printMsg(const char*fmt,...);

enum ibv_mtu pp_mtu_to_enum(int mtu);

//初始化
struct pingpong_context *init_ctx(struct ibv_device *ib_dev, int size,
					    int rx_depth, int port,
					    int use_event,int mem_access_flags,int qp_access_flags);

//post-recv
int post_recv(struct pingpong_context *ctx,int n);
//post-send
int post_send(struct pingpong_context *ctx,int imm_flag,uint32_t imm_data);
//post-write
int post_write(struct pingpong_context *ctx,struct pingpong_dest *rem_dest,int imm_flag,uint32_t imm_data);
//post-read
int post_read(struct pingpong_context *ctx,struct pingpong_dest *rem_dest);
//post-atomic
int post_atomic(struct pingpong_context *ctx,struct pingpong_dest *rem_dest,int atomic_type,...);

int modify_qp_state_RTR(struct pingpong_context *ctx, int port,
			  enum ibv_mtu mtu, int sl,struct pingpong_dest *dest, int sgid_idx);

int modify_qp_state_RTS(struct pingpong_context *ctx,int my_psn,uint8_t	timeout,
                        uint8_t retry_cnt,uint8_t rnr_retry);

int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct pingpong_dest *dest, int sgid_idx);

int close_ctx(struct pingpong_context *ctx);


void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);


//客户端socket交换信息
struct pingpong_dest *client_exch_dest(const char *servername, int port,
						 const struct pingpong_dest *my_dest);

struct pingpong_dest *client_exch_dest_unidirect(const char *servername, int port,
						 const struct pingpong_dest *my_dest);

//服务端socket交换信息
struct pingpong_dest *server_exch_dest(struct pingpong_context *ctx,
						 int ib_port, enum ibv_mtu mtu,
						 int port, int sl,
						 const struct pingpong_dest *my_dest,
						 int sgid_idx);

struct pingpong_dest *server_exch_dest_unidirect(struct pingpong_context *ctx,
						 int ib_port, enum ibv_mtu mtu,
						 int port, int sl,
						 const struct pingpong_dest *my_dest,
						 int sgid_idx);


int handle_cqe(const struct pingpong_context *ctx,const struct ibv_wc* wc);
#endif