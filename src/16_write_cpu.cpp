// clang-format off
/*
Example run:

server$ ./build/16_write_cpu
domain:  rdmap79s0-rdm, nic:  rdmap79s0, fabric: efa, link: 100Gbps
Run client with the following command:
  ./build/16_write_cpu fe8000000000000008e7effffeeee81d000000003e88df080000000000000000 [page_size num_pages]
Registered 1 buffer in CPU memory
------
Received CONNECT message from client:
  addr: fe80000000000000083425fffe7d535100000000057558100000000000000000
  MR[0]: addr=0x7f2440800000 size=16777216 rkey=0x000000000070000a
  MR[1]: addr=0x7f2442400000 size=16777216 rkey=0x0000000000a00031
Received RandomFill request from client:
  remote_context: 0x00000123
  seed: 0xb584035fabe6ce9b
  page_size: 1048576
  num_pages: 8
Generating random data..
Finished RDMA WRITE to the remote CPU memory.
------
^C

client$ ./build/16_write_cpu fe8000000000000008e7effffeeee81d000000003e88df080000000000000000
domain:  rdmap79s0-rdm, nic:  rdmap79s0, fabric: efa, link: 100Gbps
Registered 2 buffers in CPU memory
Sent CONNECT message to server
Sent RandomFillRequest to server. page_size: 1048576, num_pages: 8
Received RDMA WRITE to local CPU memory.
Data is correct
*/
// clang-format on

#include <algorithm>
#include <functional>
#include <inttypes.h>
#include <memory>
#include <netdb.h>
#include <pthread.h>
#include <random>
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <time.h>
#include <unistd.h>
#include <vector>

