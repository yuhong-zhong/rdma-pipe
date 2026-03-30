/*
 * build:
 * cc -o rdrecv rdrecv.c -libverbs -luring
 *
 * usage:
 * rdrecv <port> <key>
 *
 * Waits for client to connect to given port and authenticate with the key.
 * Uses a TCP socket for QP handshake to avoid ibacm dependency.
 *
 * Environment variables:
 *   RDMA_BUF_SIZE   - buffer size in MB (default 32)
 *   RDMA_RING_SIZE  - number of ring slots (default 4); allows up to
 *                     RDMA_RING_SIZE-1 io_uring writes in flight at once
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <liburing.h>

enum {
  RESOLVE_TIMEOUT_MS = 5000,
};

struct qp_info {
  uint32_t qpn;
  uint32_t psn;
  union ibv_gid gid;
};

void usage() {
  fprintf(stderr, "USAGE: rdrecv [-v] <port> <key> [filename]\n");
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
        // RoCEv2 IPv4-mapped GID: bytes 0-9 zero, 10-11 0xFF, 12-15 IPv4
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

  if (argc < 3) {
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

  /* Buffer and ring sizing from environment */
  uint32_t buf_size = 32 * 1024 * 1024;
  const char *bs_env = getenv("RDMA_BUF_SIZE");
  if (bs_env) buf_size = (uint32_t)atoi(bs_env) * 1024 * 1024;

  uint32_t ring_size = 4;
  const char *rs_env = getenv("RDMA_RING_SIZE");
  if (rs_env) ring_size = (uint32_t)atoi(rs_env);
  if (ring_size < 2) ring_size = 2;

  /* Allocate ring slots as one contiguous pinned region */
  char *ring_base;
  if (posix_memalign((void **)&ring_base, 4096, (size_t)buf_size * ring_size)) {
    perror("posix_memalign failed");
    return 1;
  }

  void **slots = malloc(ring_size * sizeof(void *));
  int  *slot_pending = calloc(ring_size, sizeof(int));
  if (!slots || !slot_pending) { perror("malloc"); return 1; }
  for (uint32_t s = 0; s < ring_size; s++)
    slots[s] = ring_base + (size_t)s * buf_size;

  int recv_slot = 0;   /* slot data just arrived into */
  int next_recv = 1 % (int)ring_size; /* slot to post next recv into */

  int use_uring = 0;
  struct io_uring ring;

  int fd = STDOUT_FILENO;
  if (argv_idx < argc) {
    /* Try O_DIRECT first (good for NVMe); fall back for tmpfs/char devices */
    fd = open(argv[argv_idx], O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT,
              S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
      fd = open(argv[argv_idx], O_WRONLY | O_CREAT | O_TRUNC,
                S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    }
    if (fd < 0) {
      fprintf(stderr, "Error opening file %s (%d)\n", argv[argv_idx], errno);
      return 200;
    }

    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
      /* ring_size entries in the SQ: one per in-flight write */
      if (io_uring_queue_init(ring_size, &ring, 0) == 0) {
        use_uring = 1;
      }
    }
  }

  // TCP listen for handshake
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

  // Find local IP of accepted connection to select the right RDMA device
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

  mr = ibv_reg_mr(pd, ring_base, (size_t)buf_size * ring_size,
                  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                      IBV_ACCESS_REMOTE_WRITE);
  if (!mr) return 1;

  qp_attr.send_cq = cq;
  qp_attr.recv_cq = cq;
  qp_attr.qp_type = IBV_QPT_RC;
  qp_attr.cap.max_send_wr = 2;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_wr = 1;
  qp_attr.cap.max_recv_sge = 1;

  qp = ibv_create_qp(pd, &qp_attr);
  if (!qp) return 1;

  // Move QP to INIT
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

  /* Post initial receive into slot 0 */
  sge.addr = (uintptr_t)slots[0];
  sge.length = buf_size;
  sge.lkey = mr->lkey;

  recv_wr.sg_list = &sge;
  recv_wr.num_sge = 1;

  if (ibv_post_recv(qp, &recv_wr, &bad_recv_wr))
    return 1;

  // Exchange QP info via TCP (rdrecv sends first)
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
  err = ibv_modify_qp(qp, &rtr_attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                          IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                          IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
  if (err) return err;

  // Move QP to RTS
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

  // Set up send WR for ACKs
  send_wr.opcode = IBV_WR_SEND;
  send_wr.send_flags = IBV_SEND_SIGNALED;
  send_wr.sg_list = &sge;
  send_wr.num_sge = 1;

  ssize_t total_bytes = 0;

  while (1) {
    /* Wait for receive completion into slots[recv_slot] */
    if (ibv_get_cq_event(comp_chan, &evt_cq, &cq_context))
      return 1;
    if (ibv_req_notify_cq(cq, 0))
      return 2;
    if (ibv_poll_cq(cq, 1, &wc) < 1)
      return 3;
    if (wc.status != IBV_WC_SUCCESS) {
      fprintf(stderr, "%s\n", ibv_wc_status_str(wc.status));
      return 4;
    }

    uint32_t msgLen = wc.byte_len;

    /*
     * Before reusing slots[next_recv] for the next recv, ensure any
     * in-flight io_uring write on that slot has completed.  Drain cqes
     * until the slot is free; handles out-of-order completions.
     */
    while (use_uring && slot_pending[next_recv]) {
      struct io_uring_cqe *cqe;
      if (io_uring_wait_cqe(&ring, &cqe) < 0) return 5;
      if (cqe->res < 0) {
        fprintf(stderr, "io_uring write error: %s\n", strerror(-cqe->res));
        io_uring_cqe_seen(&ring, cqe);
        return 5;
      }
      int done = (int)(uintptr_t)io_uring_cqe_get_data(cqe);
      slot_pending[done] = 0;
      io_uring_cqe_seen(&ring, cqe);
    }

    /* Post recv into next_recv immediately so sender can start next chunk */
    if (msgLen > 0) {
      sge.addr = (uintptr_t)slots[next_recv];
      sge.length = buf_size;
      if (ibv_post_recv(qp, &recv_wr, &bad_recv_wr))
        return 5;
    }

    /* ACK the sender */
    sge.length = 1;
    if (ibv_post_send(qp, &send_wr, &bad_send_wr))
      return 6;
    sge.length = buf_size;

    /* Wait for ACK send completion */
    if (ibv_get_cq_event(comp_chan, &evt_cq, &cq_context))
      return 7;
    if (ibv_req_notify_cq(cq, 0))
      return 8;
    if (ibv_poll_cq(cq, 1, &wc) < 1)
      return 9;
    if (wc.status != IBV_WC_SUCCESS)
      return 10;

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
      if (i == 0) {
        if (use_uring) {
          struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
          io_uring_prep_write(sqe, fd, cbuf, msgLen, total_bytes);
          io_uring_sqe_set_data(sqe, (void *)(uintptr_t)recv_slot);
          io_uring_submit(&ring);
          slot_pending[recv_slot] = 1;
          total_bytes += msgLen;
        } else {
          lseek(fd, total_bytes, SEEK_SET);
          ssize_t res = write(fd, cbuf, msgLen);
          while (res >= 0 && (size_t)res < msgLen) {
            res += write(fd, cbuf + res, msgLen - res);
          }
          if (res < 0) {
            fprintf(stderr, "Write error %d\n", errno);
            break;
          }
          total_bytes += res;
        }
      }
    }

    if (msgLen == 0)
      break;

    recv_slot = next_recv;
    next_recv = (recv_slot + 1) % (int)ring_size;
  }

  /* Drain all remaining in-flight io_uring writes */
  if (use_uring) {
    for (uint32_t s = 0; s < ring_size; s++) {
      if (slot_pending[s]) {
        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(&ring, &cqe) == 0) {
          if (cqe->res < 0)
            fprintf(stderr, "io_uring final write error: %s\n",
                    strerror(-cqe->res));
          int done = (int)(uintptr_t)io_uring_cqe_get_data(cqe);
          slot_pending[done] = 0;
          io_uring_cqe_seen(&ring, cqe);
        }
      }
    }
    io_uring_queue_exit(&ring);
  }

  close(fd);
  free(slots);
  free(slot_pending);

  ibv_ack_cq_events(cq, event_count);

  ibv_destroy_qp(qp);
  ibv_dereg_mr(mr);
  ibv_destroy_cq(cq);
  ibv_dealloc_pd(pd);
  ibv_destroy_comp_channel(comp_chan);
  ibv_close_device(ctx);

  return 0;
}
