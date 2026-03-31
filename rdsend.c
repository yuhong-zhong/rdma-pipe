/*
 * cc -o rdsend rdsend.c -libverbs -luring
 *
 * usage:
 * rdsend [-v] <server> <port> <key> [filename]
 *
 * Reads data from stdin and RDMA sends it to the given server and port,
 * authenticating with the key.
 *
 * Uses a TCP socket for QP handshake to avoid ibacm dependency.
 *
 * Environment variables (sender decides, receiver follows):
 *   RDMA_BUF_SIZE   - chunk size in MB (default 32)
 *   RDMA_RING_SIZE  - receiver ring depth (default 4)
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/verbs.h>

enum {
  RESOLVE_TIMEOUT_MS = 5000,
};

struct qp_info {
  uint32_t qpn;
  uint32_t psn;
  union ibv_gid gid;
};

/* Sent by rdsend to rdrecv before RDMA setup so rdrecv can allocate correctly */
struct rdma_params {
  uint32_t buf_size;
  uint32_t ring_size;
  uint64_t file_size;  /* 0 = unknown (stdin source) */
};

void usage() {
  fprintf(stderr, "USAGE: rdsend [-v] <server> <port> <key> [filename]\n");
}

// Returns 1 if GID at (devname, port, gidx) is of type "RoCE v2"
static int gid_is_roce_v2(const char *devname, uint8_t port, int gidx) {
  char path[256];
  char typebuf[32] = {};
  snprintf(path, sizeof(path),
           "/sys/class/infiniband/%s/ports/%d/gid_attrs/types/%d",
           devname, port, gidx);
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  if (!fgets(typebuf, sizeof(typebuf), f)) { fclose(f); return 0; }
  fclose(f);
  // strip trailing newline
  typebuf[strcspn(typebuf, "\n")] = '\0';
  return strcmp(typebuf, "RoCE v2") == 0;
}

// Find the ibv_context and port for the device whose RoCEv2 GID matches the
// local IP used to route to target_ip.
struct ibv_context *find_rdma_device(const char *target_ip, uint8_t *port_out,
                                     int *gid_idx_out, union ibv_gid *gid_out) {
  // Find which local IP routes to target
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) return NULL;
  struct sockaddr_in remote = {
      .sin_family = AF_INET,
      .sin_port = htons(1),
  };
  inet_aton(target_ip, &remote.sin_addr);
  if (connect(sock, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
    close(sock);
    return NULL;
  }
  struct sockaddr_in local;
  socklen_t slen = sizeof(local);
  getsockname(sock, (struct sockaddr *)&local, &slen);
  close(sock);
  uint32_t local_ip = local.sin_addr.s_addr; // network byte order

  fprintf(stderr, "DEBUG: local_ip = %08x (looking for RoCEv2 GID)\n", local_ip);

  int num_devices;
  struct ibv_device **devlist = ibv_get_device_list(&num_devices);
  if (!devlist) return NULL;

  for (int i = 0; i < num_devices; i++) {
    fprintf(stderr, "DEBUG: checking device %s\n", ibv_get_device_name(devlist[i]));
    struct ibv_context *ctx = ibv_open_device(devlist[i]);
    if (!ctx) continue;

    struct ibv_device_attr attr;
    if (ibv_query_device(ctx, &attr)) {
      ibv_close_device(ctx);
      continue;
    }

    for (uint8_t port = 1; port <= attr.phys_port_cnt; port++) {
      struct ibv_port_attr pattr;
      if (ibv_query_port(ctx, port, &pattr)) continue;
      if (pattr.state != IBV_PORT_ACTIVE) continue;

      for (int gidx = 0; gidx < pattr.gid_tbl_len; gidx++) {
        union ibv_gid gid;
        if (ibv_query_gid(ctx, port, gidx, &gid)) continue;
        // RoCEv2 IPv4-mapped GID: bytes 0-9 are zero, 10-11 are 0xFF,
        // bytes 12-15 are the IPv4 address.
        int is_roce_v2 = gid_is_roce_v2(ibv_get_device_name(devlist[i]), port, gidx);
        if (gid.raw[10] == 0xFF && gid.raw[11] == 0xFF) {
          uint32_t gid_ip;
          memcpy(&gid_ip, &gid.raw[12], 4);
          fprintf(stderr, "DEBUG:  gid[%d]: ip=%08x local=%08x roce_v2=%d\n",
                  gidx, gid_ip, local_ip, is_roce_v2);
          if (gid_ip == local_ip && is_roce_v2) {
            *port_out = port;
            *gid_idx_out = gidx;
            *gid_out = gid;
            ibv_free_device_list(devlist);
            return ctx;
          }
        }
      }
    }
    ibv_close_device(ctx);
  }
  ibv_free_device_list(devlist);
  return NULL;
}

