
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>

#include "common.h"


//#define DEBUG_INFO 


//打印函数
void printMsg(const char*fmt,...){
    char *str;

    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vasprintf(&str, fmt, ap);
    va_end(ap);

    if (r < 0)
	return;

    printf("rdma_test: %s\n", str);
    free(str);
}


enum ibv_mtu pp_mtu_to_enum(int mtu)
{
	switch (mtu) {
	case 256:  return IBV_MTU_256;
	case 512:  return IBV_MTU_512;
	case 1024: return IBV_MTU_1024;
	case 2048: return IBV_MTU_2048;
	case 4096: return IBV_MTU_4096;
	default:   return -1;
	}
}

//初始化ctx
struct pingpong_context *init_ctx(struct ibv_device *ib_dev, int size,
					    int rx_depth, int port,
					    int use_event,int mem_access_flags,int qp_access_flags){

	struct pingpong_context *ctx;
    //int access_flags = IBV_ACCESS_LOCAL_WRITE;

    //创建测试结构体
    ctx = calloc(1,sizeof(*ctx));
    if(!ctx)
        return NULL;
    
    ctx->size       = size;  //缓冲区大小
	ctx->send_flags = IBV_SEND_SIGNALED;  
	ctx->rx_depth   = rx_depth;  //接收队列长度

    int page_size = sysconf(_SC_PAGESIZE);
    //ctx->buf = memalign(page_size, size);
	ctx->buf = calloc(1, size);

    if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
        goto clean_ctx;
	}

    //获取设备上下文
    ctx->context = ibv_open_device(ib_dev);
    	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
        goto clean_buffer;
	}

    //创建事件完成通知通道
	if(use_event){
		ctx->channel =  ibv_create_comp_channel(ctx->context);
		if(!ctx->channel){
			fprintf(stderr, "Couldn't create completion channel\n");
			goto clean_device;
		}
	}else
	    ctx->channel = NULL; 
	  

    //创建保护域pd
    ctx->pd = ibv_alloc_pd(ctx->context);
    	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
        goto clean_comp_channel;
	}

    //注册内存:access_flags设置对内存的权限
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, ctx->size, mem_access_flags);
    	if (!ctx->mr) {
		//fprintf(stderr, "Couldn't register MR\n");
		perror("Couldn't register MR:");
		goto clean_pd;
	}

    //创建CQ队列
    ctx->cq = ibv_create_cq(ctx->context,rx_depth + 1,NULL,ctx->channel,0);
    if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto clean_mr;
	}
    //创建QP
    struct ibv_qp_init_attr init_attr = {
			.send_cq = ctx->cq,
			.recv_cq = ctx->cq,
			.cap     = {
				.max_send_wr  = 1, //每次最多1个send请求
				.max_recv_wr  = rx_depth,  //每次最多rx_depth个recv请求
				.max_send_sge = 1,  //每个请求里最多1个数据段sge
				.max_recv_sge = 1
			},
			.qp_type = IBV_QPT_RC
		};

    ctx->qp = ibv_create_qp(ctx->pd,&init_attr);
   	if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			goto clean_cq;
	}

    struct ibv_qp_attr attr;
    ibv_query_qp(ctx->qp,&attr,IBV_QP_CAP,&init_attr);
    	if (init_attr.cap.max_inline_data >= size) {
			ctx->send_flags |= IBV_SEND_INLINE;  //允许将SQ和buf缓冲区内联，减少RDMA次数
		}


    //修改QP状态
    struct ibv_qp_attr qp_attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = port,
        .qp_access_flags = qp_access_flags
    };


    if(ibv_modify_qp(ctx->qp, &qp_attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)){
        fprintf(stderr, "Failed to modify QP to INIT\n");
		goto clean_qp;
    }
     
    return ctx;


clean_qp:
	ibv_destroy_qp(ctx->qp);

clean_cq:
	ibv_destroy_cq(ctx->cq);

clean_mr:
	ibv_dereg_mr(ctx->mr);

clean_pd:
	ibv_dealloc_pd(ctx->pd);

clean_comp_channel:
	if (ctx->channel)
		ibv_destroy_comp_channel(ctx->channel);

