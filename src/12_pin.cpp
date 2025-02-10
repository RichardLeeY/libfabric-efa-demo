// clang-format off
/*
Example run:

server$ ./build/12_pin
GPUs: 8, NICs: 32, Total Bandwidth: 3200 Gbps
PCIe Topology:
  cuda:0(53:00.0) NUMA0 CPU 0-11 rdmap79s0 (4f:00.0) rdmap80s0 (50:00.0) rdmap81s0 (51:00.0) rdmap82s0 (52:00.0)
  cuda:1(64:00.0) NUMA0 CPU12-23 rdmap96s0 (60:00.0) rdmap97s0 (61:00.0) rdmap98s0 (62:00.0) rdmap99s0 (63:00.0)
  cuda:2(75:00.0) NUMA0 CPU24-35 rdmap113s0(71:00.0) rdmap114s0(72:00.0) rdmap115s0(73:00.0) rdmap116s0(74:00.0)
  cuda:3(86:00.0) NUMA0 CPU36-47 rdmap130s0(82:00.0) rdmap131s0(83:00.0) rdmap132s0(84:00.0) rdmap133s0(85:00.0)
  cuda:4(97:00.0) NUMA1 CPU48-59 rdmap147s0(93:00.0) rdmap148s0(94:00.0) rdmap149s0(95:00.0) rdmap150s0(96:00.0)
  cuda:5(a8:00.0) NUMA1 CPU60-71 rdmap164s0(a4:00.0) rdmap165s0(a5:00.0) rdmap166s0(a6:00.0) rdmap167s0(a7:00.0)
  cuda:6(b9:00.0) NUMA1 CPU72-83 rdmap181s0(b5:00.0) rdmap182s0(b6:00.0) rdmap183s0(b7:00.0) rdmap184s0(b8:00.0)
  cuda:7(ca:00.0) NUMA1 CPU84-95 rdmap198s0(c6:00.0) rdmap199s0(c7:00.0) rdmap200s0(c8:00.0) rdmap201s0(c9:00.0)
Run client with the following command:
  ./build/12_pin 8 32 fe80000000000000088c03fffecfda9500000000fa6bac5a0000000000000000 [page_size num_pages]
Registered MR from cuda:0 cuda:1 cuda:2 cuda:3 cuda:4 cuda:5 cuda:6 cuda:7
------
Received CONNECT message from client: num_gpus=8, num_nets=32, num_mr=64
Received RandomFill request from client:
  remote_context: 0x00000123
  seed: 0xb584035fabe6ce9b
  page_size: 65536
  num_pages: 1000
  total_repeat: 2000
Generating random data................
Started RDMA WRITE to the remote GPU memory.
[13.555s] WRITE: 100%, ops=32000000/32000000, posted=32000000(100%), bytes=2097152000000/2097152000000, bw=1237.738Gbps(38.7%), 2.361Mpps
Finished all RDMA WRITEs to the remote GPU memory.
------
^C

client$ ./build/12_pin 8 32 fe80000000000000088c03fffecfda9500000000fa6bac5a0000000000000000
GPUs: 8, NICs: 32, Total Bandwidth: 3200 Gbps
PCIe Topology:
  cuda:0(53:00.0) NUMA0 CPU 0-11 rdmap79s0 (4f:00.0) rdmap80s0 (50:00.0) rdmap81s0 (51:00.0) rdmap82s0 (52:00.0)
  cuda:1(64:00.0) NUMA0 CPU12-23 rdmap96s0 (60:00.0) rdmap97s0 (61:00.0) rdmap98s0 (62:00.0) rdmap99s0 (63:00.0)
  cuda:2(75:00.0) NUMA0 CPU24-35 rdmap113s0(71:00.0) rdmap114s0(72:00.0) rdmap115s0(73:00.0) rdmap116s0(74:00.0)
  cuda:3(86:00.0) NUMA0 CPU36-47 rdmap130s0(82:00.0) rdmap131s0(83:00.0) rdmap132s0(84:00.0) rdmap133s0(85:00.0)
  cuda:4(97:00.0) NUMA1 CPU48-59 rdmap147s0(93:00.0) rdmap148s0(94:00.0) rdmap149s0(95:00.0) rdmap150s0(96:00.0)
  cuda:5(a8:00.0) NUMA1 CPU60-71 rdmap164s0(a4:00.0) rdmap165s0(a5:00.0) rdmap166s0(a6:00.0) rdmap167s0(a7:00.0)
  cuda:6(b9:00.0) NUMA1 CPU72-83 rdmap181s0(b5:00.0) rdmap182s0(b6:00.0) rdmap183s0(b7:00.0) rdmap184s0(b8:00.0)
  cuda:7(ca:00.0) NUMA1 CPU84-95 rdmap198s0(c6:00.0) rdmap199s0(c7:00.0) rdmap200s0(c8:00.0) rdmap201s0(c9:00.0)
Registered MR from cuda:0 cuda:1 cuda:2 cuda:3 cuda:4 cuda:5 cuda:6 cuda:7
Sent CONNECT message to server. SEND latency: 19562.466us
Sent RandomFillRequest to server. page_size: 65536, num_pages: 1000, SEND latency: 1218.370us
Received RDMA WRITE to local GPU memory.
Verifying................
Data is correct
*/
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cuda.h>
#include <cuda_runtime.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <immintrin.h>
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
#include <rdma/fi_ext.h>
#include <rdma/fi_rma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <thread>
#include <time.h>
#include <type_traits>
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

