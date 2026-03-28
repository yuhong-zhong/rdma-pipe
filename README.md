# rdma-pipe

Fast file transfer over RoCEv2 RDMA. Achieves ~7 GB/s from NVMe and ~11 GB/s from memory on a 100 Gbps link.

## Requirements

Both sender and receiver need:

- Mellanox/NVIDIA ConnectX NIC with RoCEv2 GIDs configured (`ibv_devinfo` should show `link_layer: Ethernet` and active ports)
- libibverbs (`apt install libibverbs-dev`)
- OpenMP (`apt install libgomp1`)
- SSH access between hosts (passwordless recommended)

Verify RoCEv2 GIDs are present on each host:

```bash
for dev in /sys/class/infiniband/*/; do
  devname=$(basename $dev)
  for i in $(seq 0 5); do
    type=$(cat $dev/ports/1/gid_attrs/types/$i 2>/dev/null)
    gid=$(cat $dev/ports/1/gids/$i 2>/dev/null)
    [ "$type" = "RoCE v2" ] && echo "$devname gid[$i] $gid"
  done
done
```

You should see at least one line containing an IPv4-mapped GID (`0000:...:ffff:a.b.c.d`).

### Jumbo frames (recommended)

Enabling 9000-byte MTU on both hosts and the switch reduces packet overhead and improves throughput:

```bash
# Run on both hosts
sudo ip link set enp65s0f0np0 mtu 9000
sudo ip link set enp65s0f1np1 mtu 9000
```

To persist across reboots, add to `/etc/network/interfaces`:

```
iface enp65s0f0np0 inet static
    mtu 9000
```

Or with systemd-networkd, add `MTUBytes=9000` under `[Link]` in the relevant `.network` file. Also configure 9000 MTU on the switch ports.

### Pinned memory limit

Check that non-root users can pin enough memory:

```bash
ulimit -l   # result is in kB; needs to be > 70000 for 32 MiB buffers
```

If too low, create `/etc/security/limits.d/rdma.conf`:

```
*  soft  memlock  unlimited
*  hard  memlock  unlimited
```

Log out and back in for the change to take effect.

---

## Build

```bash
git clone <repo>
cd rdma-pipe
make
```

---

## Install

### Local host

```bash
sudo make install
```

Installs `rdsend`, `rdrecv`, `rdcp`, and `rdpipe` to `/usr/local/bin`.

### Remote host

Build on the local host, then copy the binaries:

```bash
scp rdsend rdrecv rdcp rdpipe remote-host:/tmp/
ssh remote-host sudo cp /tmp/rdsend /tmp/rdrecv /tmp/rdcp /tmp/rdpipe /usr/local/bin/
```

---

## Using /dev/shm for maximum throughput

`/dev/shm` is a RAM-backed tmpfs filesystem. Reading from or writing to it bypasses disk I/O entirely. When the source file is in `/dev/shm`, `rdcp` achieves ~11 GB/s (91% of 100 Gbps line rate) instead of being limited by NVMe speed (~7 GB/s).

`/dev/shm` exists by default on Linux — no setup required. Its size defaults to half of installed RAM. To check:

```bash
df -h /dev/shm
```

To stage a file into RAM before transfer:

```bash
cp /path/to/bigfile /dev/shm/bigfile
rdcp -v /dev/shm/bigfile remote-host:/destination
rm /dev/shm/bigfile
```

To increase the tmpfs size (e.g. to 200 GB):

```bash
sudo mount -o remount,size=200G /dev/shm
```

To make the larger size persist across reboots, add to `/etc/fstab`:

```
tmpfs  /dev/shm  tmpfs  defaults,size=200G  0  0
```

---

## Running rdcp

`rdcp` copies a file from source to destination over RDMA. Either end can be remote. It uses SSH to start `rdrecv` on the remote host and then transfers the data directly over the RDMA fabric.

```
rdcp [-v] [-r] SRC DST

  -v   Print bandwidth at the end.
  -r   Copy a directory tree (uses tar internally).

  SRC and DST are either a local path or host:path.
```

### Examples

```bash
# Copy a local file to a remote host
rdcp -v bigfile.bin 10.10.20.12:/data/bigfile.bin

# Copy from remote to local
rdcp -v 10.10.20.12:/data/bigfile.bin ./bigfile.bin

# Copy from /dev/shm for maximum throughput (~11 GB/s on 100G)
rdcp -v /dev/shm/bigfile.bin 10.10.20.12:/data/bigfile.bin

# Send to /dev/null to benchmark RDMA throughput
rdcp -v /dev/shm/testfile 10.10.20.12:/dev/null

# Copy a directory
rdcp -r -v mydir/ 10.10.20.12:/data/mydir/

# With a user in the remote path
rdcp -v bigfile.bin user@10.10.20.12:/data/bigfile.bin
```

### Observed throughput (ConnectX-5, 100 Gbps RoCEv2, jumbo frames)

| Source         | Destination    | Bandwidth  |
|----------------|----------------|------------|
| NVMe           | `/dev/null`    | ~7.0 GB/s  |
| `/dev/shm`     | `/dev/null`    | ~11.4 GB/s |
| `/dev/shm`     | NVMe           | limited by remote write speed |

---

## rdsend / rdrecv (low-level)

For piping data manually without SSH setup:

```bash
# On receiving host — listen on port 12345 with key "mykey", write to a file
rdrecv 12345 mykey /destination/file

# On sending host — connect and send from a file
rdsend -v 10.10.20.12 12345 mykey /source/file

# Or pipe from stdin
some_command | rdsend -v 10.10.20.12 12345 mykey
```

`rdsend` retries the connection for ~3 seconds, so `rdrecv` can be started first or shortly after.

---

## Troubleshooting

**`No RDMA device found for target <IP>`**
The tool couldn't find a RoCEv2 GID matching the local IP that routes to the target. Check that RoCEv2 GIDs are configured (see Requirements above) and that the target IP is reachable over the RDMA interface.

**`transport retry counter exceeded`**
The QP sent packets but got no ACKs. Usually a GID type mismatch (RoCEv1 vs RoCEv2) or MTU misconfiguration. Verify both sides have the same MTU setting.

**`Wrong key received`**
The key passed to `rdsend` and `rdrecv` must match exactly.

**ibacm is not needed**
This version uses a TCP socket for QP handshake and raw ibverbs for data transfer — `ibacm` is not required.