int max(int a, int b) { return a < b ? b : a; }

int main(int argc, char *argv[]) {
  struct ibv_context *ctx = NULL;
  struct ibv_pd *pd = NULL;
  struct ibv_comp_channel *comp_chan = NULL;
  struct ibv_cq *cq = NULL;
  struct ibv_cq *evt_cq = NULL;
  struct ibv_mr *mr = NULL;
  struct ibv_qp *qp = NULL;
  struct ibv_sge sge, rsge;
  struct ibv_send_wr send_wr = {};
  struct ibv_send_wr *bad_send_wr;
  struct ibv_recv_wr recv_wr = {};
  struct ibv_recv_wr *bad_recv_wr;
  struct ibv_wc wc;
  void *cq_context;

  uint32_t *buf, *buf2, *tmp;

  uint32_t event_count = 0;
  struct timespec now, tmstart;
  double seconds;

  uint64_t total_bytes, buf_read_bytes;
  int wr_id = 1, more_to_send = 1;
  uint32_t buf_len = 0;

  char *host, *ports;
  int port;
  char *key;
  uint32_t keylen;

  int verbose = 0;
  int argv_idx = 1;

  if (argc < 4) {
    usage();
    return 1;
  }

  if (strcmp(argv[argv_idx], "-v") == 0) {
    verbose++;
    argv_idx++;
  }

  host = argv[argv_idx++];
  ports = argv[argv_idx++];
  port = atoi(ports);

  if (port < 1 || port > 65535) {
    usage();
    fprintf(stderr,
            "\nError: Port should be between 1 and 65535, got %d instead.\n\n",
            port);
    return 1;
  }

  key = argv[argv_idx++];
  keylen = strlen(key);

  /* Sender decides buf_size and ring_size; these are sent to rdrecv before
   * RDMA setup so the receiver can allocate accordingly. */
  struct rdma_params params = {
      .buf_size  = 32 * 1024 * 1024,
      .ring_size = 4,
  };
  const char *bs_env = getenv("RDMA_BUF_SIZE");
  if (bs_env) params.buf_size = (uint32_t)atoi(bs_env) * 1024 * 1024;
  const char *rs_env = getenv("RDMA_RING_SIZE");
  if (rs_env) params.ring_size = (uint32_t)atoi(rs_env);
  if (params.ring_size < 2) params.ring_size = 2;

  uint32_t buf_size = params.buf_size;

  int fd = STDIN_FILENO;
  int fds[16];
  if (argv_idx < argc) {
    fd = open(argv[argv_idx], O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "Error opening file %s\n", argv[argv_idx]);
      return 200;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
      params.file_size = (uint64_t)st.st_size;
    for (int i = 0; i < 16; i++) {
      fds[i] = open(argv[argv_idx], O_RDONLY | O_DIRECT);
      posix_fadvise(fds[i], 0, 0, POSIX_FADV_NOREUSE);
      if (fds[i] < 0) {
        fprintf(stderr, "Error opening file %s\n", argv[argv_idx]);
        return 200;
      }
    }
  }

  /* Connect TCP and send params to rdrecv before allocating buffers.
   * This lets rdrecv allocate the right size ring immediately. */
  int tcp_sock = -1;
  int retries = 0;
  while (tcp_sock < 0) {
    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_aton(host, &addr.sin_addr);
    if (connect(tcp_sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) break;
    close(tcp_sock);
    tcp_sock = -1;
    retries++;
    if (retries > 300) {
      fprintf(stderr, "Connection timed out\n");
      return 199;
    }
    nanosleep((const struct timespec[]){{0, 10000000L}}, NULL);
  }

  if (write(tcp_sock, &params, sizeof(params)) != sizeof(params)) {
    fprintf(stderr, "Failed to send params\n");
    return 198;
  }

  /* 2 send buffers + 4 bytes for ACK recv, huge-page backed for fast ibv_reg_mr */
  buf_len = buf_size * 2 + 4;
  buf = mmap(NULL, buf_len, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (buf == MAP_FAILED) { perror("mmap failed"); return 113; }
  madvise(buf, buf_len, MADV_HUGEPAGE);
  memset(buf, 0, buf_len);  /* prefault before ibv_reg_mr */
  buf2 = (uint32_t *)(((char *)buf) + buf_size);

  // Find RDMA device for target
  uint8_t port_num;
  int gid_idx;
  union ibv_gid local_gid;
  ctx = find_rdma_device(host, &port_num, &gid_idx, &local_gid);
  if (!ctx) {
    fprintf(stderr, "No RDMA device found for target %s\n", host);
    return 101;
  }

  pd = ibv_alloc_pd(ctx);
  if (!pd) return 109;

  comp_chan = ibv_create_comp_channel(ctx);
  if (!comp_chan) return 110;

  cq = ibv_create_cq(ctx, 2, NULL, comp_chan, 0);
  if (!cq) return 111;

  if (ibv_req_notify_cq(cq, 0)) return 112;

  mr = ibv_reg_mr(pd, buf, buf_len, IBV_ACCESS_LOCAL_WRITE);
  if (!mr) return 99;

  struct ibv_qp_init_attr qp_init = {
      .send_cq = cq,
      .recv_cq = cq,
      .qp_type = IBV_QPT_RC,
      .cap = {.max_send_wr = 2,
              .max_send_sge = 1,
              .max_recv_wr = 1,
              .max_recv_sge = 1},
  };
  qp = ibv_create_qp(pd, &qp_init);
  if (!qp) return 114;

  // Move QP to INIT
  struct ibv_qp_attr init_attr = {
      .qp_state = IBV_QPS_INIT,
      .pkey_index = 0,
      .port_num = port_num,
      .qp_access_flags = 0,
  };
  if (ibv_modify_qp(qp, &init_attr,
                    IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                        IBV_QP_ACCESS_FLAGS))
    return 115;

  /* Exchange QP info over the same TCP connection */
  uint32_t local_psn = (uint32_t)(lrand48() & 0xFFFFFF);
  struct qp_info local_info = {
      .qpn = qp->qp_num,
      .psn = local_psn,
      .gid = local_gid,
  };
  struct qp_info remote_info;

  // rdrecv sends first, then waits for ours
  ssize_t n = read(tcp_sock, &remote_info, sizeof(remote_info));
  if (n != sizeof(remote_info)) {
    fprintf(stderr, "TCP handshake read failed\n");
    return 116;
  }
  n = write(tcp_sock, &local_info, sizeof(local_info));
  if (n != sizeof(local_info)) {
    fprintf(stderr, "TCP handshake write failed\n");
    return 117;
  }
  close(tcp_sock);

  // Move QP to RTR
  struct ibv_qp_attr rtr_attr = {
      .qp_state = IBV_QPS_RTR,
      .path_mtu = IBV_MTU_4096,
      .dest_qp_num = remote_info.qpn,
      .rq_psn = remote_info.psn,
      .max_dest_rd_atomic = 1,
      .min_rnr_timer = 12,
      .ah_attr =
          {
              .is_global = 1,
              .grh =
                  {
                      .dgid = remote_info.gid,
                      .sgid_index = gid_idx,
                      .hop_limit = 64,
                  },
              .sl = 0,
              .port_num = port_num,
          },
  };
  if (ibv_modify_qp(qp, &rtr_attr,
                    IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
    return 118;

  // Move QP to RTS
  struct ibv_qp_attr rts_attr = {
      .qp_state = IBV_QPS_RTS,
      .timeout = 14,
      .retry_cnt = 7,
      .rnr_retry = 7,
      .sq_psn = local_psn,
      .max_rd_atomic = 1,
  };
  if (ibv_modify_qp(qp, &rts_attr,
                    IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                        IBV_QP_MAX_QP_RD_ATOMIC))
    return 119;

  /* Prepost recv for ACK */
  rsge.addr = (uintptr_t)(((char *)buf) + 2 * buf_size);
  rsge.length = 4;
  rsge.lkey = mr->lkey;

  recv_wr.wr_id = 0;
  recv_wr.sg_list = &rsge;
  recv_wr.num_sge = 1;

  clock_gettime(CLOCK_REALTIME, &tmstart);
  total_bytes = 0;

  memcpy((void *)buf, key, keylen + 1);
  buf_read_bytes = keylen + 1;
  total_bytes = 0;
  more_to_send = 1;

  while (more_to_send) {
    if (buf_read_bytes == 0) {
      more_to_send = 0;
    }

    if (ibv_post_recv(qp, &recv_wr, &bad_recv_wr))
      return 1;

    sge.addr = (uintptr_t)buf;
    sge.length = buf_read_bytes;
    sge.lkey = mr->lkey;

    send_wr.wr_id = wr_id;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;

    if (ibv_post_send(qp, &send_wr, &bad_send_wr))
      return 1;

    tmp = buf;
    buf = buf2;
    buf2 = tmp;

    if (fd != STDIN_FILENO) {
      ssize_t r = pread(fds[0], buf, buf_size, total_bytes);
      buf_read_bytes = r > 0 ? (uint64_t)r : 0;
    } else {
      buf_read_bytes = max(0, read(fd, buf, buf_size));
    }
    total_bytes += buf_read_bytes;

    /* Wait for send completion */
    if (ibv_get_cq_event(comp_chan, &evt_cq, &cq_context))
      return 2;
    if (ibv_req_notify_cq(cq, 0))
      return 3;
    if (ibv_poll_cq(cq, 1, &wc) != 1)
      return 4;
    if (wc.status != IBV_WC_SUCCESS) {
      fprintf(stderr, "send WC error: %s\n", ibv_wc_status_str(wc.status));
      return 5;
    }

    /* Wait for recv completion (ACK from rdrecv) */
    if (ibv_get_cq_event(comp_chan, &evt_cq, &cq_context))
      return 6;
    if (ibv_req_notify_cq(cq, 0))
      return 7;
    if (ibv_poll_cq(cq, 1, &wc) != 1)
      return 8;
    if (wc.status != IBV_WC_SUCCESS)
      return 9;

    event_count += 2;
  }

  clock_gettime(CLOCK_REALTIME, &now);
  seconds = (double)((now.tv_sec + now.tv_nsec * 1e-9) -
                     (double)(tmstart.tv_sec + tmstart.tv_nsec * 1e-9));
  if (verbose > 0) {
    fprintf(stderr, "Bandwidth %.3f GB/s\n", (total_bytes / seconds) / 1e9);
  }

  ibv_ack_cq_events(cq, event_count);

  ibv_destroy_qp(qp);
  ibv_dereg_mr(mr);
  ibv_destroy_cq(cq);
  ibv_dealloc_pd(pd);
  ibv_destroy_comp_channel(comp_chan);
  ibv_close_device(ctx);

  return 0;
}