#define CUDA_CHECK(stmt)                                                       \
  do {                                                                         \
    cudaError_t rc = (stmt);                                                   \
    if (rc != cudaSuccess) {                                                   \
      fprintf(stderr, "%s:%d %s failed with %d (%s)\n", __FILE__, __LINE__,    \
              #stmt, rc, cudaGetErrorString(rc));                              \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

#define CU_CHECK(stmt)                                                         \
  do {                                                                         \
    CUresult rc = (stmt);                                                      \
    if (rc != CUDA_SUCCESS) {                                                  \
      const char *err_str;                                                     \
      cuGetErrorString(rc, &err_str);                                          \
      fprintf(stderr, "%s:%d %s failed with %d (%s)\n", __FILE__, __LINE__,    \
              #stmt, rc, err_str);                                             \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

constexpr size_t kBufAlign = 128; // EFA alignment requirement
constexpr size_t kMessageBufferSize = 1 << 20;
constexpr size_t kCompletionQueueReadCount = 16;
constexpr size_t kMemoryRegionSize = 1UL << 30;
constexpr size_t kEfaImmDataSize = 4;
constexpr size_t kMaxNetworksPerGroup = 4;

struct Buffer;
struct Network;

struct PciAddress {
  uint16_t domain : 16;
  uint8_t bus : 8;
  uint8_t device : 5;
  uint8_t function : 3;

  static PciAddress Parse(std::string_view addr) {
    CHECK(addr.size() == 12);
    uint16_t domain;
    uint8_t bus, device, function;
    CHECK(sscanf(addr.data(), "%hx:%hhx:%hhx.%hhx", &domain, &bus, &device,
                 &function) == 4);
    return PciAddress{domain, bus, device, function};
  }

  uint32_t AsU32() const { return *(uint32_t *)this; }

  friend bool operator==(const PciAddress &lhs, const PciAddress &rhs) {
    return lhs.AsU32() == rhs.AsU32();
  }
};
static_assert(sizeof(PciAddress) == 4);

namespace std {
template <> struct hash<PciAddress> {
  size_t operator()(const PciAddress &addr) const {
    return hash<uint32_t>()(addr.AsU32());
  }
};
} // namespace std

struct TopologyGroup {
  int cuda_device;
  int numa;
  std::vector<struct fi_info *> fi_infos;
  std::vector<int> cpus;
};

std::vector<TopologyGroup> DetectTopo(struct fi_info *info) {
  char buf[256];
  int num_gpus = 0;
  CUDA_CHECK(cudaGetDeviceCount(&num_gpus));
  std::vector<TopologyGroup> topo_groups(num_gpus);

  int num_cpus = 0;
  std::vector<std::vector<int>> numa_cpus;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(
           "/sys/devices/system/node/")) {
    if (entry.path().filename().string().rfind("node", 0) != 0) {
      continue;
    }
    numa_cpus.emplace_back();
  }
  int hardware_concurrency = std::thread::hardware_concurrency();
  for (size_t node_id = 0; node_id < numa_cpus.size(); ++node_id) {
    for (int cpu = 0; cpu < hardware_concurrency; ++cpu) {
      snprintf(buf, sizeof(buf),
               "/sys/devices/system/node/node%zu/cpu%d/"
               "topology/thread_siblings_list",
               node_id, cpu);
      // Filter out hyperthreads
      std::ifstream f(buf);
      std::string sibling_list;
      if (f >> sibling_list) {
        int first_sibling;
        try {
          first_sibling = std::stoi(sibling_list);
        } catch (std::invalid_argument &e) {
          continue;
        }
        if (first_sibling == cpu) {
          numa_cpus[node_id].push_back(cpu);
        }
      }
    }
    std::sort(numa_cpus[node_id].begin(), numa_cpus[node_id].end());
    num_cpus += numa_cpus[node_id].size();
  }
  int cpus_per_gpu = num_cpus / num_gpus;

  std::unordered_map<PciAddress, PciAddress> pci_parent_map;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator("/sys/bus/pci/devices")) {
    if (!entry.is_symlink()) {
      continue;
    }
    auto target = std::filesystem::read_symlink(entry.path());
    auto addr_str = target.filename().string();
    auto parent_addr_str = target.parent_path().filename().string();
    CHECK(addr_str.size() == 12);       // 0000:51:00.0
    if (parent_addr_str.size() != 12) { // 0000:46:01.2
      continue;                         // pci0000:cc
    }
    auto addr = PciAddress::Parse(addr_str);
    auto parent_bus = PciAddress::Parse(parent_addr_str);
    parent_bus.device = 0;
    parent_bus.function = 0;
    pci_parent_map[addr] = parent_bus;
  }

  std::vector<int> numa_gpu_count(numa_cpus.size());
  std::unordered_map<PciAddress, int> bus_cuda_map;
  for (int i = 0; i < num_gpus; ++i) {
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, i));
    auto pci_addr =
        PciAddress{(uint16_t)prop.pciDomainID, (uint8_t)prop.pciBusID,
                   (uint8_t)prop.pciDeviceID, 0};
    auto parent_bus = pci_parent_map.at(pci_addr);
    bus_cuda_map[parent_bus] = i;

    topo_groups[i].cuda_device = i;
    snprintf(buf, sizeof(buf),
             "/sys/bus/pci/devices/%04x:%02x:%02x.0/numa_node",
             prop.pciDomainID, prop.pciBusID, prop.pciDeviceID);
    std::ifstream f(buf);
    CHECK(f >> topo_groups[i].numa);
    int numa_gpu_idx = numa_gpu_count[topo_groups[i].numa]++;
    auto &cpus = numa_cpus[topo_groups[i].numa];
    int cpu_start = cpus_per_gpu * numa_gpu_idx;
    CHECK(cpu_start + cpus_per_gpu <= (int)cpus.size());
    topo_groups[i].cpus.assign(cpus.begin() + cpu_start,
                               cpus.begin() + cpu_start + cpus_per_gpu);
  }

  for (auto *fi = info; fi; fi = fi->next) {
    auto &pci = fi->nic->bus_attr->attr.pci;
    auto pci_addr =
        PciAddress{pci.domain_id, pci.bus_id, pci.device_id, pci.function_id};
    auto parent_bus = pci_parent_map.at(pci_addr);
    auto cuda_device = bus_cuda_map.at(parent_bus);
    topo_groups[cuda_device].fi_infos.push_back(fi);
  }

  return topo_groups;
}

void PrintTopologyGroups(const std::vector<TopologyGroup> &topo_groups) {
  printf("PCIe Topology:\n");
  for (const auto &topo : topo_groups) {
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, topo.cuda_device));
    printf("  cuda:%d(%02x:%02x.0)", topo.cuda_device, prop.pciBusID,
           prop.pciDeviceID);
    printf(" NUMA%d", topo.numa);
    printf(" CPU%2d-%2d", topo.cpus.front(), topo.cpus.back());
    for (auto *fi : topo.fi_infos) {
      printf(" %-10s(%02x:%02x.%d)", fi->nic->device_attr->name,
             fi->nic->bus_attr->attr.pci.bus_id,
             fi->nic->bus_attr->attr.pci.device_id,
             fi->nic->bus_attr->attr.pci.function_id);
    }
    printf("\n");
  }
}