clean_device:
	ibv_close_device(ctx->context);

clean_buffer:
	free(ctx->buf);

clean_ctx:
	free(ctx);

	return NULL;

}



//下发recv请求
int post_recv(struct pingpong_context *ctx,int n){
    struct ibv_sge sge_list = {
        .addr = (uintptr_t)ctx->buf,
        .length = ctx->size,
        .lkey = ctx->mr->lkey
    };

    struct ibv_recv_wr recv_wr = {
		.wr_id = RECV_WRID,
	    .next = NULL,
	    .sg_list = &sge_list,
        .num_sge = 1
    };

    struct ibv_recv_wr *bad_wr;
    int i=0;
    for(i = 0;i<n;i++){
        if(ibv_post_recv(ctx->qp,&recv_wr,&bad_wr)){
            break;
        }
    }

    return i; //成功下发recv请求的数量
}


//下发send请求
int post_send(struct pingpong_context *ctx,int imm_flag,uint32_t imm_data){
    struct ibv_sge sge_list = {
            .addr = (uintptr_t)ctx->buf,
            //.length = ctx->size,
			.length = strlen((char *)ctx->buf)+1,
            .lkey = ctx->mr->lkey
        };

        struct ibv_send_wr send_wr = {
            .wr_id = SEND_WRID,
            .next = NULL,
            .sg_list = &sge_list,
            .num_sge = 1,
            .opcode =IBV_WR_SEND,
            .send_flags = ctx->send_flags
        };

		if(imm_flag){
			send_wr.opcode = IBV_WR_SEND_WITH_IMM;
			send_wr.imm_data = htonl(imm_data);
		}

        struct ibv_send_wr *bad_wr;

        return ibv_post_send(ctx->qp,&send_wr,&bad_wr);
}

//下发write请求
int post_write(struct pingpong_context *ctx,struct pingpong_dest *rem_dest,int imm_flag,uint32_t imm_data){
	struct ibv_sge sge_list = {
            .addr = (uintptr_t)ctx->buf,
            //.length = ctx->size,
			.length = strlen((char *)ctx->buf)+1,
            .lkey = ctx->mr->lkey
        };

        struct ibv_send_wr send_wr = {
            .wr_id = WRITE_WRID,
            .next = NULL,
            .sg_list = &sge_list,
            .num_sge = 1,
            .opcode =IBV_WR_RDMA_WRITE, 
            .send_flags = ctx->send_flags,
			.wr.rdma.remote_addr = rem_dest->buf_addr,
			.wr.rdma.rkey = rem_dest->buf_rkey
        };

		if(imm_flag){
			send_wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
			send_wr.imm_data = htonl(imm_data);
		}
        struct ibv_send_wr *bad_wr;
        return ibv_post_send(ctx->qp,&send_wr,&bad_wr);
}

//下发read请求
int post_read(struct pingpong_context *ctx,struct pingpong_dest *rem_dest){
	struct ibv_sge sge_list = {
		.addr = (uintptr_t)ctx->buf,
		.length = ctx->size,
		.lkey = ctx->mr->lkey
	};

	struct ibv_send_wr read_wr = {
		.wr_id = READ_WRID,
		.next = NULL,
		.sg_list = &sge_list,
		.num_sge = 1,
		.opcode = IBV_WR_RDMA_READ,
		.send_flags = ctx->send_flags,
		.wr.rdma.remote_addr = rem_dest->buf_addr,
		.wr.rdma.rkey = rem_dest->buf_rkey
	};

	struct ibv_send_wr *bad_wr;
	return ibv_post_send(ctx->qp,&read_wr,&bad_wr);
}

