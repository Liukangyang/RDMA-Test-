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

static int page_size;
static int use_odp;
static int use_ts;

static void usage(const char *argv0)
{
    printf("==================================================================\n");
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
	printf("  -s, --size=<size>      size of message to exchange (default 4096)\n");
	printf("  -m, --mtu=<size>       path MTU (default 1024)\n");
	printf("  -r, --rx-depth=<dep>   number of receives to post at a time (default 500)\n");
	printf("  -n, --iters=<iters>    number of exchanges (default 1000)\n");
	printf("  -l, --sl=<sl>          service level value\n");
	printf("  -e, --events           sleep on CQ events (default poll)\n");
	printf("  -g, --gid-idx=<gid index> local port gid index\n");
	printf("  -o, --odp		    use on demand paging\n");
	printf("  -t, --ts	            get CQE with timestamp\n");
    printf("  -a, --address=<ip-addr>	 server ip address\n");
    printf("  -h, --help   	         display help information\n");
    printf("==================================================================\n");
}

int main(int argc, char *argv[]){

    struct ibv_device      **dev_list;
	struct ibv_device	*ib_dev;
	struct pingpong_context *ctx;
	struct pingpong_dest     my_dest;
	struct pingpong_dest    *rem_dest = NULL;
	struct timeval           start, end;
	char                    *ib_devname = NULL;
	char                    *servername = NULL;
	unsigned int             port = 18515;  //服务端端口号
	int                      ib_port = 1;  //物理端口号
	unsigned int             size = 4096;
	enum ibv_mtu		 mtu = IBV_MTU_1024;
	unsigned int             rx_depth = 10;
	unsigned int             iters = 1000;
	int                      use_event = 0;
	int                      routs;
	int                      rcnt, scnt;
	int                      num_cq_events = 0;
	int                      sl = 0;
	int			 gidx = 0;
	char			 gid[33];
	unsigned int		 comp_recv_max_time_delta = 0;
	unsigned int		 comp_recv_min_time_delta = 0xffffffff;
	uint64_t		 comp_recv_total_time_delta = 0;
	uint64_t		 comp_recv_prev_time = 0;
	int			 last_comp_with_ts = 0;
	unsigned int		 comp_with_time_iters = 0;

	int immdt_flag = 0;
	uint64_t immdt_data = 0;

	srand48(getpid() * time(NULL));

    //读取命令行参数
    static struct option long_options[] = {
			{ .name = "port",     .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",   .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",  .has_arg = 1, .val = 'i' },
			{ .name = "size",     .has_arg = 1, .val = 's' },
			{ .name = "mtu",      .has_arg = 1, .val = 'm' },
			{ .name = "rx-depth", .has_arg = 1, .val = 'r' },
			{ .name = "iters",    .has_arg = 1, .val = 'n' },
			{ .name = "sl",       .has_arg = 1, .val = 'l' },
			{ .name = "events",   .has_arg = 0, .val = 'e' },
			{ .name = "gid-idx",  .has_arg = 1, .val = 'g' },
			{ .name = "odp",      .has_arg = 0, .val = 'o' },
			{ .name = "ts",       .has_arg = 0, .val = 't' },
            { .name = "addr",     .has_arg = 1, .val = 'a'},   //server-ip-addr(用于socket建链)
			{ .name = "help",     .has_arg = 0, .val = 'h'},
			{ 0 }
		};

    int c;
    while(1){

        c = getopt_long(argc, argv, "p:d:i:s:m:r:n:l:eg:ota:h",
				long_options, NULL);

        if(c == -1)
            break;
        
        switch(c){
        case 'p':
			port = strtoul(optarg, NULL, 0);
			if (port > 65535) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'd':
			ib_devname = strdup(optarg);
			break;

		case 'i':
			ib_port = strtol(optarg, NULL, 0);
			if (ib_port < 1) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 's':
			size = strtoul(optarg, NULL, 0);
			break;

		case 'm':
			mtu = pp_mtu_to_enum(strtol(optarg, NULL, 0));
			if (mtu < 0) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'r':
			rx_depth = strtoul(optarg, NULL, 0);
			break;

		case 'n':
			iters = strtoul(optarg, NULL, 0);
			break;

		case 'l':
			sl = strtol(optarg, NULL, 0);
			break;

		case 'e':
			use_event = 1;
			break;

		case 'g':
			gidx = strtol(optarg, NULL, 0);
			break;

		case 'o':
			use_odp = 1;
			break;
		case 't':
			use_ts = 1;
            break;
        case 'a':
            servername = strdup(optarg); //hostname or ip-address
            break;
        case 'h':
		default:
			usage(argv[0]);
			return 1;
		}
    }

    page_size = sysconf(_SC_PAGESIZE);

    //1.获取设备信息
    int num_devices;
    dev_list = ibv_get_device_list(&num_devices);
    if (!ib_devname) {
		ib_dev = *dev_list;  //取第一个设备
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			return 1;
		}
	} else {
		int i;
		for (i = 0; dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
				break;
		ib_dev = dev_list[i];
		if (!ib_dev) {
			fprintf(stderr, "IB device %s not found\n", ib_devname);
			return 1;
		}
	}

    //2.初始化
    //printMsg("ctx init\n");
    int access_flags = IBV_ACCESS_LOCAL_WRITE | 
                        IBV_ACCESS_REMOTE_WRITE | 
                        IBV_ACCESS_REMOTE_READ || 
                        IB_UVERBS_ACCESS_REMOTE_ATOMIC;
    ctx = init_ctx(ib_dev, size,rx_depth, ib_port,use_event,access_flags,IBV_ACCESS_REMOTE_WRITE | 
                                                                          IBV_ACCESS_REMOTE_READ |   
                                                                          IB_UVERBS_ACCESS_REMOTE_ATOMIC);

    if(!ctx){
        fprintf(stderr,"Init ERROR!\n");
        return 1;
    }

	//预下发recv wr
	routs = post_recv(ctx,ctx->rx_depth);
    if (routs < ctx->rx_depth) {
		fprintf(stderr, "Couldn't post receive (%d)\n", routs);
		return 1;
	}

    //获取lid和gid信息
       if (ibv_query_port(ctx->context, ib_port, &ctx->portinfo)) {
		fprintf(stderr, "Couldn't get port info\n");
		return 1;
	}

	my_dest.lid = ctx->portinfo.lid;
	if (ctx->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET &&
							!my_dest.lid) {
		fprintf(stderr, "Couldn't get local LID\n");
		return 1;
	}

	if (gidx >= 0) {
		if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
			fprintf(stderr, "can't read sgid of index %d\n", gidx);
			return 1;
		}
	} else
		memset(&my_dest.gid, 0, sizeof my_dest.gid);


    //初始化qpn和psn
    my_dest.qpn = ctx->qp->qp_num;
    my_dest.psn = lrand48() & 0xffffff;
    //初始化buf_addr和rkey
    my_dest.buf_addr = (uint64_t)ctx->mr->addr;
    my_dest.buf_rkey = ctx->mr->rkey;

	//打印设备信息
    printf("  devices                      GUID\n");
    printf("======================================\n");
	printf("  %-16s\t%016llx\n",
		    ibv_get_device_name(ib_dev),
		    (unsigned long long) ntohll(ibv_get_device_guid(ib_dev)));
	inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);
	printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s,addr 0x%08x,rkey 0x%06x\n",
	       my_dest.lid, my_dest.qpn, my_dest.psn, gid , my_dest.buf_addr,my_dest.buf_rkey);

    //3.socket建链
    rem_dest = server_exch_dest_unidirect(ctx,ib_port,mtu,port,sl,&my_dest,gidx);

    if(!rem_dest){
        printMsg("Couldn't get peer info!\n");
        return 1;
    }

    //信息打印
