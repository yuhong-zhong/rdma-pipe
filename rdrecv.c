/*
 * build:
 * cc -o rdrecv rdrecv.c -libverbs
 *
 * usage:
 * rdrecv [-v] <port> <key> <filename>
 *
 * Receives an RDMA transfer from rdsend and writes it to <filename>.
 *
 * When the sender provides file_size (normal file-to-file transfer), the output
 * file is mmap'd and registered as the RDMA MR directly — the NIC DMA-writes
 * into the file pages with no CPU memcpy (zero-copy).
 *
 * For unknown-size sources (file_size == 0, e.g. stdin on sender side), falls
 * back to an anonymous ring buffer + write().
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

struct qp_info {
  uint32_t qpn;
  uint32_t psn;
  union ibv_gid gid;
};

struct rdma_params {
  uint32_t buf_size;
  uint32_t ring_size;
  uint64_t file_size;
};

static double now_sec(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}
#define TS(label) do { if (verbose) fprintf(stderr, "TS %.3f %s\n", now_sec() - _ts0, label); } while(0)

static int gid_is_roce_v2(const char *devname, uint8_t port, int gidx) {
  char path[256], typebuf[32] = {};
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

static struct ibv_context *find_rdma_device_by_ip(uint32_t local_ip,
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
    if (ibv_query_device(ctx, &attr)) { ibv_close_device(ctx); continue; }

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
            *port_out    = port;
            *gid_idx_out = gidx;
            *gid_out     = gid;
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

  int verbose = 0, argv_idx = 1;
  if (argc < 4) { fprintf(stderr, "USAGE: rdrecv [-v] <port> <key> <filename>\n"); return 1; }
  if (strcmp(argv[argv_idx], "-v") == 0) { verbose++; argv_idx++; }

  int port = atoi(argv[argv_idx++]);
  if (port < 1 || port > 65535) { fprintf(stderr, "bad port\n"); return 1; }

  char    *key    = argv[argv_idx++];
  uint32_t keylen = strlen(key);
  uint32_t keyIdx = 0;

  if (argv_idx >= argc) { fprintf(stderr, "USAGE: rdrecv [-v] <port> <key> <filename>\n"); return 1; }
  const char *filename = argv[argv_idx];

  /* Open without O_TRUNC: existing pages survive for warm rewrites */
  int fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (fd < 0) { fprintf(stderr, "open %s: %s\n", filename, strerror(errno)); return 1; }

  /* TCP: accept and receive params */
  int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
  int optval = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
  struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = INADDR_ANY };
  if (bind(listen_sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
    fprintf(stderr, "bind: %s\n", strerror(errno)); return 1;
  }
  listen(listen_sock, 1);

  struct sockaddr_in peer_addr;
  socklen_t peer_len = sizeof(peer_addr);
  int tcp_sock = accept(listen_sock, (struct sockaddr *)&peer_addr, &peer_len);
  close(listen_sock);
  if (tcp_sock < 0) return 1;

  struct rdma_params params;
  if (read(tcp_sock, &params, sizeof(params)) != sizeof(params)) {
    fprintf(stderr, "failed to receive params\n"); return 1;
  }
  uint32_t buf_size  = params.buf_size;
  uint32_t ring_size = params.ring_size < 2 ? 2 : params.ring_size;

  /*
   * Buffer setup:
   *   file_size > 0 → zero-copy: mmap the output file as the RDMA receive
   *                   buffer; NIC DMA-writes directly into file pages.
   *   file_size == 0 → fallback: anonymous ring + write().
   *
   * For zero-copy we need buf_size extra bytes beyond file_size:
   *   [0 .. file_size-1]         data pages (RDMA writes land here)
   *   [file_size .. file_size+buf_size-1]  scratch: key recv + sentinel recv
   */
  char  *rdma_buf      = NULL;   /* base of RDMA MR */
  size_t rdma_buf_size = 0;
  char  *key_buf       = NULL;   /* where the key chunk lands */
  char  *file_map      = NULL;   /* non-NULL if using zero-copy */
  size_t file_map_size = 0;

  /* fallback ring state (used only when file_map == NULL) */
  void **slots      = NULL;
  int    recv_slot  = 0;
  int    next_recv  = 0;

  if (params.file_size > 0) {
    TS("file mmap start");
    file_map_size = params.file_size + buf_size;   /* +buf_size scratch */
    if (ftruncate(fd, (off_t)file_map_size) < 0) { perror("ftruncate"); return 1; }
    file_map = mmap(NULL, file_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (file_map == MAP_FAILED) { perror("mmap file"); return 1; }
    madvise(file_map, file_map_size, MADV_HUGEPAGE);
    key_buf       = file_map + params.file_size;   /* scratch at end */
    rdma_buf      = file_map;
    rdma_buf_size = file_map_size;
    TS("file mmap done");
  } else {
    TS("ring mmap start");
    rdma_buf_size = (size_t)buf_size * ring_size;
    rdma_buf = mmap(NULL, rdma_buf_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rdma_buf == MAP_FAILED) { perror("mmap ring"); return 1; }
    madvise(rdma_buf, rdma_buf_size, MADV_HUGEPAGE);
    slots = malloc(ring_size * sizeof(void *));
    if (!slots) { perror("malloc"); return 1; }
    for (uint32_t s = 0; s < ring_size; s++)
      slots[s] = rdma_buf + (size_t)s * buf_size;
    key_buf    = rdma_buf;           /* key goes to slot 0 */
    recv_slot  = 0;
    next_recv  = 1 % (int)ring_size;
    TS("ring mmap done");
  }

  /* RDMA device */
  struct sockaddr_in local_addr;
  socklen_t local_len = sizeof(local_addr);
  getsockname(tcp_sock, (struct sockaddr *)&local_addr, &local_len);

  uint8_t port_num; int gid_idx; union ibv_gid local_gid;
  struct ibv_context *ctx =
      find_rdma_device_by_ip(local_addr.sin_addr.s_addr, &port_num, &gid_idx, &local_gid);
  if (!ctx) { fprintf(stderr, "no RDMA device\n"); return 1; }

  struct ibv_pd           *pd        = ibv_alloc_pd(ctx);
  struct ibv_comp_channel *comp_chan = ibv_create_comp_channel(ctx);
  struct ibv_cq           *cq       = ibv_create_cq(ctx, 2, NULL, comp_chan, 0);
  if (!pd || !comp_chan || !cq) return 1;
  ibv_req_notify_cq(cq, 0);

  TS("ibv_reg_mr start");
  struct ibv_mr *mr = ibv_reg_mr(pd, rdma_buf, rdma_buf_size,
                                 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                                 IBV_ACCESS_REMOTE_WRITE);
  if (!mr) { fprintf(stderr, "ibv_reg_mr failed\n"); return 1; }
  TS("ibv_reg_mr done");

  struct ibv_qp_init_attr qp_attr = {
    .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
    .cap = { .max_send_wr=2, .max_send_sge=1, .max_recv_wr=1, .max_recv_sge=1 },
  };
  struct ibv_qp *qp = ibv_create_qp(pd, &qp_attr);
  if (!qp) return 1;

  struct ibv_qp_attr init_attr = {
    .qp_state = IBV_QPS_INIT, .pkey_index = 0, .port_num = port_num,
    .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE,
  };
  if (ibv_modify_qp(qp, &init_attr,
                    IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS)) return 1;

  /* Post initial recv for key chunk */
  struct ibv_sge sge = { .addr=(uintptr_t)key_buf, .length=buf_size, .lkey=mr->lkey };
  struct ibv_recv_wr recv_wr = { .sg_list=&sge, .num_sge=1 };
  struct ibv_recv_wr *bad_recv_wr;
  struct ibv_send_wr send_wr = { .opcode=IBV_WR_SEND, .send_flags=IBV_SEND_SIGNALED,
                                  .sg_list=&sge, .num_sge=1 };
  struct ibv_send_wr *bad_send_wr;
  if (ibv_post_recv(qp, &recv_wr, &bad_recv_wr)) return 1;

  /* QP handshake */
  TS("QP ready");
  uint32_t local_psn = (uint32_t)(lrand48() & 0xFFFFFF);
  struct qp_info local_info  = { .qpn=qp->qp_num, .psn=local_psn, .gid=local_gid };
  struct qp_info remote_info;
  if (write(tcp_sock, &local_info,   sizeof(local_info))   != sizeof(local_info))   return 1;
  if (read (tcp_sock, &remote_info,  sizeof(remote_info))  != sizeof(remote_info))  return 1;
  close(tcp_sock);

  struct ibv_qp_attr rtr_attr = {
    .qp_state=IBV_QPS_RTR, .path_mtu=IBV_MTU_4096,
    .dest_qp_num=remote_info.qpn, .rq_psn=remote_info.psn,
    .max_dest_rd_atomic=1, .min_rnr_timer=12,
    .ah_attr={ .is_global=1,
               .grh={ .dgid=remote_info.gid, .sgid_index=gid_idx, .hop_limit=64 },
               .sl=0, .port_num=port_num },
  };
  if (ibv_modify_qp(qp, &rtr_attr,
                    IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|
                    IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER)) return 1;

  struct ibv_qp_attr rts_attr = {
    .qp_state=IBV_QPS_RTS, .timeout=14, .retry_cnt=7, .rnr_retry=7,
    .sq_psn=local_psn, .max_rd_atomic=1,
  };
  if (ibv_modify_qp(qp, &rts_attr,
                    IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|
                    IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC)) return 1;

  struct ibv_wc wc;
  struct ibv_cq *evt_cq; void *cq_ctx;
  uint32_t event_count = 0;
  ssize_t  total_bytes = 0;

  TS("transfer start");

  if (file_map) {
    /*
     * Zero-copy path: NIC DMA-writes each chunk directly into file_map.
     * recv_buf tracks where the CURRENT pending recv WR was posted.
     * next_post_offset is the file offset for the NEXT recv WR to post.
     *
     * Initial state: first recv (key) is at key_buf (= file_map + file_size).
     * After key: subsequent recvs go to file_map + 0, buf_size, 2*buf_size, ...
     */
    char  *recv_buf        = key_buf;
    size_t next_post_offset = 0;
    int    key_done         = 0;

    while (1) {
      if (ibv_get_cq_event(comp_chan, &evt_cq, &cq_ctx)) return 1;
      if (ibv_req_notify_cq(cq, 0))                      return 2;
      if (ibv_poll_cq(cq, 1, &wc) < 1)                   return 3;
      if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "%s\n", ibv_wc_status_str(wc.status)); return 4;
      }
      uint32_t msgLen = wc.byte_len;

      /* Post next recv directly into file_map (or scratch if sentinel) */
      if (msgLen > 0) {
        sge.addr   = (uintptr_t)(file_map + next_post_offset);
        sge.length = buf_size;
        if (ibv_post_recv(qp, &recv_wr, &bad_recv_wr)) return 5;
      }

      /* ACK */
      sge.length = 1;
      if (ibv_post_send(qp, &send_wr, &bad_send_wr)) return 6;
      sge.length = buf_size;
      if (ibv_get_cq_event(comp_chan, &evt_cq, &cq_ctx)) return 7;
      if (ibv_req_notify_cq(cq, 0))                      return 8;
      if (ibv_poll_cq(cq, 1, &wc) < 1)                   return 9;
      if (wc.status != IBV_WC_SUCCESS)                    return 10;
      event_count += 2;

      if (!key_done) {
        /* Verify key from key_buf (recv_buf == key_buf) */
        char *cbuf = recv_buf;
        for (uint32_t i = 0; i < msgLen && keyIdx < keylen+1; i++, keyIdx++, cbuf++) {
          if (*cbuf != key[keyIdx]) {
            fprintf(stderr, "Wrong key received\n");
            ibv_ack_cq_events(cq, event_count);
            return 20;
          }
        }
        key_done = 1;
      } else {
        /* Data already in file at recv_buf — no copy needed */
        if (msgLen > 0) total_bytes += msgLen;
      }

      if (msgLen == 0) { TS("transfer done"); break; }

      /* Advance: next iteration's recv_buf = where we just posted */
      recv_buf         = file_map + next_post_offset;
      next_post_offset += buf_size;
    }

    /* Trim to actual size */
    TS("ftruncate start");
    munmap(file_map, file_map_size);
    if (ftruncate(fd, (off_t)total_bytes) < 0) perror("ftruncate final");
    TS("ftruncate done");

  } else {
    /*
     * Fallback path (file_size == 0): anonymous ring + write().
     * Uses ring_size slots; key goes to slot 0.
     */
    while (1) {
      if (ibv_get_cq_event(comp_chan, &evt_cq, &cq_ctx)) return 1;
      if (ibv_req_notify_cq(cq, 0))                      return 2;
      if (ibv_poll_cq(cq, 1, &wc) < 1)                   return 3;
      if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "%s\n", ibv_wc_status_str(wc.status)); return 4;
      }
      uint32_t msgLen = wc.byte_len;

      if (msgLen > 0) {
        sge.addr   = (uintptr_t)slots[next_recv];
        sge.length = buf_size;
        if (ibv_post_recv(qp, &recv_wr, &bad_recv_wr)) return 5;
      }

      sge.length = 1;
      if (ibv_post_send(qp, &send_wr, &bad_send_wr)) return 6;
      sge.length = buf_size;
      if (ibv_get_cq_event(comp_chan, &evt_cq, &cq_ctx)) return 7;
      if (ibv_req_notify_cq(cq, 0))                      return 8;
      if (ibv_poll_cq(cq, 1, &wc) < 1)                   return 9;
      if (wc.status != IBV_WC_SUCCESS)                    return 10;
      event_count += 2;

      if (msgLen <= buf_size) {
        char *cbuf = (char *)slots[recv_slot];
        uint32_t i;
        for (i = 0; i < msgLen && keyIdx < keylen+1; i++, keyIdx++, cbuf++) {
          if (*cbuf != key[keyIdx]) {
            fprintf(stderr, "Wrong key received\n");
            ibv_ack_cq_events(cq, event_count);
            return 20;
          }
        }
        if (i == 0 && msgLen > 0) {
          ssize_t res = write(fd, cbuf, msgLen);
          while (res >= 0 && (size_t)res < msgLen)
            res += write(fd, cbuf + res, msgLen - res);
          if (res < 0) { fprintf(stderr, "write error: %s\n", strerror(errno)); break; }
          total_bytes += msgLen;
        }
      }

      if (msgLen == 0) { TS("transfer done"); break; }

      recv_slot = next_recv;
      next_recv = (recv_slot + 1) % (int)ring_size;
    }
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

  if (!file_map)
    munmap(rdma_buf, rdma_buf_size);

  return 0;
}