void TrimTopo(std::vector<TopologyGroup> &groups, int num_gpus, int num_nets) {
  CHECK(num_gpus <= (int)groups.size());
  CHECK(num_nets % num_gpus == 0);
  int nets_per_gpu = num_nets / num_gpus;
  for (const auto &group : groups) {
    CHECK(nets_per_gpu <= (int)group.fi_infos.size());
  }
  while ((int)groups.size() > num_gpus) {
    groups.pop_back();
  }
  for (int i = 0; i < num_gpus; ++i) {
    while ((int)groups[i].fi_infos.size() > nets_per_gpu) {
      groups[i].fi_infos.pop_back();
    }
  }
}

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
  int cuda_device;

  std::deque<RdmaOp *> pending_ops;

  std::unordered_map<void *, struct fid_mr *> mr;
  std::unordered_map<uint32_t, RdmaOp *> remote_write_ops;

  static Network Open(struct fi_info *fi, int cuda_device);

  fi_addr_t AddPeerAddress(const EfaAddress &peer_addr);
  void RegisterMemory(Buffer &buf);
  struct fid_mr *GetMR(const Buffer &buf);

  void PollCompletion();
  void ProgressPendingOps();
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
        av(other.av), ep(other.ep), addr(other.addr),
        cuda_device(other.cuda_device) {
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
          struct fid_ep *ep, EfaAddress addr, int cuda_device)
      : fi(fi), fabric(fabric), domain(domain), cq(cq), av(av), ep(ep),
        addr(addr), cuda_device(cuda_device) {}
};

struct NetworkGroup {
  std::vector<Network *> nets;
  uint8_t rr_mask;
  uint8_t rr_idx = 0;

  NetworkGroup(std::vector<Network *> &&nets) {
    CHECK(nets.size() <= kMaxNetworksPerGroup);
    CHECK((nets.size() & (nets.size() - 1)) == 0); // power of 2
    this->rr_mask = nets.size() - 1;
    this->nets = std::move(nets);
  }
  NetworkGroup(const NetworkGroup &) = delete;
  NetworkGroup(NetworkGroup &&) = default;

  uint8_t GetNext() {
    rr_idx = (rr_idx + 1) & rr_mask;
    return rr_idx;
  }
};

void *align_up(void *ptr, size_t align) {
  uintptr_t addr = (uintptr_t)ptr;
  return (void *)((addr + align - 1) & ~(align - 1));
}

struct Buffer {
  void *data;
  size_t size;
  int cuda_device;
  int dmabuf_fd;

  static Buffer Alloc(size_t size, size_t align) {
    void *raw_data = malloc(size);
    CHECK(raw_data != nullptr);
    return Buffer(raw_data, size, align, -1, -1);
  }

  static Buffer AllocCuda(size_t size, size_t align) {
    void *raw_data;
    struct cudaPointerAttributes attrs = {};
    CUDA_CHECK(cudaMalloc(&raw_data, size));
    CUDA_CHECK(cudaPointerGetAttributes(&attrs, raw_data));
    CHECK(attrs.type == cudaMemoryTypeDevice);
    int cuda_device = attrs.device;
    int fd = -1;
    CU_CHECK(cuMemGetHandleForAddressRange(
        &fd, (CUdeviceptr)align_up(raw_data, align), size,
        CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0));
    return Buffer(raw_data, size, align, cuda_device, fd);
  }

  bool is_cuda() const { return cuda_device >= 0; }

  Buffer(Buffer &&other)
      : data(other.data), size(other.size), cuda_device(other.cuda_device),
        dmabuf_fd(other.dmabuf_fd), raw_data(other.raw_data) {
    other.data = nullptr;
    other.raw_data = nullptr;
    other.size = 0;
    other.cuda_device = -1;
    other.dmabuf_fd = -1;
  }

  ~Buffer() {
    if (is_cuda()) {
      CUDA_CHECK(cudaFree(raw_data));
    } else {
      free(raw_data);
    }
  }

private:
  void *raw_data;

  Buffer(void *raw_data, size_t raw_size, size_t align, int cuda_device,
         int dmabuf_fd) {
    this->raw_data = raw_data;
    this->data = align_up(raw_data, align);
    this->size = (size_t)((uintptr_t)raw_data + raw_size - (uintptr_t)data);
    this->cuda_device = cuda_device;
    this->dmabuf_fd = dmabuf_fd;
  }
  Buffer(const Buffer &) = delete;
};

