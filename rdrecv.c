/*
 * build:
 * cc -o rdrecv rdrecv.c -libverbs
 *
 * usage:
 * rdrecv [-v] <port> <key> <filename>
 *
 * Waits for client to connect to given port and authenticate with the key.
 * Uses a TCP socket for QP handshake to avoid ibacm dependency.
 *
 * buf_size, ring_size, and file_size are sent by the sender (rdsend) at the
 * start of the TCP handshake.  rdrecv has no tuning env vars of its own.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

/* Received from rdsend before RDMA setup */
struct rdma_params {
  uint32_t buf_size;
  uint32_t ring_size;
  uint64_t file_size;  /* 0 = unknown (stdin source) */
};

static double now_sec(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}
#define TS(label) fprintf(stderr, "TS %.3f %s\n", now_sec() - _ts0, label)

void usage() {
  fprintf(stderr, "USAGE: rdrecv [-v] <port> <key> <filename>\n");
}

void wrongkey() { fprintf(stderr, "Wrong key received\n"); }

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
  typebuf[strcspn(typebuf, "\n")] = '\0';
  return strcmp(typebuf, "RoCE v2") == 0;
}

// Find the ibv_context and port for the device whose RoCEv2 GID matches local_ip.
struct ibv_context *find_rdma_device_by_ip(uint32_t local_ip,
                                            uint8_t *port_out,
                                            int *gid_idx_out,
                                            union ibv_gid *gid_out) {
  int num_devices;
  struct ibv_device **devlist = ibv_get_device_list(&num_devices);
  if (!devlist) return NULL;

  for (int i = 0; i < num_devices; i++) {
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
        if (gid.raw[10] == 0xFF && gid.raw[11] == 0xFF) {
          uint32_t gid_ip;
          memcpy(&gid_ip, &gid.raw[12], 4);
          if (gid_ip == local_ip &&
              gid_is_roce_v2(ibv_get_device_name(devlist[i]), port, gidx)) {
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

int main(int argc, char *argv[]) {
  double _ts0 = now_sec();
  struct ibv_context *ctx = NULL;
  struct ibv_pd *pd = NULL;
  struct ibv_comp_channel *comp_chan = NULL;
  struct ibv_cq *cq = NULL;
  struct ibv_cq *evt_cq = NULL;
  struct ibv_mr *mr = NULL;
  struct ibv_qp *qp = NULL;
  struct ibv_qp_init_attr qp_attr = {};
  struct ibv_sge sge;
  struct ibv_send_wr send_wr = {};
  struct ibv_send_wr *bad_send_wr;
  struct ibv_recv_wr recv_wr = {};
  struct ibv_recv_wr *bad_recv_wr;
  struct ibv_wc wc;
  void *cq_context;

  struct sockaddr_in sin;

  int err;
  uint32_t event_count = 0;

  int port;
  char *key, *ports;
  uint32_t keylen, keyIdx = 0;

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

  if (argv_idx >= argc) {
    usage();
    return 1;
  }
  const char *filename = argv[argv_idx];

  /* Open output file (no O_TRUNC so existing pages survive for warm rewrites) */
  int fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (fd < 0) {
    fprintf(stderr, "Error opening file %s (%d)\n", filename, errno);
    return 200;
  }

  /* TCP: accept connection and receive params from sender */
  int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_sock < 0) return 1;

  int optval = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = INADDR_ANY;

  if (bind(listen_sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
    fprintf(stderr, "bind failed: %d\n", errno);
    return 1;
  }
  if (listen(listen_sock, 1) < 0) return 1;

  struct sockaddr_in peer_addr;
  socklen_t peer_len = sizeof(peer_addr);
  int tcp_sock = accept(listen_sock, (struct sockaddr *)&peer_addr, &peer_len);
  close(listen_sock);
  if (tcp_sock < 0) return 1;

  struct rdma_params params;
  if (read(tcp_sock, &params, sizeof(params)) != sizeof(params)) {
    fprintf(stderr, "Failed to receive params from sender\n");
    return 1;
  }
  uint32_t buf_size  = params.buf_size;
  uint32_t ring_size = params.ring_size;
  if (ring_size < 2) ring_size = 2;

  /* Map output file into memory for zero-copy writes.
   * Pre-size to file_size so we do one ftruncate now and one trim at the end.
   * Opening without O_TRUNC preserves existing pages for warm rewrites. */
  char   *file_map      = NULL;
  size_t  file_map_size = 0;
  if (params.file_size > 0) {
    TS("ftruncate+mmap start");
    file_map_size = params.file_size;
    if (ftruncate(fd, (off_t)file_map_size) < 0) {
      perror("ftruncate"); return 1;
    }
    file_map = mmap(NULL, file_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (file_map == MAP_FAILED) { perror("mmap file"); return 1; }
    TS("ftruncate+mmap done");
  }

  /* Allocate RDMA ring (anonymous, separate from file mapping) */
  size_t ring_bytes = (size_t)buf_size * ring_size;
  TS("mmap ring start");
  char *ring_base = mmap(NULL, ring_bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ring_base == MAP_FAILED) { perror("mmap ring failed"); return 1; }
  madvise(ring_base, ring_bytes, MADV_HUGEPAGE);
  TS("mmap ring done");

  void **slots = malloc(ring_size * sizeof(void *));
  if (!slots) { perror("malloc"); return 1; }
  for (uint32_t s = 0; s < ring_size; s++)
    slots[s] = ring_base + (size_t)s * buf_size;

  int recv_slot = 0;
  int next_recv = 1 % (int)ring_size;

  /* Find RDMA device from local IP of accepted TCP connection */
  struct sockaddr_in local_addr;
  socklen_t local_len = sizeof(local_addr);
  getsockname(tcp_sock, (struct sockaddr *)&local_addr, &local_len);
  uint32_t local_ip = local_addr.sin_addr.s_addr;

  uint8_t port_num;
  int gid_idx;
  union ibv_gid local_gid;
  ctx = find_rdma_device_by_ip(local_ip, &port_num, &gid_idx, &local_gid);
  if (!ctx) {
    fprintf(stderr, "No RDMA device found for local IP\n");
    close(tcp_sock);
    return 1;
  }

  pd = ibv_alloc_pd(ctx);
  if (!pd) return 1;

  comp_chan = ibv_create_comp_channel(ctx);
  if (!comp_chan) return 1;

  cq = ibv_create_cq(ctx, 2, NULL, comp_chan, 0);
  if (!cq) return 1;

  if (ibv_req_notify_cq(cq, 0)) return 1;

  TS("ibv_reg_mr start");
  mr = ibv_reg_mr(pd, ring_base, ring_bytes,
                  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                      IBV_ACCESS_REMOTE_WRITE);
  if (!mr) return 1;
  TS("ibv_reg_mr done");

  qp_attr.send_cq = cq;
  qp_attr.recv_cq = cq;
  qp_attr.qp_type = IBV_QPT_RC;
  qp_attr.cap.max_send_wr = 2;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_wr = 1;
  qp_attr.cap.max_recv_sge = 1;

  qp = ibv_create_qp(pd, &qp_attr);
  if (!qp) return 1;

  struct ibv_qp_attr init_attr = {
      .qp_state = IBV_QPS_INIT,
      .pkey_index = 0,
      .port_num = port_num,
      .qp_access_flags =
          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE,
  };
  err = ibv_modify_qp(qp, &init_attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS);
  if (err) return err;

  /* Post initial recv into slot 0 */
  sge.addr   = (uintptr_t)slots[0];
  sge.length = buf_size;
  sge.lkey   = mr->lkey;
  recv_wr.sg_list = &sge;
  recv_wr.num_sge = 1;
  if (ibv_post_recv(qp, &recv_wr, &bad_recv_wr))
    return 1;

  TS("QP ready");
  /* Exchange QP info over the same TCP connection */
  uint32_t local_psn = (uint32_t)(lrand48() & 0xFFFFFF);
  struct qp_info local_info = {
      .qpn = qp->qp_num,
      .psn = local_psn,
      .gid = local_gid,
  };
  struct qp_info remote_info;

  ssize_t n = write(tcp_sock, &local_info, sizeof(local_info));
  if (n != sizeof(local_info)) {
    fprintf(stderr, "TCP handshake write failed\n");
    return 1;
  }
  n = read(tcp_sock, &remote_info, sizeof(remote_info));
  if (n != sizeof(remote_info)) {
    fprintf(stderr, "TCP handshake read failed\n");
    return 1;
  }
  close(tcp_sock);

  struct ibv_qp_attr rtr_attr = {
      .qp_state = IBV_QPS_RTR,
      .path_mtu = IBV_MTU_4096,
      .dest_qp_num = remote_info.qpn,
      .rq_psn = remote_info.psn,
      .max_dest_rd_atomic = 1,
      .min_rnr_timer = 12,
      .ah_attr = {
          .is_global = 1,
          .grh = {
              .dgid = remote_info.gid,
              .sgid_index = gid_idx,
              .hop_limit = 64,
          },
          .sl = 0,
          .port_num = port_num,
      },
  };
  err = ibv_modify_qp(qp, &rtr_attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                          IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                          IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
  if (err) return err;

  struct ibv_qp_attr rts_attr = {
      .qp_state = IBV_QPS_RTS,
      .timeout = 14,
      .retry_cnt = 7,
      .rnr_retry = 7,
      .sq_psn = local_psn,
      .max_rd_atomic = 1,
  };
  err = ibv_modify_qp(qp, &rts_attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                          IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                          IBV_QP_MAX_QP_RD_ATOMIC);
  if (err) return err;

  send_wr.opcode     = IBV_WR_SEND;
  send_wr.send_flags = IBV_SEND_SIGNALED;
  send_wr.sg_list    = &sge;
  send_wr.num_sge    = 1;

  ssize_t total_bytes = 0;

  TS("transfer start");
  while (1) {
    /* Wait for receive completion into slots[recv_slot] */
    if (ibv_get_cq_event(comp_chan, &evt_cq, &cq_context)) return 1;
    if (ibv_req_notify_cq(cq, 0))                          return 2;
    if (ibv_poll_cq(cq, 1, &wc) < 1)                      return 3;
    if (wc.status != IBV_WC_SUCCESS) {
      fprintf(stderr, "%s\n", ibv_wc_status_str(wc.status));
      return 4;
    }

    uint32_t msgLen = wc.byte_len;

    /* Post next recv immediately so sender can start the next chunk */
    if (msgLen > 0) {
      sge.addr   = (uintptr_t)slots[next_recv];
      sge.length = buf_size;
      if (ibv_post_recv(qp, &recv_wr, &bad_recv_wr)) return 5;
    }

    /* ACK the sender */
    sge.length = 1;
    if (ibv_post_send(qp, &send_wr, &bad_send_wr)) return 6;
    sge.length = buf_size;

    if (ibv_get_cq_event(comp_chan, &evt_cq, &cq_context)) return 7;
    if (ibv_req_notify_cq(cq, 0))                          return 8;
    if (ibv_poll_cq(cq, 1, &wc) < 1)                      return 9;
    if (wc.status != IBV_WC_SUCCESS)                       return 10;

    event_count += 2;

    /* Key verification and write */
    if (msgLen <= buf_size) {
      char *cbuf = (char *)slots[recv_slot];
      uint32_t i;
      for (i = 0; i < msgLen && keyIdx < keylen + 1; i++, keyIdx++, cbuf++) {
        if (*cbuf != key[keyIdx]) {
          wrongkey();
          ibv_ack_cq_events(cq, event_count);
          return 20;
        }
      }
      if (i == 0 && msgLen > 0) {
        if (file_map) {
          memcpy(file_map + total_bytes, cbuf, msgLen);
        } else {
          ssize_t res = write(fd, cbuf, msgLen);
          while (res >= 0 && (size_t)res < msgLen)
            res += write(fd, cbuf + res, msgLen - res);
          if (res < 0) { fprintf(stderr, "Write error %d\n", errno); break; }
        }
        total_bytes += msgLen;
      }
    }

    if (msgLen == 0) { TS("transfer done"); break; }

    recv_slot = next_recv;
    next_recv = (recv_slot + 1) % (int)ring_size;
  }

  /* Trim file to actual bytes written */
  if (file_map) {
    TS("munmap+ftruncate start");
    munmap(file_map, file_map_size);
    if (ftruncate(fd, (off_t)total_bytes) < 0) perror("ftruncate final");
    TS("munmap+ftruncate done");
  }
  close(fd);
  free(slots);

  ibv_ack_cq_events(cq, event_count);
  ibv_destroy_qp(qp);
  ibv_dereg_mr(mr);
  ibv_destroy_cq(cq);
  ibv_dealloc_pd(pd);
  ibv_destroy_comp_channel(comp_chan);
  ibv_close_device(ctx);

  munmap(ring_base, ring_bytes);

  return 0;
}