#define CHECK(stmt)                                                            \
  do {                                                                         \
    if (!(stmt)) {                                                             \
      fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #stmt);                \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

#define FI_CHECK(stmt)                                                         \
  do {                                                                         \
    int rc = (stmt);                                                           \
    if (rc) {                                                                  \
      fprintf(stderr, "%s:%d %s failed with %d (%s)\n", __FILE__, __LINE__,    \
              #stmt, rc, fi_strerror(-rc));                                    \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

constexpr size_t kBufAlign = 128; // EFA alignment requirement
constexpr size_t kMessageBufferSize = 8192;
constexpr size_t kCompletionQueueReadCount = 16;
constexpr size_t kMemoryRegionSize = 16 << 20;
constexpr size_t kEfaImmDataSize = 4;

// EFA page size optimization to avoid scatter-gather issues
size_t OptimizePageSizeForEFA(size_t requested_size) {
  // Known problematic: exact 1MB (1048576) causes "Remote scatter-gather list too short"
  // Known working: 1047552 (1MB - 1024)
  if (requested_size == 1048576) {
    printf("INFO: Converting exact 1MB page size to 1047552 to avoid EFA scatter-gather issues\n");
    return 1047552;
  }
  
  // For other power-of-2 sizes >= 512KB, subtract small amount to avoid EFA issues
  if (requested_size >= 524288 && (requested_size & (requested_size - 1)) == 0) {
    size_t adjusted = requested_size - 1024;
    printf("INFO: Adjusting power-of-2 page size from %zu to %zu to avoid EFA issues\n", requested_size, adjusted);
    return adjusted;
  }
  
  return requested_size;
}

struct Buffer;
struct Network;

struct EfaAddress {
  uint8_t bytes[32];

  explicit EfaAddress(uint8_t bytes[32]) { memcpy(this->bytes, bytes, 32); }

  std::string ToString() const {
    char buf[65];
    for (size_t i = 0; i < 32; i++) {
      snprintf(buf + 2 * i, 3, "%02x", bytes[i]);
    }
    return std::string(buf, 64);
  }

  static EfaAddress Parse(const std::string &str) {
    if (str.size() != 64) {
      fprintf(stderr, "Unexpected address length %zu\n", str.size());
      std::exit(1);
    }
    uint8_t bytes[32];
    for (size_t i = 0; i < 32; i++) {
      sscanf(str.c_str() + 2 * i, "%02hhx", &bytes[i]);
    }
    return EfaAddress(bytes);
  }
};

enum class RdmaOpType : uint8_t {
  kRecv = 0,
  kSend = 1,
  kWrite = 2,
  kRemoteWrite = 3,
};

struct RdmaRecvOp {
  Buffer *buf;
  fi_addr_t src_addr; // Set after completion
  size_t recv_size;   // Set after completion
};
static_assert(std::is_pod_v<RdmaRecvOp>);

struct RdmaSendOp {
  Buffer *buf;
  size_t len;
  fi_addr_t dest_addr;
};
static_assert(std::is_pod_v<RdmaSendOp>);

struct RdmaWriteOp {
  Buffer *buf;
  size_t offset;
  size_t len;
  uint32_t imm_data;
  uint64_t dest_ptr;
  fi_addr_t dest_addr;
  uint64_t dest_key;
};
static_assert(std::is_pod_v<RdmaWriteOp>);

struct RdmaRemoteWriteOp {
  uint32_t op_id;
};
static_assert(std::is_pod_v<RdmaRemoteWriteOp>);
static_assert(sizeof(RdmaRemoteWriteOp) <= kEfaImmDataSize);

struct RdmaOp {
  RdmaOpType type;
  union {
    RdmaRecvOp recv;
    RdmaSendOp send;
    RdmaWriteOp write;
    RdmaRemoteWriteOp remote_write;
  };
  std::function<void(Network &, RdmaOp &)> callback;
};

struct Network {
  struct fi_info *fi;
  struct fid_fabric *fabric;
  struct fid_domain *domain;
  struct fid_cq *cq;
  struct fid_av *av;
  struct fid_ep *ep;
  EfaAddress addr;

  std::unordered_map<void *, struct fid_mr *> mr;
  std::unordered_map<uint32_t, RdmaOp *> remote_write_ops;

  static Network Open(struct fi_info *fi);

  fi_addr_t AddPeerAddress(const EfaAddress &peer_addr);
  void RegisterMemory(Buffer &buf);
  struct fid_mr *GetMR(const Buffer &buf);

  void PollCompletion();
  void PostRecv(Buffer &buf,
                std::function<void(Network &, RdmaOp &)> &&callback);
  void PostSend(fi_addr_t addr, Buffer &buf, size_t len,
                std::function<void(Network &, RdmaOp &)> &&callback);
  void PostWrite(RdmaWriteOp &&write,
                 std::function<void(Network &, RdmaOp &)> &&callback);
  void AddRemoteWrite(uint32_t id,
                      std::function<void(Network &, RdmaOp &)> &&callback);

  Network(const Network &) = delete;
  Network(Network &&other)
      : fi(other.fi), fabric(other.fabric), domain(other.domain), cq(other.cq),
        av(other.av), ep(other.ep), addr(other.addr) {
    other.fi = nullptr;
    other.fabric = nullptr;
    other.domain = nullptr;
    other.cq = nullptr;
    other.av = nullptr;
    other.ep = nullptr;
  }

  ~Network() {
    for (const auto &[_, mr] : mr) {
      FI_CHECK(fi_close(&mr->fid));
    }
    if (ep)
      FI_CHECK(fi_close(&ep->fid));
    if (av)
      FI_CHECK(fi_close(&av->fid));
    if (cq)
      FI_CHECK(fi_close(&cq->fid));
    if (domain)
      FI_CHECK(fi_close(&domain->fid));
    if (fabric)
      FI_CHECK(fi_close(&fabric->fid));
  }

private:
  Network(struct fi_info *fi, struct fid_fabric *fabric,
          struct fid_domain *domain, struct fid_cq *cq, struct fid_av *av,
          struct fid_ep *ep, EfaAddress addr)
      : fi(fi), fabric(fabric), domain(domain), cq(cq), av(av), ep(ep),
        addr(addr) {}
};
void *align_up(void *ptr, size_t align) {
  uintptr_t addr = (uintptr_t)ptr;
  return (void *)((addr + align - 1) & ~(align - 1));
}

struct Buffer {
  void *data;
  size_t size;

  static Buffer Alloc(size_t size, size_t align) {
    void *raw_data = malloc(size + align);
    CHECK(raw_data != nullptr);
    return Buffer(raw_data, size, align);
  }

  Buffer(Buffer &&other)
      : data(other.data), size(other.size), raw_data(other.raw_data) {
    other.data = nullptr;
    other.raw_data = nullptr;
    other.size = 0;
  }

  ~Buffer() {
    if (raw_data) {
      free(raw_data);
    }
  }

private:
  void *raw_data;

  Buffer(void *raw_data, size_t raw_size, size_t align) {
    this->raw_data = raw_data;
    this->data = align_up(raw_data, align);
    this->size = (size_t)((uintptr_t)raw_data + raw_size - (uintptr_t)data);
  }
  Buffer(const Buffer &) = delete;
};

struct fi_info *GetInfo() {
  struct fi_info *hints, *info;
  hints = fi_allocinfo();
  hints->caps = FI_MSG | FI_RMA | FI_LOCAL_COMM | FI_REMOTE_COMM;
  hints->ep_attr->type = FI_EP_RDM;
  hints->fabric_attr->prov_name = strdup("efa");
  hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR |
                                FI_MR_ALLOCATED | FI_MR_PROV_KEY;
  hints->domain_attr->threading = FI_THREAD_SAFE;
  FI_CHECK(fi_getinfo(FI_VERSION(2, 0), nullptr, nullptr, 0, hints, &info));
  fi_freeinfo(hints);
  return info;
}

Network Network::Open(struct fi_info *fi) {
  struct fid_fabric *fabric;
  FI_CHECK(fi_fabric(fi->fabric_attr, &fabric, nullptr));

  struct fid_domain *domain;
  FI_CHECK(fi_domain(fabric, fi, &domain, nullptr));

  struct fid_cq *cq;
  struct fi_cq_attr cq_attr = {};
  cq_attr.format = FI_CQ_FORMAT_DATA;
  FI_CHECK(fi_cq_open(domain, &cq_attr, &cq, nullptr));

  struct fid_av *av;
  struct fi_av_attr av_attr = {};
  FI_CHECK(fi_av_open(domain, &av_attr, &av, nullptr));

  struct fid_ep *ep;
  FI_CHECK(fi_endpoint(domain, fi, &ep, nullptr));
  FI_CHECK(fi_ep_bind(ep, &cq->fid, FI_SEND | FI_RECV));
  FI_CHECK(fi_ep_bind(ep, &av->fid, 0));

  FI_CHECK(fi_enable(ep));

  uint8_t addr[64];
  size_t addrlen = sizeof(addr);
  FI_CHECK(fi_getname(&ep->fid, addr, &addrlen));
  if (addrlen != 32) {
    fprintf(stderr, "Unexpected address length %zu\n", addrlen);
    std::exit(1);
  }

  return Network(fi, fabric, domain, cq, av, ep, EfaAddress(addr));
}
fi_addr_t Network::AddPeerAddress(const EfaAddress &peer_addr) {
  fi_addr_t addr = FI_ADDR_UNSPEC;
  int ret = fi_av_insert(av, peer_addr.bytes, 1, &addr, 0, nullptr);
  if (ret != 1) {
    fprintf(stderr, "fi_av_insert failed: %d\n", ret);
    std::exit(1);
  }
  return addr;
}

void Network::RegisterMemory(Buffer &buf) {
  struct fid_mr *mr;
  struct fi_mr_attr mr_attr = {
      .iov_count = 1,
      .access = FI_SEND | FI_RECV | FI_REMOTE_WRITE | FI_REMOTE_READ |
                FI_WRITE | FI_READ,
  };
  struct iovec iov = {.iov_base = buf.data, .iov_len = buf.size};
  mr_attr.mr_iov = &iov;
  
  FI_CHECK(fi_mr_regattr(domain, &mr_attr, 0, &mr));
  this->mr[buf.data] = mr;
}

struct fid_mr *Network::GetMR(const Buffer &buf) {
  auto it = mr.find(buf.data);
  CHECK(it != mr.end());
  return it->second;
}

void Network::PostRecv(Buffer &buf,
                       std::function<void(Network &, RdmaOp &)> &&callback) {
  auto *op = new RdmaOp{
      .type = RdmaOpType::kRecv,
      .recv =
          RdmaRecvOp{.buf = &buf, .src_addr = FI_ADDR_UNSPEC, .recv_size = 0},
      .callback = std::move(callback),
  };
  struct iovec iov = {
      .iov_base = buf.data,
      .iov_len = buf.size,
  };
  struct fi_msg msg = {
      .msg_iov = &iov,
      .desc = &GetMR(buf)->mem_desc,
      .iov_count = 1,
      .addr = FI_ADDR_UNSPEC,
      .context = op,
  };
  
  // Handle EAGAIN by retrying with backoff
  int ret;
  int retry_count = 0;
  const int max_retries = 10;
  const int retry_delay_us = 1000; // 1ms initial delay
  
  while ((ret = fi_recvmsg(ep, &msg, 0)) == -FI_EAGAIN) {
    if (++retry_count > max_retries) {
      fprintf(stderr, "%s:%d fi_recvmsg(ep, &msg, 0) failed with %d (%s) after %d retries\n", 
              __FILE__, __LINE__, ret, fi_strerror(-ret), retry_count);
      std::exit(1);
    }
    // Exponential backoff
    usleep(retry_delay_us * (1 << (retry_count - 1)));
    // Poll completions to free up resources
    PollCompletion();
  }
  
  if (ret) {
    fprintf(stderr, "%s:%d fi_recvmsg(ep, &msg, 0) failed with %d (%s)\n", 
            __FILE__, __LINE__, ret, fi_strerror(-ret));
    std::exit(1);
  }
}

void Network::PostSend(fi_addr_t addr, Buffer &buf, size_t len,
                       std::function<void(Network &, RdmaOp &)> &&callback) {
  CHECK(len <= buf.size);
  auto *op = new RdmaOp{
      .type = RdmaOpType::kSend,
      .send = RdmaSendOp{.buf = &buf, .len = len, .dest_addr = addr},
      .callback = std::move(callback),
  };
  struct iovec iov = {
      .iov_base = buf.data,
      .iov_len = len,
  };
  struct fi_msg msg = {
      .msg_iov = &iov,
      .desc = &GetMR(buf)->mem_desc,
      .iov_count = 1,
      .addr = addr,
      .context = op,
  };
  
  // Handle EAGAIN by retrying with backoff
  int ret;
  int retry_count = 0;
  const int max_retries = 10;
  const int retry_delay_us = 1000; // 1ms initial delay
  
  while ((ret = fi_sendmsg(ep, &msg, 0)) == -FI_EAGAIN) {
    if (++retry_count > max_retries) {
      fprintf(stderr, "%s:%d fi_sendmsg(ep, &msg, 0) failed with %d (%s) after %d retries\n", 
              __FILE__, __LINE__, ret, fi_strerror(-ret), retry_count);
      std::exit(1);
    }
    // Exponential backoff
    usleep(retry_delay_us * (1 << (retry_count - 1)));
    // Poll completions to free up resources
    PollCompletion();
  }
  
  if (ret) {
    fprintf(stderr, "%s:%d fi_sendmsg(ep, &msg, 0) failed with %d (%s)\n", 
            __FILE__, __LINE__, ret, fi_strerror(-ret));
    std::exit(1);
  }
}
void Network::PostWrite(RdmaWriteOp &&write,
                        std::function<void(Network &, RdmaOp &)> &&callback) {
  auto *op = new RdmaOp{
      .type = RdmaOpType::kWrite,
      .write = std::move(write),
      .callback = std::move(callback),
  };
  struct iovec iov = {
      .iov_base = (uint8_t *)write.buf->data + write.offset,
      .iov_len = write.len,
  };
  struct fi_rma_iov rma_iov = {
      .addr = write.dest_ptr,
      .len = write.len,
      .key = write.dest_key,
  };
  struct fi_msg_rma msg = {
      .msg_iov = &iov,
      .desc = &GetMR(*write.buf)->mem_desc,
      .iov_count = 1,
      .addr = write.dest_addr,
      .rma_iov = &rma_iov,
      .rma_iov_count = 1,
      .context = op,
      .data = write.imm_data,
  };
  uint64_t flags = 0;
  if (write.imm_data) {
    flags |= FI_REMOTE_CQ_DATA;
  }
  
  // Handle EAGAIN by retrying with backoff
  int ret;
  int retry_count = 0;
  const int max_retries = 10;
  const int retry_delay_us = 1000; // 1ms initial delay
  
  while ((ret = fi_writemsg(ep, &msg, flags)) == -FI_EAGAIN) {
    if (++retry_count > max_retries) {
      fprintf(stderr, "%s:%d fi_writemsg(ep, &msg, flags) failed with %d (%s) after %d retries\n", 
              __FILE__, __LINE__, ret, fi_strerror(-ret), retry_count);
      std::exit(1);
    }
    // Exponential backoff
    usleep(retry_delay_us * (1 << (retry_count - 1)));
    // Poll completions to free up resources
    PollCompletion();
  }
  
  if (ret) {
    fprintf(stderr, "%s:%d fi_writemsg(ep, &msg, flags) failed with %d (%s)\n", 
            __FILE__, __LINE__, ret, fi_strerror(-ret));
    std::exit(1);
  }
}

void Network::AddRemoteWrite(
    uint32_t id, std::function<void(Network &, RdmaOp &)> &&callback) {
  CHECK(remote_write_ops.count(id) == 0);
  auto *op = new RdmaOp{
      .type = RdmaOpType::kRemoteWrite,
      .remote_write = RdmaRemoteWriteOp{.op_id = id},
      .callback = std::move(callback),
  };
  remote_write_ops[id] = op;
}

void HandleCompletion(Network &net, const struct fi_cq_data_entry &cqe) {
  RdmaOp *op = nullptr;
  if (cqe.flags & FI_REMOTE_WRITE) {
    // REMOTE WRITE does not have op_context
    // NOTE(lequn): EFA only supports 4 bytes of immediate data.
    uint32_t op_id = cqe.data;
    if (!op_id)
      return;
    auto it = net.remote_write_ops.find(op_id);
    if (it == net.remote_write_ops.end())
      return;
    op = it->second;
    net.remote_write_ops.erase(it);
  } else {
    // RECV / SEND / WRITE
    op = (RdmaOp *)cqe.op_context;
    if (!op)
      return;
    if (cqe.flags & FI_RECV) {
      op->recv.recv_size = cqe.len;
    } else if (cqe.flags & FI_SEND) {
      // Nothing special
    } else if (cqe.flags & FI_WRITE) {
      // Nothing special
    } else {
      fprintf(stderr, "Unhandled completion type. cqe.flags=%lx\n", cqe.flags);
      std::exit(1);
    }
  }
  if (op->callback)
    op->callback(net, *op);
  delete op;
}

void Network::PollCompletion() {
  struct fi_cq_data_entry cqe[kCompletionQueueReadCount];
  for (;;) {
    auto ret = fi_cq_read(cq, cqe, kCompletionQueueReadCount);
    if (ret > 0) {
      for (ssize_t i = 0; i < ret; i++) {
        HandleCompletion(*this, cqe[i]);
      }
    } else if (ret == -FI_EAVAIL) {
      struct fi_cq_err_entry err_entry;
      ret = fi_cq_readerr(cq, &err_entry, 0);
      if (ret < 0) {
        fprintf(stderr, "fi_cq_readerr error: %zd (%s)\n", ret,
                fi_strerror(-ret));
        std::exit(1);
      } else if (ret > 0) {
        fprintf(stderr, "Failed libfabric operation: %s\n",
                fi_cq_strerror(cq, err_entry.prov_errno, err_entry.err_data,
                               nullptr, 0));
      } else {
        fprintf(stderr, "fi_cq_readerr returned 0 unexpectedly.\n");
        std::exit(1);
      }
    } else if (ret == -FI_EAGAIN) {
      // No more completions
      break;
    } else {
      fprintf(stderr, "fi_cq_read error: %zd (%s)\n", ret, fi_strerror(-ret));
      std::exit(1);
    }
  }
}
enum class AppMessageType : uint8_t {
  kConnect = 0,
  kRandomFill = 1,
};

struct AppMessageBase {
  AppMessageType type;
};

struct AppConnectMessage {
  struct MemoryRegion {
    uint64_t addr;
    uint64_t size;
    uint64_t rkey;
  };

  AppMessageBase base;
  EfaAddress client_addr;
  size_t num_mr;

  MemoryRegion &mr(size_t index) {
    CHECK(index < num_mr);
    return ((MemoryRegion *)((uintptr_t)&base + sizeof(*this)))[index];
  }

  size_t MessageBytes() const {
    return sizeof(*this) + num_mr * sizeof(MemoryRegion);
  }
};

struct AppRandomFillMessage {
  AppMessageBase base;
  uint32_t remote_context;
  uint64_t seed;
  size_t page_size;
  size_t num_pages;

  uint32_t &page_idx(size_t index) {
    CHECK(index < num_pages);
    return ((uint32_t *)((uintptr_t)&base + sizeof(*this)))[index];
  }

  size_t MessageBytes() const {
    return sizeof(*this) + num_pages * sizeof(uint32_t);
  }
};

std::vector<uint8_t> RandomBytes(uint64_t seed, size_t size) {
  CHECK(size % sizeof(uint64_t) == 0);
  std::vector<uint8_t> buf(size);
  std::mt19937_64 gen(seed);
  std::uniform_int_distribution<uint64_t> dist;
  for (size_t i = 0; i < size; i += sizeof(uint64_t)) {
    *(uint64_t *)(buf.data() + i) = dist(gen);
  }
  return buf;
}
struct RandomFillRequestState {
  Buffer *cpu_buf1;
  Buffer *cpu_buf2;
  fi_addr_t client_addr = FI_ADDR_UNSPEC;
  bool done = false;
  AppConnectMessage *connect_msg = nullptr;

  explicit RandomFillRequestState(Buffer *cpu_buf1, Buffer *cpu_buf2) 
    : cpu_buf1(cpu_buf1), cpu_buf2(cpu_buf2) {}

  void HandleConnect(Network &net, RdmaOp &op) {
    auto *base_msg = (AppMessageBase *)op.recv.buf->data;
    CHECK(base_msg->type == AppMessageType::kConnect);
    CHECK(op.recv.recv_size >= sizeof(AppConnectMessage));
    auto &msg = *(AppConnectMessage *)base_msg;
    CHECK(op.recv.recv_size == msg.MessageBytes());
    CHECK(msg.num_mr > 0);

    // Save the message. Note that we don't reuse the buffer.
    connect_msg = &msg;

    // Add the client to AV
    client_addr = net.AddPeerAddress(msg.client_addr);

    printf("Received CONNECT message from client:\n");
    printf("  addr: %s\n", msg.client_addr.ToString().c_str());
    for (size_t i = 0; i < msg.num_mr; i++) {
      printf("  MR[%zu]: addr=0x%012lx size=%lu rkey=0x%016lx\n", i,
             msg.mr(i).addr, msg.mr(i).size, msg.mr(i).rkey);
    }
  }

  void HandleRequest(Network &net, RdmaOp &op) {
    auto *base_msg = (const AppMessageBase *)op.recv.buf->data;
    CHECK(base_msg->type == AppMessageType::kRandomFill);
    CHECK(op.recv.recv_size >= sizeof(AppRandomFillMessage));
    auto &msg = *(AppRandomFillMessage *)base_msg;
    CHECK(op.recv.recv_size == msg.MessageBytes());

    printf("Received RandomFill request from client:\n");
    printf("  remote_context: 0x%08x\n", msg.remote_context);
    printf("  seed: 0x%016lx\n", msg.seed);
    printf("  page_size: %zu\n", msg.page_size);
    printf("  num_pages: %zu\n", msg.num_pages);

    // Generate random data and copy to local CPU memory
    printf("Generating random data");
    auto bytes1 = RandomBytes(msg.seed, msg.page_size * msg.num_pages);
    memcpy(cpu_buf1->data, bytes1.data(), bytes1.size());
    printf(".");
    fflush(stdout);
    
    auto bytes2 = RandomBytes(msg.seed + 1, msg.page_size * msg.num_pages);
    memcpy(cpu_buf2->data, bytes2.data(), bytes2.size());
    printf(".");
    fflush(stdout);
    printf("Random Data Generated\n");

    // RDMA WRITE the data to remote CPU memory.
    //
    // NOTE(lequn): iov_limit==4, rma_iov_limit==1.
    // So need multiple WRITE instead of a vectorized WRITE.
    
    // DEBUG: Print all page indices first
    printf("DEBUG: Page indices for %zu pages:\n", msg.num_pages);
    for (size_t j = 0; j < msg.num_pages; j++) {
      printf("  page_idx[%zu] = %u\n", j, msg.page_idx(j));
    }
    printf("DEBUG: MR info:\n");
    for (size_t i = 0; i < connect_msg->num_mr; ++i) {
      printf("  MR[%zu]: addr=0x%012lx size=%lu (max_page_idx=%lu)\n", 
             i, connect_msg->mr(i).addr, connect_msg->mr(i).size,
             (connect_msg->mr(i).size / msg.page_size) - 1);
    }
    
    for (size_t i = 0; i < connect_msg->num_mr; ++i) {
      Buffer *local_buf = (i == 0) ? cpu_buf1 : cpu_buf2;
      printf("DEBUG: Processing MR[%zu] with %s\n", i, (i == 0) ? "cpu_buf1" : "cpu_buf2");
      
      for (size_t j = 0; j < msg.num_pages; j++) {
        uint32_t page_idx = msg.page_idx(j);
        uint64_t remote_offset = page_idx * msg.page_size;
        uint64_t dest_addr = connect_msg->mr(i).addr + remote_offset;
        
        // DEBUG: Check boundaries
        printf("DEBUG: MR[%zu] Write[%zu]: page_idx=%u, remote_offset=%lu, dest_addr=0x%lx\n",
               i, j, page_idx, remote_offset, dest_addr);
        
        if (remote_offset + msg.page_size > connect_msg->mr(i).size) {
          printf("ERROR: Write would exceed MR[%zu] boundary!\n", i);
          printf("  remote_offset(%lu) + page_size(%zu) = %lu > mr_size(%lu)\n",
                 remote_offset, msg.page_size, remote_offset + msg.page_size, 
                 connect_msg->mr(i).size);
          std::exit(1);
        }
        
        uint32_t imm_data = 0;
        std::function<void(Network &, RdmaOp &)> callback;
        if (i + 1 == connect_msg->num_mr && j + 1 == msg.num_pages) {
          // The last WRITE.
          // NOTE(lequn): EFA RDM guarantees send-after-send ordering.
          imm_data = msg.remote_context;
          callback = [this](Network &net, RdmaOp &op) {
            CHECK(op.type == RdmaOpType::kWrite);
            done = true;
            printf("Finished RDMA WRITE to the remote CPU memory.\n");
          };
        } else {
          // Don't send immediate data. Don't wake up the remote side.
          // Also skip local callback.
        }
        
        printf("DEBUG: Posting RDMA write: local_offset=%zu, remote_addr=0x%lx, len=%zu\n",
               j * msg.page_size, dest_addr, msg.page_size);
               
        net.PostWrite(
            {.buf = local_buf,
             .offset = j * msg.page_size,
             .len = msg.page_size,
             .imm_data = imm_data,
             .dest_ptr = dest_addr,
             .dest_addr = client_addr,
             .dest_key = connect_msg->mr(i).rkey},
            std::move(callback));
      }
    }
  }

  void OnRecv(Network &net, RdmaOp &op) {
    if (client_addr == FI_ADDR_UNSPEC) {
      HandleConnect(net, op);
    } else {
      HandleRequest(net, op);
    }
  }
};
int ServerMain(int argc, char **argv) {
  // Open Netowrk
  struct fi_info *info = GetInfo();
  auto net = Network::Open(info);
  printf("domain: %14s", info->domain_attr->name);
  printf(", nic: %10s", info->nic->device_attr->name);
  printf(", fabric: %s", info->fabric_attr->prov_name);
  printf(", link: %.0fGbps", info->nic->link_attr->speed / 1e9);
  printf("\n");
  printf("Run client with the following command:\n");
  printf("  %s %s [page_size num_pages]\n", argv[0],
         net.addr.ToString().c_str());

  // Allocate and register message buffer
  auto buf1 = Buffer::Alloc(kMessageBufferSize, kBufAlign);
  net.RegisterMemory(buf1);
  auto buf2 = Buffer::Alloc(kMessageBufferSize, kBufAlign);
  net.RegisterMemory(buf2);

  // Allocate and register CPU memory
  auto cpu_buf1 = Buffer::Alloc(kMemoryRegionSize, kBufAlign);
  net.RegisterMemory(cpu_buf1);
  auto cpu_buf2 = Buffer::Alloc(kMemoryRegionSize, kBufAlign);
  net.RegisterMemory(cpu_buf2);
  printf("Registered 2 buffers in CPU memory\n");

  // Loop forever. Accept one client at a time.
  for (;;) {
    printf("------\n");
    // State machine
    RandomFillRequestState s(&cpu_buf1, &cpu_buf2);
    // RECV for CONNECT
    net.PostRecv(buf1, [&s](Network &net, RdmaOp &op) { s.OnRecv(net, op); });
    // RECV for RandomFillRequest
    net.PostRecv(buf2, [&s](Network &net, RdmaOp &op) { s.OnRecv(net, op); });
    // Wait for completion
    while (!s.done) {
      net.PollCompletion();
    }
  }

  fi_freeinfo(info);
  return 0;
}

int ClientMain(int argc, char **argv) {
  CHECK(argc == 2 || argc == 4);
  auto server_addrname = EfaAddress::Parse(argv[1]);
  size_t page_size, num_pages;
  if (argc == 4) {
    page_size = std::stoull(argv[2]);
    num_pages = std::stoull(argv[3]);
  } else {
    page_size = 1 << 20;
    num_pages = 8;
  }
  
  // Apply EFA optimization to avoid scatter-gather issues
  size_t original_page_size = page_size;
  page_size = OptimizePageSizeForEFA(page_size);
  if (page_size != original_page_size) {
    printf("DEBUG: Page size optimized from %zu to %zu for EFA compatibility\n", 
           original_page_size, page_size);
  }
  
  size_t max_pages = kMemoryRegionSize / page_size;
  CHECK(page_size * num_pages <= kMemoryRegionSize);

  // Open Netowrk
  struct fi_info *info = GetInfo();
  
  auto net = Network::Open(info);
  printf("domain: %14s", info->domain_attr->name);
  printf(", nic: %10s", info->nic->device_attr->name);
  printf(", fabric: %s", info->fabric_attr->prov_name);
  printf(", link: %.0fGbps", info->nic->link_attr->speed / 1e9);
  printf("\n");
  auto server_addr = net.AddPeerAddress(server_addrname);

  // Allocate and register message buffer
  auto buf1 = Buffer::Alloc(kMessageBufferSize, kBufAlign);
  net.RegisterMemory(buf1);

  // Allocate and register CPU memory
  auto cpu_buf1 = Buffer::Alloc(kMemoryRegionSize, kBufAlign);
  net.RegisterMemory(cpu_buf1);
  auto cpu_buf2 = Buffer::Alloc(kMemoryRegionSize, kBufAlign);
  net.RegisterMemory(cpu_buf2);
  printf("Registered 2 buffers in CPU memory\n");

  // Prepare request
  std::mt19937_64 rng(0xabcdabcd987UL);
  uint64_t req_seed = rng();
  std::vector<uint32_t> page_idx;
  std::vector<uint32_t> tmp(max_pages);
  std::iota(tmp.begin(), tmp.end(), 0);
  std::sample(tmp.begin(), tmp.end(), std::back_inserter(page_idx), num_pages,
              rng);

  // DEBUG: Print client-side configuration
  printf("DEBUG CLIENT: Configuration:\n");
  printf("  page_size: %zu (%zuMB)\n", page_size, page_size / (1024*1024));
  printf("  num_pages: %zu\n", num_pages);
  printf("  max_pages: %zu\n", max_pages);
  printf("  kMemoryRegionSize: %zu (%zuMB)\n", kMemoryRegionSize, kMemoryRegionSize / (1024*1024));
  printf("DEBUG CLIENT: Generated page indices:\n");
  for (size_t i = 0; i < page_idx.size(); i++) {
    printf("  page_idx[%zu] = %u (offset = %u * %zu = %zu)\n", 
           i, page_idx[i], page_idx[i], page_size, page_idx[i] * page_size);
  }

  // Send address and MR to server
  auto &connect_msg = *(AppConnectMessage *)buf1.data;
  connect_msg = {
      .base = {.type = AppMessageType::kConnect},
      .client_addr = net.addr,
      .num_mr = 2,
  };
  connect_msg.mr(0) = {.addr = (uint64_t)cpu_buf1.data,
                       .size = kMemoryRegionSize,
                       .rkey = net.GetMR(cpu_buf1)->key};
  connect_msg.mr(1) = {.addr = (uint64_t)cpu_buf2.data,
                       .size = kMemoryRegionSize,
                       .rkey = net.GetMR(cpu_buf2)->key};
  bool connect_sent = false;
  net.PostSend(
      server_addr, buf1, connect_msg.MessageBytes(),
      [&connect_sent](Network &net, RdmaOp &op) { connect_sent = true; });
  while (!connect_sent) {
    net.PollCompletion();
  }
  printf("Sent CONNECT message to server\n");

  // Prepare to receive the last REMOTE WRITE from server
  bool last_remote_write_received = false;
  uint32_t remote_write_op_id = 0x123;
  net.AddRemoteWrite(remote_write_op_id,
                     [&last_remote_write_received](Network &net, RdmaOp &op) {
                       last_remote_write_received = true;
                     });

  // Send message to server
  auto &req_msg = *(AppRandomFillMessage *)buf1.data;
  req_msg = {
      .base = {.type = AppMessageType::kRandomFill},
      .remote_context = remote_write_op_id,
      .seed = req_seed,
      .page_size = page_size,
      .num_pages = num_pages,
  };
  for (size_t i = 0; i < num_pages; i++) {
    req_msg.page_idx(i) = page_idx[i];
  }
  bool req_sent = false;
  net.PostSend(server_addr, buf1, req_msg.MessageBytes(),
               [&req_sent](Network &net, RdmaOp &op) { req_sent = true; });
  while (!req_sent) {
    net.PollCompletion();
  }
  printf("Sent RandomFillRequest to server. page_size: %zu, num_pages: %zu\n",
         page_size, num_pages);

  // Wait for REMOTE WRITE from server
  while (!last_remote_write_received) {
    net.PollCompletion();
  }
  printf("Received RDMA WRITE to local CPU memory.\n");

  // Verify data
  auto expected1 = RandomBytes(req_seed, page_size * num_pages);
  auto expected2 = RandomBytes(req_seed + 1, page_size * num_pages);
  auto actual1 = std::vector<uint8_t>(page_size * num_pages);
  auto actual2 = std::vector<uint8_t>(page_size * num_pages);
  for (size_t i = 0; i < num_pages; i++) {
    memcpy(actual1.data() + i * page_size,
           (uint8_t *)cpu_buf1.data + page_idx[i] * page_size,
           page_size);
    memcpy(actual2.data() + i * page_size,
           (uint8_t *)cpu_buf2.data + page_idx[i] * page_size,
           page_size);
  }
  CHECK(expected1 == actual1);
  CHECK(expected2 == actual2);
  printf("Data is correct\n");

  fi_freeinfo(info);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 1) {
    return ServerMain(argc, argv);
  } else {
    return ClientMain(argc, argv);
  }
}