struct fi_info *GetInfo() {
  struct fi_info *hints, *info;
  hints = fi_allocinfo();
  hints->caps = FI_MSG | FI_RMA | FI_HMEM | FI_LOCAL_COMM | FI_REMOTE_COMM;
  hints->ep_attr->type = FI_EP_RDM;
  hints->fabric_attr->prov_name = strdup("efa");
  hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_HMEM | FI_MR_VIRT_ADDR |
                                FI_MR_ALLOCATED | FI_MR_PROV_KEY;
  hints->domain_attr->threading = FI_THREAD_SAFE;
  FI_CHECK(fi_getinfo(FI_VERSION(2, 0), nullptr, nullptr, 0, hints, &info));
  fi_freeinfo(hints);
  return info;
}

Network Network::Open(struct fi_info *fi, int cuda_device) {
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

  uint8_t addrbuf[64];
  size_t addrlen = sizeof(addrbuf);
  FI_CHECK(fi_getname(&ep->fid, addrbuf, &addrlen));
  if (addrlen != 32) {
    fprintf(stderr, "Unexpected address length %zu\n", addrlen);
    std::exit(1);
  }
  auto addr = EfaAddress(addrbuf);

  return Network(fi, fabric, domain, cq, av, ep, addr, cuda_device);
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
  struct fi_mr_dmabuf dmabuf = {
      .fd = buf.dmabuf_fd, .offset = 0, .len = buf.size, .base_addr = buf.data};
  uint64_t flags = 0;
  if (buf.is_cuda()) {
    CHECK(buf.cuda_device == cuda_device);
    mr_attr.iface = FI_HMEM_CUDA;
    mr_attr.device.cuda = buf.cuda_device;
    if (buf.dmabuf_fd != -1) {
      mr_attr.dmabuf = &dmabuf;
      flags = FI_MR_DMABUF;
    } else {
      mr_attr.mr_iov = &iov;
    }
  } else {
    mr_attr.mr_iov = &iov;
  }
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
  pending_ops.push_back(op);
  ProgressPendingOps();
}

void Network::PostSend(fi_addr_t addr, Buffer &buf, size_t len,
                       std::function<void(Network &, RdmaOp &)> &&callback) {
  CHECK(len <= buf.size);
  auto *op = new RdmaOp{
      .type = RdmaOpType::kSend,
      .send = RdmaSendOp{.buf = &buf, .len = len, .dest_addr = addr},
      .callback = std::move(callback),
  };
  pending_ops.push_back(op);
  ProgressPendingOps();
}

