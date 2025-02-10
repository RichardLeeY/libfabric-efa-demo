// clang-format off
/*
Example run:

server$ ./build/8_topo
GPUs: 8, NICs: 32, Total Bandwidth: 3200 Gbps
PCIe Topology:
  cuda:0(53:00.0) NUMA0 CPU 6 rdmap79s0 (4f:00.0) rdmap80s0 (50:00.0) rdmap81s0 (51:00.0) rdmap82s0 (52:00.0)
  cuda:1(64:00.0) NUMA0 CPU18 rdmap96s0 (60:00.0) rdmap97s0 (61:00.0) rdmap98s0 (62:00.0) rdmap99s0 (63:00.0)
  cuda:2(75:00.0) NUMA0 CPU30 rdmap113s0(71:00.0) rdmap114s0(72:00.0) rdmap115s0(73:00.0) rdmap116s0(74:00.0)
  cuda:3(86:00.0) NUMA0 CPU42 rdmap130s0(82:00.0) rdmap131s0(83:00.0) rdmap132s0(84:00.0) rdmap133s0(85:00.0)
  cuda:4(97:00.0) NUMA1 CPU54 rdmap147s0(93:00.0) rdmap148s0(94:00.0) rdmap149s0(95:00.0) rdmap150s0(96:00.0)
  cuda:5(a8:00.0) NUMA1 CPU66 rdmap164s0(a4:00.0) rdmap165s0(a5:00.0) rdmap166s0(a6:00.0) rdmap167s0(a7:00.0)
  cuda:6(b9:00.0) NUMA1 CPU78 rdmap181s0(b5:00.0) rdmap182s0(b6:00.0) rdmap183s0(b7:00.0) rdmap184s0(b8:00.0)
  cuda:7(ca:00.0) NUMA1 CPU90 rdmap198s0(c6:00.0) rdmap199s0(c7:00.0) rdmap200s0(c8:00.0) rdmap201s0(c9:00.0)
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
      fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #stmt);         \
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
  int preferred_cpu;
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
    topo_groups[i].preferred_cpu = topo_groups[i].cpus[cpus_per_gpu / 2];
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
    printf(" CPU%2d", topo.preferred_cpu);
    for (auto *fi : topo.fi_infos) {
      printf(" %-10s(%02x:%02x.%d)", fi->nic->device_attr->name,
             fi->nic->bus_attr->attr.pci.bus_id,
             fi->nic->bus_attr->attr.pci.device_id,
             fi->nic->bus_attr->attr.pci.function_id);
    }
    printf("\n");
  }
}

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

int main() {
  struct fi_info *info = GetInfo();
  int num_nets = 0;
  size_t total_bw = 0;
  for (auto *fi = info; fi; fi = fi->next) {
    ++num_nets;
    total_bw += info->nic->link_attr->speed;
  }
  auto topo_groups = DetectTopo(info);
  printf("GPUs: %zu, NICs: %d, Total Bandwidth: %.0f Gbps\n",
         topo_groups.size(), num_nets, total_bw * 1e-9);
  PrintTopologyGroups(topo_groups);
  fi_freeinfo(info);
  return 0;
}