#ifdef DEBUG_INFO
    inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printMsg(" remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s ,addr 0x%08x ,rkey 0x%06x\n",
	       rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid,rem_dest->buf_addr,rem_dest->buf_rkey);
#endif


    //请求通知
	if(use_event){
		if(ibv_req_notify_cq(ctx->cq,0)){
			fprintf(stderr, "Couldn't request CQ notification\n");
			return 1;
		}
	}    

    //获取完成事件通知(阻塞)
	if(use_event){
		struct ibv_cq *ev_cq;
		void          *ev_ctx;		
		if(ibv_get_cq_event(ctx->channel,&ev_cq,&ev_ctx)){
				fprintf(stderr, "Failed to get cq_event\n");
				return 1;
		}
 
		if(ev_cq != ctx->cq){
			fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
			return 1;
		}
		num_cq_events++;	

		//请求再次通知
		if(ibv_req_notify_cq(ctx->cq,0)){
			fprintf(stderr, "Couldn't request CQ notification\n");
			return 1;
		}
	}

    //7.poll cq
    int ne = 0;
	struct ibv_wc wc;
	struct timeval poll_starttime,poll_curtime;
	double time =.0;
	int count = 0;
	gettimeofday(&poll_starttime,NULL);
    do{
        ne = ibv_poll_cq(ctx->cq,1,&wc);
		if(ne < 0){
		    fprintf(stderr, "poll CQ failed %d\n", ne);
			return 1;	
		}
		else if(ne == 0){
			goto timeout;
		}
		//ne>0,handle cqe
        handle_cqe(ctx,&wc);   
timeout:
		//超时退出
		gettimeofday(&poll_curtime,NULL);
		time = ((poll_curtime.tv_sec * 1000000 + poll_curtime.tv_usec) - (poll_starttime.tv_sec * 1000000 + poll_starttime.tv_usec)) / 1000000;
    }while(time < 2);

    if(use_event){
		//确认cqe
		ibv_ack_cq_events(ctx->cq,num_cq_events);
	}
    //释放资源
    if(close_ctx(ctx)){
        printMsg("failed to release sources!\n");
        return 1;
    }
    ibv_free_device_list(dev_list);
    
    if(rem_dest)
       free(rem_dest);
    return 0;

}