void Network::PostWrite(RdmaWriteOp &&write,
                        std::function<void(Network &, RdmaOp &)> &&callback) {
  auto *op = new RdmaOp{
      .type = RdmaOpType::kWrite,
      .write = std::move(write),
      .callback = std::move(callback),
  };
  pending_ops.push_back(op);
  ProgressPendingOps();
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

void Network::ProgressPendingOps() {
  while (!pending_ops.empty()) {
    auto *op = pending_ops.front();
    pending_ops.pop_front();
    const char *op_name = nullptr;
    ssize_t ret = 0;
    switch (op->type) {
    case RdmaOpType::kRecv: {
      op_name = "fi_recv";
      auto &recv = op->recv;
      struct iovec iov = {
          .iov_base = recv.buf->data,
          .iov_len = recv.buf->size,
      };
      struct fi_msg msg = {
          .msg_iov = &iov,
          .desc = &GetMR(*recv.buf)->mem_desc,
          .iov_count = 1,
          .addr = FI_ADDR_UNSPEC,
          .context = op,
      };
      ret = fi_recvmsg(ep, &msg, 0);
      break;
    }
    case RdmaOpType::kSend: {
      op_name = "fi_send";
      auto &send = op->send;
      struct iovec iov = {
          .iov_base = send.buf->data,
          .iov_len = send.len,
      };
      struct fi_msg msg = {
          .msg_iov = &iov,
          .desc = &GetMR(*send.buf)->mem_desc,
          .iov_count = 1,
          .addr = send.dest_addr,
          .context = op,
      };
      ret = fi_sendmsg(ep, &msg, 0);
      break;
    }
    case RdmaOpType::kWrite: {
      op_name = "fi_writemsg";
      auto &write = op->write;
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
      ret = fi_writemsg(ep, &msg, flags);
      break;
    }
    case RdmaOpType::kRemoteWrite: {
      CHECK(false); // Unreachable
      break;
    }
    }
    if (ret == -FI_EAGAIN) {
      // Put it back to the front of the queue
      pending_ops.push_front(op);
      break;
    }
    if (ret) {
      // Unexpected error. Don't put it back.
      // Delete the op since it's not going to be in the completion queue.
      delete op;
      fprintf(stderr, "Failed to ProgressPendingOps. %s() returned %ld (%s)\n",
              op_name, ret, fi_strerror(-ret));
      fflush(stderr);
      break;
    }
  }
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
  // Process completions
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

  // Try to make progress.
  ProgressPendingOps();
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
  size_t num_gpus;
  size_t num_nets;
  size_t num_mr;

  EfaAddress &net_addr(size_t index) {
    CHECK(index < num_nets);
    return ((EfaAddress *)((uintptr_t)&base + sizeof(*this)))[index];
  }

  MemoryRegion &mr(size_t index) {
    CHECK(index < num_mr);
    return ((MemoryRegion *)((uintptr_t)&base + sizeof(*this) +
                             num_nets * sizeof(EfaAddress)))[index];
  }

  size_t MessageBytes() const {
    return sizeof(*this) + num_nets * sizeof(EfaAddress) +
           num_mr * sizeof(MemoryRegion);
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

long TimeDeltaNanos(
    const std::chrono::time_point<std::chrono::high_resolution_clock> &start,
    const std::chrono::time_point<std::chrono::high_resolution_clock> &end) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
      .count();
}

struct RandomFillRequestState {
  enum class State {
    kWaitRequest,
    kPostWarmup,
    kWaitWarmup,
    kWrite,
    kDone,
  };

  struct WriteState {
    bool warmup_posted = false;
    size_t i_repeat = 0;
    size_t i_buf = 0;
    size_t i_page = 0;
  };

  std::vector<Network> *nets;
  std::vector<NetworkGroup> *net_groups;
  std::vector<Buffer> *cuda_bufs;
  size_t total_bw = 0;
  std::atomic<State> state = State::kWaitRequest;

  AppConnectMessage *connect_msg = nullptr;
  AppRandomFillMessage *request_msg = nullptr;

  size_t total_repeat = 0;
  size_t nets_per_gpu = 0;
  size_t buf_per_gpu = 0;
  std::vector<std::array<fi_addr_t, kMaxNetworksPerGroup>> remote_addrs;
  std::vector<WriteState> write_states;
  std::atomic<size_t> posted_warmups = 0;
  std::atomic<size_t> cnt_warmups = 0;
  size_t total_write_ops_per_gpu = 0;
  size_t total_write_ops = 0;
  size_t write_op_size = 0;
  std::atomic<size_t> posted_write_ops = 0;
  std::atomic<size_t> finished_write_ops = 0;
  std::chrono::time_point<std::chrono::high_resolution_clock> write_start_at;

  RandomFillRequestState(std::vector<Network> *nets,
                         std::vector<NetworkGroup> *net_groups,
                         std::vector<Buffer> *cuda_bufs)
      : nets(nets), net_groups(net_groups), cuda_bufs(cuda_bufs) {
    for (auto &net : *nets) {
      total_bw += net.fi->nic->link_attr->speed;
    }
  }

  void OnRecv(Network &net, RdmaOp &op) {
    if (!connect_msg) {
      HandleConnect(net, op);
    } else {
      HandleRequest(net, op);
    }
  }

  void HandleConnect(Network &net, RdmaOp &op) {
    auto *base_msg = (AppMessageBase *)op.recv.buf->data;
    CHECK(base_msg->type == AppMessageType::kConnect);
    CHECK(op.recv.recv_size >= sizeof(AppConnectMessage));
    auto &msg = *(AppConnectMessage *)base_msg;
    CHECK(op.recv.recv_size == msg.MessageBytes());
    CHECK(msg.num_mr > 0);
    printf("Received CONNECT message from client: num_gpus=%zu, "
           "num_nets=%zu, num_mr=%zu\n",
           msg.num_gpus, msg.num_nets, msg.num_mr);

    // Save the message. Note that we don't reuse the buffer.
    connect_msg = &msg;

    // Assuming remote has the same number of GPUs and NICs.
    CHECK(msg.num_gpus == cuda_bufs->size());
    CHECK(msg.num_nets == nets->size());

    // Add peer addresses
    nets_per_gpu = msg.num_nets / msg.num_gpus;
    buf_per_gpu = connect_msg->num_mr / connect_msg->num_nets;
    for (size_t i = 0; i < msg.num_gpus; ++i) {
      std::array<fi_addr_t, kMaxNetworksPerGroup> addrs = {};
      for (size_t j = 0; j < nets_per_gpu; ++j) {
        auto idx = i * nets_per_gpu + j;
        addrs[j] = nets->at(idx).AddPeerAddress(msg.net_addr(idx));
      }
      remote_addrs.push_back(addrs);
    }
  }

  void HandleRequest(Network &net, RdmaOp &op) {
    auto *base_msg = (const AppMessageBase *)op.recv.buf->data;
    CHECK(base_msg->type == AppMessageType::kRandomFill);
    CHECK(op.recv.recv_size >= sizeof(AppRandomFillMessage));
    auto &msg = *(AppRandomFillMessage *)base_msg;
    CHECK(op.recv.recv_size == msg.MessageBytes());

    // Save the message. Note that we don't reuse the buffer.
    request_msg = &msg;

    printf("Received RandomFill request from client:\n");
    printf("  remote_context: 0x%08x\n", msg.remote_context);
    printf("  seed: 0x%016lx\n", msg.seed);
    printf("  page_size: %zu\n", msg.page_size);
    printf("  num_pages: %zu\n", msg.num_pages);
    total_repeat = 500 * nets_per_gpu;
    printf("  total_repeat: %zu\n", total_repeat);

    // Generate random data and copy to local GPU memory
    printf("Generating random data");
    fflush(stdout);
    for (size_t i = 0; i < connect_msg->num_gpus; ++i) {
      for (size_t j = 0; j < buf_per_gpu; ++j) {
        auto bytes = RandomBytes(msg.seed + i * buf_per_gpu + j,
                                 msg.page_size * msg.num_pages);
        CUDA_CHECK(
            cudaMemcpy((uint8_t *)cuda_bufs->at(i).data + j * bytes.size(),
                       bytes.data(), bytes.size(), cudaMemcpyHostToDevice));
        printf(".");
        fflush(stdout);
      }
    }
    printf("\n");

    // Prepare for warmup
    write_states.resize(connect_msg->num_gpus);
    state = State::kPostWarmup;
  }

  void PostWarmup(size_t gpu_idx) {
    // Warmup the connection.
    // Write 1 page via each network
    auto &s = write_states[gpu_idx];
    if (s.warmup_posted) {
      return;
    }

    auto page_size = request_msg->page_size;
    auto &group = (*net_groups)[gpu_idx];
    for (size_t k = 0; k < group.nets.size(); ++k) {
      auto net_idx = group.GetNext();
      const auto &mr =
          connect_msg->mr((gpu_idx * nets_per_gpu + net_idx) * buf_per_gpu);
      auto write = RdmaWriteOp{
          .buf = &(*cuda_bufs)[gpu_idx],
          .offset = 0,
          .len = page_size,
          .imm_data = 0,
          .dest_ptr = mr.addr + request_msg->page_idx(0) * page_size,
          .dest_addr = remote_addrs[gpu_idx][net_idx],
          .dest_key = mr.rkey,
      };
      group.nets[net_idx]->PostWrite(std::move(write),
                                     [this](Network &net, RdmaOp &op) {
                                       HandleWarmupCompletion(net, op);
                                     });
    }
    s.warmup_posted = true;
    if (++posted_warmups == connect_msg->num_gpus) {
      state = State::kWaitWarmup;
    }
  }

  void HandleWarmupCompletion(Network &net, RdmaOp &op) {
    if (++cnt_warmups < connect_msg->num_nets) {
      return;
    }
    printf("Warmup completed.\n");

    // Prepare RDMA WRITE the data to remote GPU.
    printf("Started RDMA WRITE to the remote GPU memory.\n");
    total_write_ops_per_gpu =
        buf_per_gpu * request_msg->num_pages * total_repeat;
    total_write_ops = connect_msg->num_gpus * total_write_ops_per_gpu;
    write_op_size = request_msg->page_size;
    write_start_at = std::chrono::high_resolution_clock::now();
    state = State::kWrite;
  }

  void ContinuePostWrite(size_t gpu_idx) {
    auto &s = write_states[gpu_idx];
    if (s.i_repeat == total_repeat)
      return;
    auto page_size = request_msg->page_size;
    auto num_pages = request_msg->num_pages;

    auto &group = (*net_groups)[gpu_idx];
    auto net_idx = group.GetNext();
    uint32_t imm_data = 0;
    if (s.i_repeat + 1 == total_repeat && s.i_buf + 1 == buf_per_gpu &&
        s.i_page + nets_per_gpu >= num_pages) {
      // The last WRITE. Pass remote context back.
      imm_data = request_msg->remote_context;
    }
    const auto &mr = connect_msg->mr(
        (gpu_idx * nets_per_gpu + net_idx) * buf_per_gpu + s.i_buf);
    group.nets[net_idx]->PostWrite(
        {.buf = &(*cuda_bufs)[gpu_idx],
         .offset = s.i_buf * (page_size * num_pages) + s.i_page * page_size,
         .len = page_size,
         .imm_data = imm_data,
         .dest_ptr = mr.addr + request_msg->page_idx(s.i_page) * page_size,
         .dest_addr = remote_addrs[gpu_idx][net_idx],
         .dest_key = mr.rkey},
        [this](Network &net, RdmaOp &op) { HandleWriteCompletion(); });
    ++posted_write_ops;

    if (++s.i_page == num_pages) {
      s.i_page = 0;
      if (++s.i_buf == buf_per_gpu) {
        s.i_buf = 0;
        if (++s.i_repeat == total_repeat)
          return;
      }
    }
  }

  void PrintProgress(std::chrono::high_resolution_clock::time_point now,
                     uint64_t posted, uint64_t finished) {
    auto elapsed = TimeDeltaNanos(write_start_at, now) * 1e-9;
    float bw_gbps = 8.0f * write_op_size * finished / (elapsed * 1e9);
    float bw_util = bw_gbps / (total_bw * 1e-9);
    printf("\r[%.3fs] WRITE: %.0f%%, ops=%zu/%zu, posted=%zu(%.0f%%), "
           "bytes=%zu/%zu, bw=%.3fGbps(%.1f%%), %.3fMpps\033[K",
           // progress
           elapsed, 100.0 * finished / total_write_ops,
           // ops
           finished, total_write_ops, posted, 100.0 * posted / total_write_ops,
           // bytes
           write_op_size * finished, write_op_size * total_write_ops,
           // bw
           bw_gbps, 100.0 * bw_util, finished / elapsed * 1e-6);
    fflush(stdout);
  }

  void HandleWriteCompletion() {
    auto finished = ++finished_write_ops;
    if (finished % 16384 == 0) {
      auto now = std::chrono::high_resolution_clock::now();
      PrintProgress(now, posted_write_ops, finished_write_ops);
    }
    if (finished == total_write_ops) {
      auto now = std::chrono::high_resolution_clock::now();
      PrintProgress(now, posted_write_ops, finished_write_ops);
      printf("\nFinished all RDMA WRITEs to the remote GPU memory.\n");
      state = State::kDone;
    }
  }
};

int ServerMain(int argc, char **argv) {
  if (argc != 1 && argc != 3) {
    fprintf(stderr, "Server Usage: \n");
    fprintf(stderr, "Default runs with all GPUs and all NICs:\n");
    fprintf(stderr, "  %s\n", argv[0]);
    fprintf(stderr, "Alternatively, specify the number of GPUs and NICs:\n");
    fprintf(stderr, "  %s num_gpus num_nics\n", argv[0]);
    std::exit(1);
  }

  // Topology detection
  struct fi_info *info = GetInfo();
  auto topo_groups = DetectTopo(info);
  int num_gpus, num_nets;
  if (argc == 1) {
    num_gpus = topo_groups.size();
    num_nets = topo_groups[0].fi_infos.size() * topo_groups.size();
  } else {
    num_gpus = std::stoi(argv[1]);
    num_nets = std::stoi(argv[2]);
    TrimTopo(topo_groups, num_gpus, num_nets);
  }
  int nets_per_gpu = num_nets / num_gpus;

  // Open Netowrk
  std::vector<Network> nets;
  std::vector<NetworkGroup> net_groups;
  nets.reserve(num_nets);
  net_groups.reserve(num_gpus);
  size_t total_bw = 0;
  for (int cuda_device = 0; cuda_device < num_gpus; ++cuda_device) {
    std::vector<Network *> group_nets;
    for (auto *fi : topo_groups[cuda_device].fi_infos) {
      int cuda_device = nets.size() / nets_per_gpu;
      nets.push_back(Network::Open(fi, cuda_device));
      group_nets.push_back(&nets.back());
      total_bw += info->nic->link_attr->speed;
    }
    net_groups.push_back(NetworkGroup(std::move(group_nets)));
  }
  printf("GPUs: %d, NICs: %d, Total Bandwidth: %.0f Gbps\n", num_gpus, num_nets,
         total_bw * 1e-9);
  PrintTopologyGroups(topo_groups);
  printf("Run client with the following command:\n");
  printf("  %s %d %d %s [page_size num_pages]\n", argv[0], num_gpus, num_nets,
         nets[0].addr.ToString().c_str());

  // Allocate and register message buffer
  auto buf1 = Buffer::Alloc(kMessageBufferSize, kBufAlign);
  auto buf2 = Buffer::Alloc(kMessageBufferSize, kBufAlign);
  nets[0].RegisterMemory(buf1);
  nets[0].RegisterMemory(buf2);

  // Allocate and register CUDA memory
  printf("Registered MR from");
  std::vector<Buffer> cuda_bufs;
  for (int i = 0; i < num_gpus; ++i) {
    CUDA_CHECK(cudaSetDevice(i));
    cuda_bufs.push_back(Buffer::AllocCuda(kMemoryRegionSize * 2, kBufAlign));
    for (int j = 0; j < nets_per_gpu; ++j) {
      nets[i * nets_per_gpu + j].RegisterMemory(cuda_bufs.back());
    }
    printf(" cuda:%d", i);
    fflush(stdout);
  }
  printf("\n");

  // Loop forever. Accept one client at a time.
  for (;;) {
    printf("------\n");
    // State machine
    RandomFillRequestState s(&nets, &net_groups, &cuda_bufs);
    // RECV for CONNECT
    nets[0].PostRecv(buf1,
                     [&s](Network &net, RdmaOp &op) { s.OnRecv(net, op); });
    // RECV for RandomFillRequest
    nets[0].PostRecv(buf2,
                     [&s](Network &net, RdmaOp &op) { s.OnRecv(net, op); });
    // Multi-thread Poll completions
    std::vector<std::thread> threads;
    threads.reserve(num_gpus);
    for (size_t gpu_idx = 0; gpu_idx < net_groups.size(); ++gpu_idx) {
      const auto &cpus = topo_groups[gpu_idx].cpus;
      int preferred_cpu = cpus[cpus.size() / 2];
      threads.emplace_back([&s, preferred_cpu, gpu_idx] {
        // Pin CPU
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(preferred_cpu, &cpuset);
        CHECK(pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t),
                                     &cpuset) == 0);

        // Poll completions
        auto nets = (*s.net_groups)[gpu_idx].nets;
        while (s.state != RandomFillRequestState::State::kDone) {
          for (auto *net : nets) {
            net->PollCompletion();
          }
          switch (s.state) {
          case RandomFillRequestState::State::kWaitRequest:
            break; // Do nothing
          case RandomFillRequestState::State::kPostWarmup:
            s.PostWarmup(gpu_idx);
            break;
          case RandomFillRequestState::State::kWaitWarmup:
            break; // Do nothing
          case RandomFillRequestState::State::kWrite:
            s.ContinuePostWrite(gpu_idx);
            break;
          case RandomFillRequestState::State::kDone:
            break; // Do nothing
          }
        }
      });
    }
    for (auto &t : threads) {
      t.join();
    }
  }

  fi_freeinfo(info);
  return 0;
}