//下发atomic请求
int post_atomic(struct pingpong_context *ctx,struct pingpong_dest *rem_dest,int atomic_type,...){
	    //ATOMIC-ACK返回数据的存放地
		struct ibv_sge sge_list = {
            .addr = (uintptr_t)ctx->buf,
            .length = ctx->size,
            .lkey = ctx->mr->lkey
        };

		if(atomic_type != ATOMIC_COMSWAP && atomic_type != ATOMIC_FETCHADD){
			fprintf(stderr,"Unknown atomic type error\n");
			return 1;
		}
        struct ibv_send_wr send_wr = {
            .wr_id = atomic_type + 1,
            .next = NULL,
            .sg_list = &sge_list,
            .num_sge = 1,
			.opcode = (atomic_type==ATOMIC_COMSWAP?IBV_WR_ATOMIC_CMP_AND_SWP:IBV_WR_ATOMIC_FETCH_AND_ADD),
            .send_flags = ctx->send_flags,
			.wr.atomic.remote_addr = rem_dest->buf_addr,
			.wr.atomic.rkey = rem_dest->buf_rkey,
        };

		va_list args;
		va_start(args,atomic_type);

		send_wr.wr.atomic.compare_add = va_arg(args,uint64_t);
		if(atomic_type == ATOMIC_COMSWAP){
			send_wr.wr.atomic.swap = va_arg(args,uint64_t);
		}

		va_end(args);
        struct ibv_send_wr *bad_wr;
        return ibv_post_send(ctx->qp,&send_wr,&bad_wr);
}

//修改QP状态为RTR
int modify_qp_state_RTR(struct pingpong_context *ctx, int port,
			  enum ibv_mtu mtu, int sl,struct pingpong_dest *dest, int sgid_idx){
	struct ibv_qp_attr attr = {
		.qp_state		= IBV_QPS_RTR, //RTR
		.path_mtu		= mtu,
		.dest_qp_num		= dest->qpn,
		.rq_psn			= dest->psn,
		.max_dest_rd_atomic	= 1,
		.min_rnr_timer		= 12,
		.ah_attr		= {
			.is_global	= 0,
			.dlid		= dest->lid,
			.sl		= sl,
			.src_path_bits	= 0,
			.port_num	= port
		}
	};

	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;  //启用GRH
		attr.ah_attr.grh.hop_limit = 0xff;   //跳数限制
		attr.ah_attr.grh.dgid = dest->gid; //目标gid
		attr.ah_attr.grh.sgid_index = sgid_idx;  //本地GID索引
		attr.ah_attr.grh.traffic_class = 0;
	}
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		//fprintf(stderr, "Failed to modify QP to RTR\n");
		perror("Failed to modify QP to RTR");
		return 1;
	}
    return 0;
}

//修改QP状态为RTS
int modify_qp_state_RTS(struct pingpong_context *ctx,int my_psn,uint8_t	timeout,
                        uint8_t retry_cnt,uint8_t rnr_retry){
    struct ibv_qp_attr attr;
	attr.qp_state	    = IBV_QPS_RTS;  //RTS
	attr.timeout	    = timeout;
	attr.retry_cnt	    = retry_cnt;
	attr.rnr_retry	    = rnr_retry;
	attr.sq_psn	    = my_psn;  //本方发送的起始PSN
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		//fprintf(stderr, "Failed to modify QP to RTS\n");
		perror("Failed to modify QP to RTS");
		return 1;
	}
    return 0;
}


//修改QP状态
int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct pingpong_dest *dest, int sgid_idx){
  
	return !((!modify_qp_state_RTR(ctx,port,mtu,sl,dest,sgid_idx)) && (!modify_qp_state_RTS(ctx,my_psn,14,7,7)));
}

//资源销毁
int close_ctx(struct pingpong_context *ctx){
	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->cq)) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
			return 1;
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx->buf);
	free(ctx);

	return 0;
}

