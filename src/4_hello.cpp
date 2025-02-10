// clang-format off
/*
Example run:

server$ ./build/4_hello
domain:  rdmap79s0-rdm, nic:  rdmap79s0, fabric: efa, link: 100Gbps
Run client with the following command:
  ./build/4_hello fe800000000000000853f7fffea442e1000000001826f6400000000000000000
  ./build/4_hello fe800000000000000853f7fffea442e1000000001826f6400000000000000000 "anytext"
------
Received message (len=13): Hello, world!
Received message (len=7): anytext
^C

client$ ./build/4_hello fe800000000000000853f7fffea442e1000000001826f6400000000000000000
domain:  rdmap79s0-rdm, nic:  rdmap79s0, fabric: efa, link: 100Gbps
Sent message (len=13): Hello, world!

client$ ./build/4_hello fe800000000000000853f7fffea442e1000000001826f6400000000000000000 "anytext"
domain:  rdmap79s0-rdm, nic:  rdmap79s0, fabric: efa, link: 100Gbps
Sent message (len=7): anytext
*/
// clang-format on

#include <functional>
#include <inttypes.h>
#include <memory>
#include <netdb.h>
#include <pthread.h>
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
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
};

struct RdmaRecvOp {
  Buffer *buf;
  fi_addr_t src_addr; // Set after completion
  size_t recv_size;   // Set after completion
};
static_assert(std::is_pod_v<RdmaRecvOp> == true);

struct RdmaSendOp {
  Buffer *buf;
  size_t len;
  fi_addr_t dest_addr;
};
static_assert(std::is_pod_v<RdmaSendOp> == true);

struct RdmaOp {
  RdmaOpType type;
  union {
    RdmaRecvOp recv;
    RdmaSendOp send;
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

  static Network Open(struct fi_info *fi);

  fi_addr_t AddPeerAddress(const EfaAddress &peer_addr);
  void RegisterMemory(Buffer &buf);
  struct fid_mr *GetMR(const Buffer &buf);

  void PollCompletion();
  void PostRecv(Buffer &buf,
                std::function<void(Network &, RdmaOp &)> &&callback);
  void PostSend(fi_addr_t addr, Buffer &buf, size_t len,
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
    void *raw_data = malloc(size);
    CHECK(raw_data != nullptr);
    return Buffer(raw_data, size, align);
  }

  Buffer(Buffer &&other)
      : data(other.data), size(other.size), raw_data(other.raw_data) {
    other.data = nullptr;
    other.raw_data = nullptr;
  }

  ~Buffer() { free(raw_data); }

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
  hints->ep_attr->type = FI_EP_RDM;
  hints->fabric_attr->prov_name = strdup("efa");
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
  struct fi_mr_attr mr_attr = {};
  struct iovec iov = {.iov_base = buf.data, .iov_len = buf.size};
  mr_attr.mr_iov = &iov;
  mr_attr.iov_count = 1;
  mr_attr.access = FI_SEND | FI_RECV;
  uint64_t flags = 0;
  FI_CHECK(fi_mr_regattr(domain, &mr_attr, flags, &mr));
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
  FI_CHECK(fi_recvmsg(ep, &msg, 0)); // TODO: handle EAGAIN
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
  FI_CHECK(fi_sendmsg(ep, &msg, 0)); // TODO: handle EAGAIN
}

void HandleCompletion(Network &net, const struct fi_cq_data_entry &cqe) {
  auto comp_flags = cqe.flags;
  auto op = (RdmaOp *)cqe.op_context;
  if (!op) {
    return;
  }
  if (comp_flags & FI_RECV) {
    op->recv.recv_size = cqe.len;
    if (op->callback)
      op->callback(net, *op);
  } else if (comp_flags & FI_SEND) {
    if (op->callback)
      op->callback(net, *op);
  } else {
    fprintf(stderr, "Unhandled completion type. comp_flags=%lx\n", comp_flags);
    std::exit(1);
  }
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

int ServerMain(int argc, char **argv) {
  struct fi_info *info = GetInfo();
  auto net = Network::Open(info);
  printf("domain: %14s", info->domain_attr->name);
  printf(", nic: %10s", info->nic->device_attr->name);
  printf(", fabric: %s", info->fabric_attr->prov_name);
  printf(", link: %.0fGbps", info->nic->link_attr->speed / 1e9);
  printf("\n");
  printf("Run client with the following command:\n");
  printf("  %s %s\n", argv[0], net.addr.ToString().c_str());
  printf("  %s %s \"anytext\"\n", argv[0], net.addr.ToString().c_str());
  printf("------\n");

  auto buf_msg = Buffer::Alloc(kMessageBufferSize, kBufAlign);
  net.RegisterMemory(buf_msg);
  net.PostRecv(buf_msg, [](Network &net, RdmaOp &op) {
    auto *msg = (const char *)op.recv.buf->data;
    auto len = op.recv.recv_size;
    printf("Received message (len=%zu): %.*s\n", len, (int)len, msg);
    net.PostRecv(*op.recv.buf, std::move(op.callback));
  });

  for (;;) {
    net.PollCompletion();
  }

  fi_freeinfo(info);
  return 0;
}

int ClientMain(int argc, char **argv) {
  CHECK(argc == 2 || argc == 3);
  auto server_addrname = EfaAddress::Parse(argv[1]);
  std::string message = argc == 3 ? argv[2] : "Hello, world!";

  struct fi_info *info = GetInfo();
  auto net = Network::Open(info);
  printf("domain: %14s", info->domain_attr->name);
  printf(", nic: %10s", info->nic->device_attr->name);
  printf(", fabric: %s", info->fabric_attr->prov_name);
  printf(", link: %.0fGbps", info->nic->link_attr->speed / 1e9);
  printf("\n");
  auto server_addr = net.AddPeerAddress(server_addrname);
  auto buf_msg = Buffer::Alloc(kMessageBufferSize, kBufAlign);
  net.RegisterMemory(buf_msg);
  memcpy(buf_msg.data, message.data(), message.size());

  bool sent = false;
  net.PostSend(server_addr, buf_msg, message.size(),
               [&sent](Network &net, RdmaOp &op) {
                 auto *msg = (const char *)op.send.buf->data;
                 auto len = op.send.len;
                 printf("Sent message (len=%zu): %.*s\n", len, (int)len, msg);
                 sent = true;
               });
  while (!sent) {
    net.PollCompletion();
  }

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