int ClientMain(int argc, char **argv) {
  CHECK(argc == 4 || argc == 6);
  auto server_addrname = EfaAddress::Parse(argv[3]);
  size_t page_size, num_pages;
  if (argc == 6) {
    page_size = std::stoull(argv[4]);
    num_pages = std::stoull(argv[5]);
  } else {
    page_size = 128 * 8 * 2 * 16 * sizeof(uint16_t);
    num_pages = 1000;
  }
  size_t max_pages = kMemoryRegionSize / page_size;
  CHECK(page_size * num_pages <= kMemoryRegionSize);

  // Topology detection
  struct fi_info *info = GetInfo();
  auto topo_groups = DetectTopo(info);
  int num_gpus, num_nets;
  if (argc == 1) {
    num_gpus = topo_groups.size();
    num_nets = topo_groups[0].fi_infos.size() * topo_groups.size();
  } else {
    num_gpus = std::stoi(argv[1]);
    num_nets = std::stoi(argv[2]);
    TrimTopo(topo_groups, num_gpus, num_nets);
  }
  int nets_per_gpu = num_nets / num_gpus;

  // Open Netowrk
  std::vector<Network> nets;
  std::vector<NetworkGroup> net_groups;
  nets.reserve(num_nets);
  net_groups.reserve(num_gpus);
  size_t total_bw = 0;
  for (int cuda_device = 0; cuda_device < num_gpus; ++cuda_device) {
    std::vector<Network *> group_nets;
    for (auto *fi : topo_groups[cuda_device].fi_infos) {
      int cuda_device = nets.size() / nets_per_gpu;
      nets.push_back(Network::Open(fi, cuda_device));
      group_nets.push_back(&nets.back());
      total_bw += info->nic->link_attr->speed;
    }
    net_groups.push_back(NetworkGroup(std::move(group_nets)));
  }
  printf("GPUs: %d, NICs: %d, Total Bandwidth: %.0f Gbps\n", num_gpus, num_nets,
         total_bw * 1e-9);
  PrintTopologyGroups(topo_groups);

  // Add server address to the first network
  auto server_addr = nets[0].AddPeerAddress(server_addrname);

  // Allocate and register message buffer
  auto buf1 = Buffer::Alloc(kMessageBufferSize, kBufAlign);
  nets[0].RegisterMemory(buf1);

  // Allocate and register CUDA memory
  printf("Registered MR from");
  std::vector<Buffer> cuda_bufs1, cuda_bufs2;
  for (int i = 0; i < num_gpus; ++i) {
    CUDA_CHECK(cudaSetDevice(i));
    cuda_bufs1.push_back(Buffer::AllocCuda(kMemoryRegionSize, kBufAlign));
    cuda_bufs2.push_back(Buffer::AllocCuda(kMemoryRegionSize, kBufAlign));
    for (int j = 0; j < nets_per_gpu; ++j) {
      nets[i * nets_per_gpu + j].RegisterMemory(cuda_bufs1.back());
      nets[i * nets_per_gpu + j].RegisterMemory(cuda_bufs2.back());
    }
    printf(" cuda:%d", i);
    fflush(stdout);
  }
  printf("\n");

  // Prepare request
  std::mt19937_64 rng(0xabcdabcd987UL);
  uint64_t req_seed = rng();
  std::vector<uint32_t> page_idx;
  std::vector<uint32_t> tmp(max_pages);
  std::iota(tmp.begin(), tmp.end(), 0);
  std::sample(tmp.begin(), tmp.end(), std::back_inserter(page_idx), num_pages,
              rng);

  // Send address to server
  auto &connect_msg = *(AppConnectMessage *)buf1.data;
  connect_msg = {
      .base = {.type = AppMessageType::kConnect},
      .num_gpus = (size_t)num_gpus,
      .num_nets = nets.size(),
      .num_mr = nets.size() * 2,
  };
  for (size_t i = 0; i < nets.size(); i++) {
    connect_msg.net_addr(i) = nets[i].addr;
    int cuda_device = nets[i].cuda_device;
    connect_msg.mr(i * 2) = {
        .addr = (uint64_t)cuda_bufs1[cuda_device].data,
        .size = cuda_bufs1[cuda_device].size,
        .rkey = nets[i].GetMR(cuda_bufs1[cuda_device])->key,
    };
    connect_msg.mr(i * 2 + 1) = {
        .addr = (uint64_t)cuda_bufs2[cuda_device].data,
        .size = cuda_bufs2[cuda_device].size,
        .rkey = nets[i].GetMR(cuda_bufs2[cuda_device])->key,
    };
  }
  auto send_at = std::chrono::high_resolution_clock::now();
  bool connect_sent = false;
  nets[0].PostSend(
      server_addr, buf1, connect_msg.MessageBytes(),
      [&connect_sent](Network &net, RdmaOp &op) { connect_sent = true; });
  while (!connect_sent) {
    nets[0].PollCompletion();
  }
  auto sent_at = std::chrono::high_resolution_clock::now();
  printf("Sent CONNECT message to server. SEND latency: %.3fus\n",
         1e-3 * TimeDeltaNanos(send_at, sent_at));

  // Prepare to receive the last REMOTE WRITE from server
  int cnt_last_remote_write_received = 0;
  uint32_t remote_write_op_id = 0x123;
  for (auto &net : nets) {
    net.AddRemoteWrite(remote_write_op_id, [&cnt_last_remote_write_received](
                                               Network &net, RdmaOp &op) {
      ++cnt_last_remote_write_received;
    });
  }

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
  send_at = std::chrono::high_resolution_clock::now();
  bool req_sent = false;
  nets[0].PostSend(server_addr, buf1, req_msg.MessageBytes(),
                   [&req_sent](Network &net, RdmaOp &op) { req_sent = true; });
  while (!req_sent) {
    nets[0].PollCompletion();
  }
  sent_at = std::chrono::high_resolution_clock::now();
  printf("Sent RandomFillRequest to server. page_size: %zu, num_pages: %zu, "
         "SEND latency: %.3fus\n",
         page_size, num_pages, 1e-3 * TimeDeltaNanos(send_at, sent_at));

  // Wait for REMOTE WRITE from server
  while (cnt_last_remote_write_received != num_nets) {
    for (auto &net : nets) {
      net.PollCompletion();
    }
  }
  printf("Received RDMA WRITE to local GPU memory.\n");
  printf("Verifying");
  fflush(stdout);

  // Verify data
  auto verify = [&nets, &page_idx, page_size,
                 num_pages](Buffer &cuda_buf, uint64_t seed) -> bool {
    auto actual = std::vector<uint8_t>(page_size * num_pages);
    for (size_t j = 0; j < num_pages; ++j) {
      CUDA_CHECK(cudaMemcpy(actual.data() + j * page_size,
                            (uint8_t *)cuda_buf.data + page_idx[j] * page_size,
                            page_size, cudaMemcpyDeviceToHost));
    }
    auto expected = RandomBytes(seed, page_size * num_pages);
    return expected == actual;
  };
  for (int i = 0; i < num_gpus; ++i) {
    CHECK(verify(cuda_bufs1[i], req_seed + i * 2));
    printf(".");
    fflush(stdout);
    CHECK(verify(cuda_bufs2[i], req_seed + i * 2 + 1));
    printf(".");
    fflush(stdout);
  }
  printf("\n");
  printf("Data is correct\n");

  fi_freeinfo(info);
  return 0;
}

int main(int argc, char **argv) {
  if (argc <= 3) {
    return ServerMain(argc, argv);
  } else {
    return ClientMain(argc, argv);
  }
}