//客户端连接
struct pingpong_dest *client_exch_dest(const char *servername, int port,
						 const struct pingpong_dest *my_dest){
    struct addrinfo *res,*t;
	struct addrinfo hints={
		.ai_flags = AI_NUMERICSERV,
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP
	};
	char *service;
	char send_msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];  //传递的信息
    char recv_msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];  //接收的信息

	int n;
	int sockfd = -1;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	//创建套接字
	if(asprintf(&service,"%d",port) < 0){
	  return NULL;
	}

	//根据服务地址和端口号生成地址信息
	n = getaddrinfo(servername,service,&hints,&res);

		if (n < 0) {
		fprintf(stderr, "Can't not get addr for %s:%d\n", servername, port);
		free(service);
		return NULL;
	}

	for(t = res; t ;t = t->ai_next){
		sockfd = socket(t->ai_family,t->ai_socktype,t->ai_protocol);
		if(sockfd >=0 ){
			//连接
			if(!connect(sockfd,t->ai_addr,t->ai_addrlen)){
				printMsg("Client connect to %s:%d successfully!\n",servername,port);
				break;
			}
			close(sockfd);
			sockfd = -1;
		}
	}

	
	freeaddrinfo(res);
	free(service);

	if(sockfd < 0){
		fprintf(stderr, "Client Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}

	//向服务端传递信息
	gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(send_msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	
	if(write(sockfd,send_msg,sizeof send_msg) != sizeof send_msg){
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

#ifdef DEBUG_INFO
	printMsg("Client send info to server!\n");
#endif

	//从服务端接收对端信息
	if(read(sockfd,recv_msg,sizeof recv_msg) != sizeof recv_msg){
		perror("client read"); 
		fprintf(stderr, "Couldn't read remote address\n");
		goto out;
	}

#ifdef DEBUG_INFO
	printMsg("Client recv info from server!\n");
#endif

	write(sockfd,"done",sizeof "done");

#ifdef DEBUG_INFO
	printMsg("Client finish info exchange!\n");
#endif

	rem_dest = calloc(1,sizeof *rem_dest);
	if(!rem_dest){
		goto out;
	}

	//将获得的信息存储到rem_dest中
	sscanf(recv_msg,"%x:%x:%x:%s",&rem_dest->lid, &rem_dest->qpn,
						&rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

out:
	close(sockfd);
	return rem_dest;
}


//单边操作下的客户端信息交换
struct pingpong_dest *client_exch_dest_unidirect(const char *servername, int port,
						 const struct pingpong_dest *my_dest){
 struct addrinfo *res,*t;
	struct addrinfo hints={
		.ai_flags = AI_NUMERICSERV,
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP
	};
	char *service;
	//char send_msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];  //传递的信息
    //char recv_msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];  //接收的信息
	char msg[1024]={0};
	int length = 0;
	int n;
	int sockfd = -1;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	//创建套接字
	if(asprintf(&service,"%d",port) < 0){
	  return NULL;
	}

	//根据服务地址和端口号生成地址信息
	n = getaddrinfo(servername,service,&hints,&res);

		if (n < 0) {
		fprintf(stderr, "Can't not get addr for %s:%d\n", servername, port);
		free(service);
		return NULL;
	}

	for(t = res; t ;t = t->ai_next){
		sockfd = socket(t->ai_family,t->ai_socktype,t->ai_protocol);
		if(sockfd >=0 ){
			//连接
			if(!connect(sockfd,t->ai_addr,t->ai_addrlen)){
				printMsg("Client connect to %s:%d successfully!\n",servername,port);
				break;
			}
			close(sockfd);
			sockfd = -1;
		}
	}

	
	freeaddrinfo(res);
	free(service);

	if(sockfd < 0){
		fprintf(stderr, "Client Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}

	//向服务端传递信息
	struct pingpong_dest send_msg = {
		.lid = (my_dest->lid),
		.qpn = (my_dest->qpn),
		.psn = (my_dest->psn),
		.gid = my_dest->gid,
		.buf_addr = (my_dest->buf_addr),
		.buf_rkey = (my_dest->buf_rkey)
	};
	if(write(sockfd,&send_msg,sizeof send_msg) != sizeof send_msg){
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

#ifdef DEBUG_INFO
	printMsg("Client send info to server!\n");
#endif

	//从服务端接收对端信息
	rem_dest = calloc(1,sizeof *rem_dest);
	if(!rem_dest){
		goto out;
	}
	if(read(sockfd,rem_dest,sizeof *rem_dest) != sizeof *rem_dest){
		perror("client read"); 
		fprintf(stderr, "Couldn't read remote address\n");
		goto out;
	}


#ifdef DEBUG_INFO
	printMsg("Client recv info from server!\n");
#endif

	write(sockfd,"done",sizeof "done");

#ifdef DEBUG_INFO
	printMsg("Client finish info exchange!\n");
#endif


out:
	close(sockfd);
	return rem_dest;
}

//服务端连接
struct pingpong_dest *server_exch_dest(struct pingpong_context *ctx,
						 int ib_port, enum ibv_mtu mtu,
						 int port, int sl,
						 const struct pingpong_dest *my_dest,
						 int sgid_idx)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE | AI_NUMERICSERV,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char send_msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	char recv_msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1, connfd;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	if (asprintf(&service, "%d", port) < 0)  //asprintf比sprintf多了动态分配内存
		return NULL;

	n = getaddrinfo(NULL, service, &hints, &res);  //直接根据本机端口号生成地址信息

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			//n = 1;
			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Server Couldn't listen to port %d\n", port);
		return NULL;
	}

	listen(sockfd, 1);  //监听

#ifdef DEBUG_INFO
	printMsg("Server wait for socket connection!\n");
#endif

	//connfd = accept(sockfd, NULL, 0);
	struct sockaddr_in clientaddr={0};
	int nsize = sizeof(clientaddr);
    connfd = accept(sockfd, (struct sockaddr *)&clientaddr,&nsize);
	if (connfd < 0) {
		//fprintf(stderr, "accept() failed\n");//accept失败
		perror("accept() failed");
		return NULL;
	}

#ifdef DEBUG_INFO
	char cli_addr[20]={0};
	inet_ntop(AF_INET,&clientaddr.sin_addr,cli_addr,sizeof(cli_addr));
	int cli_port = ntohs(clientaddr.sin_port);
	printMsg("Server accept a socket connection from %s:%d\n",cli_addr,cli_port);
#endif
	close(sockfd);//关闭监听套接字后不能再接收新的请求了-------

	//先从客户端读取信息
	n = read(connfd, recv_msg, sizeof recv_msg);
	if (n != sizeof recv_msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof recv_msg);
		goto out;
	}
#ifdef DEBUG_INFO
	printMsg("Server recv info from client\n");
#endif

	rem_dest = calloc(1,sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(recv_msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
							&rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

	//服务端QP状态修改
	if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest,
								sgid_idx)) { 
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}
#ifdef DEBUG_INFO
	printMsg("Server modify QP successfully!\n");
#endif

	//服务端发送自身信息
	gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(send_msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	if (write(connfd, send_msg, sizeof send_msg) != sizeof send_msg) {
		fprintf(stderr, "Couldn't send local address\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

#ifdef DEBUG_INFO
	printMsg("Server send info to client\n");
#endif

	//接收客户端的"done"信息
	char *done = calloc(1,5);
	read(connfd, done, 5);
#ifdef DEBUG_INFO
	printMsg("Server finish info exchange!\n");
#endif
out:
	close(connfd);
	return rem_dest;
}

//单边操作下的服务端信息交换
struct pingpong_dest *server_exch_dest_unidirect(struct pingpong_context *ctx,
						 int ib_port, enum ibv_mtu mtu,
						 int port, int sl,
						 const struct pingpong_dest *my_dest,
						 int sgid_idx){
struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE | AI_NUMERICSERV,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	//char send_msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	//char recv_msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    char msg[1024]={0};
	int length = 0;
	int n;
	int sockfd = -1, connfd;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	if (asprintf(&service, "%d", port) < 0)  //asprintf比sprintf多了动态分配内存
		return NULL;

	n = getaddrinfo(NULL, service, &hints, &res);  //直接根据本机端口号生成地址信息

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			//n = 1;
			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Server Couldn't listen to port %d\n", port);
		return NULL;
	}

	listen(sockfd, 1);  //监听

#ifdef DEBUG_INFO
	printMsg("Server wait for socket connection!\n");
#endif

	struct sockaddr_in clientaddr={0};
	int nsize = sizeof(clientaddr);
    connfd = accept(sockfd, (struct sockaddr *)&clientaddr,&nsize);
	if (connfd < 0) {
		//fprintf(stderr, "accept() failed\n");//accept失败
		perror("accept() failed");
		return NULL;
	}

#ifdef DEBUG_INFO
	char cli_addr[20]={0};
	inet_ntop(AF_INET,&clientaddr.sin_addr,cli_addr,sizeof(cli_addr));
	int cli_port = ntohs(clientaddr.sin_port);
	printMsg("Server accept a socket connection from %s:%d\n",cli_addr,cli_port);
#endif
	close(sockfd);//关闭监听套接字后不能再接收新的请求了

	//先从客户端读取信息
	rem_dest = calloc(1,sizeof *rem_dest);
	if (!rem_dest)
		goto out;
	if(read(connfd, rem_dest, sizeof *rem_dest) != sizeof *rem_dest) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof *rem_dest);
		goto out;
	}


#ifdef DEBUG_INFO
	printMsg("Server recv info from client\n");
#endif


	//服务端QP状态修改
	if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest,
								sgid_idx)) { 
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}
#ifdef DEBUG_INFO
	printMsg("Server modify QP successfully!\n");
#endif

	struct pingpong_dest send_msg = {
		.lid = (my_dest->lid),
		.qpn = (my_dest->qpn),
		.psn = (my_dest->psn),
		.gid = my_dest->gid,
		.buf_addr = (my_dest->buf_addr),
		.buf_rkey = (my_dest->buf_rkey)
	};	
	if (write(connfd, &send_msg, sizeof send_msg) != sizeof send_msg) {
		fprintf(stderr, "Couldn't send local address\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

#ifdef DEBUG_INFO
	printMsg("Server send info to client\n");
#endif

	//接收客户端的"done"信息
	char *done = calloc(1,5);
	read(connfd, done, 5);
#ifdef DEBUG_INFO
	printMsg("Server finish info exchange!\n");
#endif
out:
	close(connfd);
	return rem_dest;
}


//将十六进制转化为gid
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
	char tmp[9];
	uint32_t v32;
	int i;

	for (tmp[8] = 0, i = 0; i < 4; ++i) {
		memcpy(tmp, wgid + i * 8, 8);
		sscanf(tmp, "%x", &v32);
		*(uint32_t *)(&gid->raw[i * 4]) = ntohl(v32);
	}
}

//将gid转化为十六进制形式
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
	int i;

	for (i = 0; i < 4; ++i)
		sprintf(&wgid[i * 8], "%08x",
			htonl(*(uint32_t *)(gid->raw + i * 4)));
}


//处理WC
int handle_cqe(const struct pingpong_context *ctx,const struct ibv_wc* wc){
	 		int status = wc->status;
            uint64_t wr_id = wc->wr_id;
            int opcode = wc->opcode;

            if(status != IBV_WC_SUCCESS){
                fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
						ibv_wc_status_str(status),
						status, (int)wr_id);
                return -1;
            }

            //处理不同的事件类型
            switch((int)wr_id){
                case RECV_WRID:
				if(opcode == IBV_WC_RECV_RDMA_WITH_IMM || opcode == IBV_WC_RECV){//imm_data at reciver
                    //获取imma_data
                    uint32_t imm_data = ntohl(wc->imm_data);
                    printMsg("Server receive immdt_data:%d\n",imm_data);
					//打印数据
					printMsg("Server receive data :%s\n",(char *)ctx->buf);
                }
                break;
				//client
				case SEND_IMMDT_WRID:
				case SEND_WRID:
				printMsg("Client completes send request\n");
				break;
				
				case WRITE_IMMDT_WRID:
                case WRITE_WRID:
				printMsg("Client completes write request\n");
				break;

			    case READ_WRID:
				printMsg("Client receive data:%s \n",(char *)ctx->buf);
				printMsg("Client completes read request\n");
				break;

				case ATOMIC_COMSWAP_WRID:
				case ATOMIC_FETCHADD_WRID:
				//打印atomic数据
				printMsg("Server memory original data:%ld\n",*(uint64_t *)ctx->buf);
				printMsg("Client completes atomic request\n");
				break;

				default:
				 fprintf(stderr,"Unknown wr_id error\n");
				 return -1;
			}

			return 0;
}



