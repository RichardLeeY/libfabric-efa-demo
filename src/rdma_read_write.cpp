#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <cstring>
#include <memory>
#include <vector>
#include <optional>
#include <chrono>
#include <unordered_map>
#include <functional>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <inttypes.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <cstdlib>

// Configuration constants
constexpr size_t kMessageSize = 1024;           // 1KB messages
constexpr size_t kTotalOperations = 100;        // Total RDMA operations
constexpr size_t kBufferSize = 1024 * 1024;     // 1MB buffer for RDMA operations
constexpr size_t kCompletionQueueReadCount = 16;

// Test patterns
constexpr uint32_t kTestPattern = 0xDEADBEEF;
constexpr uint32_t kReadTestPattern = 0xCAFEBABE;

// Operation types
enum class RdmaOpType {
    kWrite,
    kRead
};

struct RdmaOperation {
    RdmaOpType type;
    size_t offset;
    size_t length;
    uint64_t start_time;
    uint64_t completion_time;
    void* context;
};

struct EfaAddress {
    uint8_t bytes[32];

    EfaAddress() = default;
    EfaAddress(uint8_t bytes[32]) { memcpy(this->bytes, bytes, 32); }

    std::string toString() const {
        char buf[65];
        for (size_t i = 0; i < 32; i++) {
            snprintf(buf + 2 * i, 3, "%02x", bytes[i]);
        }
        return std::string(buf, 64);
    }

    static EfaAddress parse(const std::string &str) {
        if (str.size() != 64) {
            std::cout << "Unexpected efa address length " << str.size() << std::endl;
            std::exit(1);
        }
        uint8_t bytes[32];
        for (size_t i = 0; i < 32; i++) {
            sscanf(str.c_str() + 2 * i, "%02hhx", &bytes[i]);
        }
        return EfaAddress(bytes);
    }
};

// RAII wrapper for libfabric resources
template<typename T>
class FabricResource {
private:
    T* resource_;

public:
    FabricResource() : resource_(nullptr) {}
    explicit FabricResource(T* resource) : resource_(resource) {}

    ~FabricResource() {
        close();
    }

    void close() {
        if (resource_) {
            if constexpr (std::is_same_v<T,fi_info>) {
                fi_freeinfo(resource_);
            } else {
                fi_close(&resource_->fid);
            }
            resource_ = nullptr;
        }
    }

    T* get() const { return resource_; }
    void reset(T* resource) {
        close();
        resource_ = resource;
    }
    T* operator->() const { return resource_; }
    operator bool() const { return resource_ != nullptr; }

    // Disable copy
    FabricResource(const FabricResource&) = delete;
    FabricResource& operator=(const FabricResource&) = delete;

    // Allow move
    FabricResource(FabricResource&& other) noexcept : resource_(other.resource_) {
        other.resource_ = nullptr;
    }

    FabricResource& operator=(FabricResource&& other) noexcept {
        if (this != &other) {
            close();
            resource_ = other.resource_;
            other.resource_ = nullptr;
        }
        return *this;
    }
};

// Get current time in nanoseconds
uint64_t get_timestamp_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

// Convert nanoseconds to microseconds
double ns_to_us(uint64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

class RdmaChannel {
public:
    using CompletionCallback = std::function<void(const RdmaOperation&)>;

private:
    FabricResource<fi_info> info_;
    FabricResource<fid_fabric> fabric_;
    FabricResource<fid_domain> domain_;
    FabricResource<fid_cq> cq_;
    FabricResource<fid_av> av_;
    FabricResource<fid_ep> ep_;
    FabricResource<fid_mr> local_mr_;
    
    void* local_buffer_;
    size_t buffer_size_;
    uint64_t remote_addr_;
    uint64_t remote_key_;
    
    CompletionCallback completion_callback_;
    std::unordered_map<void*, RdmaOperation> pending_operations_;

public:
    RdmaChannel() : local_buffer_(nullptr), buffer_size_(0), remote_addr_(0), remote_key_(0) {}
    
    ~RdmaChannel() {
        if (local_buffer_) std::free(local_buffer_);
    }

    bool init() {
        std::cout << "[DEBUG] Initializing RDMA channel..." << std::endl;
        
        if (!initInfo()) {
            std::cout << "[ERROR] Failed to initialize fabric info" << std::endl;
            return false;
        }
        
        if (!initNetwork()) {
            std::cout << "[ERROR] Failed to initialize network" << std::endl;
            return false;
        }
        
        std::cout << "[DEBUG] RDMA channel initialization complete" << std::endl;
        return true;
    }

    void printInfo() {
        if (!info_.get()) return;

        std::cout << "=== RDMA Channel Information ===" << std::endl;
        std::cout << "Provider: " << info_->fabric_attr->prov_name << std::endl;
        std::cout << "Fabric: " << (info_->fabric_attr->name ? info_->fabric_attr->name : "NULL") << std::endl;
        std::cout << "Domain: " << info_->domain_attr->name << std::endl;
        std::cout << "Device: " << info_->nic->device_attr->name << std::endl;
        std::cout << "Link Speed: " << (info_->nic->link_attr->speed / 1e9) << " Gbps" << std::endl;
        std::cout << "Mode: 0x" << std::hex << info_->mode << std::dec << std::endl;
        std::cout << "MR Mode: 0x" << std::hex << info_->domain_attr->mr_mode << std::dec << std::endl;
        
        bool is_efa_direct = (info_->fabric_attr->name && 
                             std::string(info_->fabric_attr->name) == "efa-direct");
        std::cout << "EFA-Direct: " << (is_efa_direct ? "YES" : "NO") << std::endl;
        std::cout << "=================================" << std::endl;
    }

    EfaAddress getSelfAddress() {
        uint8_t addr[64];
        size_t addrlen = sizeof(addr);
        if (auto ret = fi_getname(&ep_->fid, addr, &addrlen); ret != 0) {
            std::cout << "fi_getname failed: " << fi_strerror(-ret) << std::endl;
            std::exit(1);
        }

        EfaAddress address(addr);
        return address;
    }

    std::optional<fi_addr_t> addPeerAddress(const EfaAddress& peer_addr) {
        fi_addr_t addr = FI_ADDR_UNSPEC;
        if (auto ret = fi_av_insert(av_.get(), peer_addr.bytes, 1, &addr, 0, nullptr); ret != 1) {
            std::cout << "fi_av_insert failed: " << fi_strerror(-ret) << std::endl;
            return std::nullopt;
        }
        return std::make_optional<fi_addr_t>(addr);
    }

    bool allocateBuffer(size_t size) {
        buffer_size_ = size;
        local_buffer_ = std::malloc(size);
        if (!local_buffer_) {
            std::cout << "Failed to allocate buffer" << std::endl;
            return false;
        }
        
        // Initialize buffer with test pattern
        std::memset(local_buffer_, 0, size);
        
        // Register memory with libfabric
        struct fid_mr *mr;
        struct fi_mr_attr mr_attr = {};
        mr_attr.iov_count = 1;
        mr_attr.access = FI_SEND | FI_RECV | FI_REMOTE_WRITE | FI_REMOTE_READ | FI_WRITE | FI_READ;
        struct iovec iov = {.iov_base = local_buffer_, .iov_len = size};
        mr_attr.mr_iov = &iov;
        
        if (auto ret = fi_mr_regattr(domain_.get(), &mr_attr, 0, &mr); ret != 0) {
            std::cout << "fi_mr_regattr failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        local_mr_.reset(mr);
        
        std::cout << "[DEBUG] Allocated and registered " << size << " bytes buffer" << std::endl;
        return true;
    }

    void* getBuffer() const { return local_buffer_; }
    uint64_t getBufferAddr() const { return (uint64_t)local_buffer_; }
    uint64_t getBufferKey() const { return fi_mr_key(local_mr_.get()); }

    void setRemoteBuffer(uint64_t addr, uint64_t key) {
        remote_addr_ = addr;
        remote_key_ = key;
        std::cout << "[DEBUG] Set remote buffer - addr: 0x" << std::hex << addr 
                  << ", key: 0x" << key << std::dec << std::endl;
    }

    void registerCompletionCallback(CompletionCallback&& callback) {
        completion_callback_ = std::move(callback);
    }

    bool rdmaWrite(fi_addr_t dest_addr, size_t offset, size_t length, void* context = nullptr) {
        if (offset + length > buffer_size_) {
            std::cout << "Write operation exceeds buffer bounds" << std::endl;
            return false;
        }

        // Prepare local data with test pattern
        uint32_t* data = static_cast<uint32_t*>(static_cast<char*>(local_buffer_) + offset);
        for (size_t i = 0; i < length / sizeof(uint32_t); i++) {
            data[i] = kTestPattern + i;
        }

        struct iovec iov = {
            .iov_base = static_cast<char*>(local_buffer_) + offset,
            .iov_len = length
        };
        
        struct fi_rma_iov rma_iov = {
            .addr = remote_addr_ + offset,
            .len = length,
            .key = remote_key_
        };
        
        struct fi_msg_rma msg = {
            .msg_iov = &iov,
            .desc = &local_mr_->mem_desc,
            .iov_count = 1,
            .addr = dest_addr,
            .rma_iov = &rma_iov,
            .rma_iov_count = 1,
            .context = context ? context : this,
        };

        // Store operation info for completion tracking
        RdmaOperation op = {
            .type = RdmaOpType::kWrite,
            .offset = offset,
            .length = length,
            .start_time = get_timestamp_ns(),
            .completion_time = 0,
            .context = context
        };
        pending_operations_[msg.context] = op;

        if (auto ret = fi_writemsg(ep_.get(), &msg, 0); ret != 0) {
            std::cout << "fi_writemsg failed: " << fi_strerror(-ret) << std::endl;
            pending_operations_.erase(msg.context);
            return false;
        }

        return true;
    }

    bool rdmaRead(fi_addr_t dest_addr, size_t offset, size_t length, void* context = nullptr) {
        if (offset + length > buffer_size_) {
            std::cout << "Read operation exceeds buffer bounds" << std::endl;
            return false;
        }

        struct iovec iov = {
            .iov_base = static_cast<char*>(local_buffer_) + offset,
            .iov_len = length
        };
        
        struct fi_rma_iov rma_iov = {
            .addr = remote_addr_ + offset,
            .len = length,
            .key = remote_key_
        };
        
        struct fi_msg_rma msg = {
            .msg_iov = &iov,
            .desc = &local_mr_->mem_desc,
            .iov_count = 1,
            .addr = dest_addr,
            .rma_iov = &rma_iov,
            .rma_iov_count = 1,
            .context = context ? context : this,
        };

        // Store operation info for completion tracking
        RdmaOperation op = {
            .type = RdmaOpType::kRead,
            .offset = offset,
            .length = length,
            .start_time = get_timestamp_ns(),
            .completion_time = 0,
            .context = context
        };
        pending_operations_[msg.context] = op;

        if (auto ret = fi_readmsg(ep_.get(), &msg, 0); ret != 0) {
            std::cout << "fi_readmsg failed: " << fi_strerror(-ret) << std::endl;
            pending_operations_.erase(msg.context);
            return false;
        }

        return true;
    }

    void pollCompletion() {
        struct fi_cq_data_entry cqe[kCompletionQueueReadCount];
        while (true) {
            auto ret = fi_cq_read(cq_.get(), cqe, kCompletionQueueReadCount);
            if (ret > 0) {
                for (int i = 0; i < ret; i++) {
                    handleCompletion(cqe[i]);
                }
            } else if (ret == -FI_EAVAIL) {
                handleError();
            } else if (ret == -FI_EAGAIN) {
                break; // No more completions
            } else {
                std::cout << "fi_cq_read error: " << fi_strerror(-ret) << std::endl;
                break;
            }
        }
    }

private:
    bool initInfo() {
        std::cout << "[DEBUG] Initializing fabric info..." << std::endl;
        
        struct fi_info *hints = fi_allocinfo();
        hints->ep_attr->type = FI_EP_RDM;
        hints->fabric_attr->prov_name = strdup("efa");
        hints->fabric_attr->name = strdup("efa-direct");
        hints->mode = FI_CONTEXT2;
        hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY;
        hints->caps = FI_MSG | FI_RMA;
        hints->domain_attr->threading = FI_THREAD_SAFE;

        struct fi_info *info;
        if (auto ret = fi_getinfo(FI_VERSION(2, 0), nullptr, nullptr, 0, hints, &info); ret != 0) {
            std::cout << "[DEBUG] EFA-direct failed, trying regular EFA..." << std::endl;
            
            // Try regular EFA
            free(hints->fabric_attr->name);
            hints->fabric_attr->name = strdup("efa");
            
            if (auto ret2 = fi_getinfo(FI_VERSION(2, 0), nullptr, nullptr, 0, hints, &info); ret2 != 0) {
                std::cout << "fi_getinfo failed for both EFA-direct and EFA: " << fi_strerror(-ret2) << std::endl;
                fi_freeinfo(hints);
                return false;
            }
        }
        
        fi_freeinfo(hints);
        info_.reset(info);
        
        std::cout << "[DEBUG] Successfully initialized fabric info" << std::endl;
        return true;
    }

    bool initNetwork() {
        std::cout << "[DEBUG] Initializing network components..." << std::endl;
        
        // Create fabric
        struct fid_fabric* fabric;
        if (auto ret = fi_fabric(info_->fabric_attr, &fabric, nullptr); ret != 0) {
            std::cout << "fi_fabric failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        fabric_.reset(fabric);

        // Create domain
        struct fid_domain* domain;
        if (auto ret = fi_domain(fabric_.get(), info_.get(), &domain, nullptr); ret != 0) {
            std::cout << "fi_domain failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        domain_.reset(domain);

        // Create completion queue
        struct fid_cq* cq;
        struct fi_cq_attr cq_attr = {};
        cq_attr.format = FI_CQ_FORMAT_DATA;
        if (auto ret = fi_cq_open(domain_.get(), &cq_attr, &cq, nullptr); ret != 0) {
            std::cout << "fi_cq_open failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        cq_.reset(cq);

        // Create address vector
        struct fid_av* av;
        struct fi_av_attr av_attr = {};
        if (auto ret = fi_av_open(domain_.get(), &av_attr, &av, nullptr); ret != 0) {
            std::cout << "fi_av_open failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        av_.reset(av);

        // Create endpoint
        struct fid_ep *ep;
        if (auto ret = fi_endpoint(domain_.get(), info_.get(), &ep, nullptr); ret != 0) {
            std::cout << "fi_endpoint failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        ep_.reset(ep);

        // Bind endpoint to completion queue
        if (auto ret = fi_ep_bind(ep, &cq_->fid, FI_SEND | FI_RECV | FI_WRITE | FI_READ | FI_REMOTE_WRITE | FI_REMOTE_READ); ret != 0) {
            std::cout << "fi_ep_bind cq failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }

        // Bind endpoint to address vector
        if (auto ret = fi_ep_bind(ep, &av_->fid, 0); ret != 0) {
            std::cout << "fi_ep_bind av failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }

        // Enable endpoint
        if (auto ret = fi_enable(ep); ret != 0) {
            std::cout << "fi_enable failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }

        std::cout << "[DEBUG] Network initialization complete" << std::endl;
        return true;
    }

    void handleCompletion(const struct fi_cq_data_entry &cqe) {
        auto it = pending_operations_.find(cqe.op_context);
        if (it != pending_operations_.end()) {
            RdmaOperation op = it->second;
            op.completion_time = get_timestamp_ns();
            
            // Verify data for read operations
            if (op.type == RdmaOpType::kRead) {
                verifyReadData(op.offset, op.length);
            }
            
            if (completion_callback_) {
                completion_callback_(op);
            }
            
            pending_operations_.erase(it);
        }
    }

    void handleError() {
        struct fi_cq_err_entry err_entry;
        auto ret = fi_cq_readerr(cq_.get(), &err_entry, 0);
        if (ret < 0) {
            std::cout << "fi_cq_readerr error: " << fi_strerror(-ret) << std::endl;
        } else if (ret > 0) {
            std::cout << "RDMA operation failed: " << 
                fi_cq_strerror(cq_.get(), err_entry.prov_errno, err_entry.err_data, nullptr, 0) << std::endl;
        }
    }

    void verifyReadData(size_t offset, size_t length) {
        uint32_t* data = static_cast<uint32_t*>(static_cast<char*>(local_buffer_) + offset);
        bool valid = true;
        
        for (size_t i = 0; i < length / sizeof(uint32_t); i++) {
            uint32_t expected = kTestPattern + i;
            if (data[i] != expected) {
                std::cout << "[ERROR] Data verification failed at offset " << (offset + i * sizeof(uint32_t))
                          << " - expected: 0x" << std::hex << expected 
                          << ", got: 0x" << data[i] << std::dec << std::endl;
                valid = false;
                break;
            }
        }
        
        if (valid) {
            std::cout << "[DEBUG] Read data verification passed for " << length << " bytes" << std::endl;
        }
    }
};
// Address exchange utility
void exchangeAddress(const EfaAddress& self, EfaAddress& peer, const std::string& ip, uint16_t port, bool is_server) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr, peer_addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (is_server) {
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
        std::cout << "Server listening for address exchange on port " << port << std::endl;
        
        uint8_t buffer[64];
        socklen_t len = sizeof(peer_addr);
        recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr*)&peer_addr, &len);
        memcpy(peer.bytes, buffer, 32);
        
        sendto(sockfd, self.bytes, 32, 0, (struct sockaddr*)&peer_addr, len);
        std::cout << "Address exchange complete - peer: " << peer.toString() << std::endl;
    } else {
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        sendto(sockfd, self.bytes, 32, 0, (struct sockaddr*)&addr, sizeof(addr));
        
        uint8_t buffer[64];
        socklen_t len = sizeof(peer_addr);
        recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr*)&peer_addr, &len);
        memcpy(peer.bytes, buffer, 32);
        std::cout << "Address exchange complete - peer: " << peer.toString() << std::endl;
    }
    close(sockfd);
}

// Buffer info exchange utility
void exchangeBufferInfo(uint64_t local_addr, uint64_t local_key, uint64_t& remote_addr, uint64_t& remote_key,
                        const std::string& ip, uint16_t port, bool is_server) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port + 1);
    
    if (is_server) {
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
        listen(sockfd, 1);
        std::cout << "Server listening for buffer info exchange on port " << (port + 1) << std::endl;
        
        int client = accept(sockfd, nullptr, nullptr);
        
        // Send local buffer info
        send(client, &local_addr, sizeof(local_addr), 0);
        send(client, &local_key, sizeof(local_key), 0);
        
        // Receive remote buffer info
        recv(client, &remote_addr, sizeof(remote_addr), 0);
        recv(client, &remote_key, sizeof(remote_key), 0);
        
        close(client);
        std::cout << "Buffer info exchange complete" << std::endl;
    } else {
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
        
        // Receive remote buffer info
        recv(sockfd, &remote_addr, sizeof(remote_addr), 0);
        recv(sockfd, &remote_key, sizeof(remote_key), 0);
        
        // Send local buffer info
        send(sockfd, &local_addr, sizeof(local_addr), 0);
        send(sockfd, &local_key, sizeof(local_key), 0);
        
        std::cout << "Buffer info exchange complete" << std::endl;
    }
    close(sockfd);
}

// Statistics calculation
void calculateStatistics(const std::vector<RdmaOperation>& operations) {
    if (operations.empty()) {
        std::cout << "No operations to analyze" << std::endl;
        return;
    }

    std::vector<uint64_t> write_latencies, read_latencies;
    size_t write_count = 0, read_count = 0;
    
    for (const auto& op : operations) {
        uint64_t latency = op.completion_time - op.start_time;
        
        if (op.type == RdmaOpType::kWrite) {
            write_latencies.push_back(latency);
            write_count++;
        } else {
            read_latencies.push_back(latency);
            read_count++;
        }
    }

    std::cout << "\n=== RDMA Operation Statistics ===" << std::endl;
    std::cout << "Total Operations: " << operations.size() << std::endl;
    std::cout << "Write Operations: " << write_count << std::endl;
    std::cout << "Read Operations: " << read_count << std::endl;

    auto printLatencyStats = [](const std::vector<uint64_t>& latencies, const std::string& op_type) {
        if (latencies.empty()) return;
        
        std::vector<uint64_t> sorted_latencies = latencies;
        std::sort(sorted_latencies.begin(), sorted_latencies.end());
        
        std::cout << "\n" << op_type << " Latency Statistics:" << std::endl;
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "  Min: " << ns_to_us(sorted_latencies.front()) << " μs" << std::endl;
        std::cout << "  Max: " << ns_to_us(sorted_latencies.back()) << " μs" << std::endl;
        std::cout << "  P50: " << ns_to_us(sorted_latencies[sorted_latencies.size() * 50 / 100]) << " μs" << std::endl;
        std::cout << "  P90: " << ns_to_us(sorted_latencies[sorted_latencies.size() * 90 / 100]) << " μs" << std::endl;
        std::cout << "  P99: " << ns_to_us(sorted_latencies[sorted_latencies.size() * 99 / 100]) << " μs" << std::endl;
        
        // Calculate average
        uint64_t sum = 0;
        for (uint64_t lat : sorted_latencies) sum += lat;
        std::cout << "  Avg: " << ns_to_us(sum / sorted_latencies.size()) << " μs" << std::endl;
    };

    printLatencyStats(write_latencies, "RDMA Write");
    printLatencyStats(read_latencies, "RDMA Read");
    std::cout << "=================================" << std::endl;
}

int serverMain(int argc, char *argv[]) {
    if (argc != 2) {
        std::cout << "Server usage: " << argv[0] << " <port>" << std::endl;
        return -1;
    }
    
    auto port = std::stoi(argv[1]);
    
    std::cout << "=== RDMA Read/Write Server ===" << std::endl;
    
    // Initialize RDMA channel
    RdmaChannel channel;
    if (!channel.init()) {
        std::cout << "Failed to initialize RDMA channel" << std::endl;
        return -1;
    }
    
    channel.printInfo();
    
    // Allocate buffer
    if (!channel.allocateBuffer(kBufferSize)) {
        std::cout << "Failed to allocate buffer" << std::endl;
        return -1;
    }
    
    // Initialize buffer with test pattern for client reads
    uint32_t* buffer = static_cast<uint32_t*>(channel.getBuffer());
    for (size_t i = 0; i < kBufferSize / sizeof(uint32_t); i++) {
        buffer[i] = kTestPattern + i;
    }
    std::cout << "[DEBUG] Initialized server buffer with test pattern" << std::endl;
    
    // Exchange addresses
    EfaAddress self_addr = channel.getSelfAddress();
    EfaAddress peer_addr;
    std::cout << "Server EFA address: " << self_addr.toString() << std::endl;
    exchangeAddress(self_addr, peer_addr, "", port, true);
    
    auto client_addr = channel.addPeerAddress(peer_addr);
    if (!client_addr) {
        std::cout << "Failed to add peer address" << std::endl;
        return -1;
    }
    
    // Exchange buffer information
    uint64_t remote_addr, remote_key;
    exchangeBufferInfo(channel.getBufferAddr(), channel.getBufferKey(), remote_addr, remote_key, "", port, true);
    
    std::cout << "Server ready. Buffer available for RDMA operations." << std::endl;
    std::cout << "Local buffer: addr=0x" << std::hex << channel.getBufferAddr() 
              << ", key=0x" << channel.getBufferKey() << std::dec << std::endl;
    
    // Wait for client operations (server is passive in this demo)
    std::cout << "Press Enter to exit..." << std::endl;
    std::cin.get();
    
    // Print final buffer state
    std::cout << "\n=== Final Buffer State ===" << std::endl;
    std::cout << "First 16 words of buffer:" << std::endl;
    for (int i = 0; i < 16; i++) {
        std::cout << "  [" << i << "] = 0x" << std::hex << buffer[i] << std::dec << std::endl;
    }
    
    return 0;
}

int clientMain(int argc, char *argv[]) {
    if (argc != 3) {
        std::cout << "Client usage: " << argv[0] << " <server_ip> <port>" << std::endl;
        return -1;
    }
    
    auto ip = std::string(argv[1]);
    auto port = std::stoi(argv[2]);
    
    std::cout << "=== RDMA Read/Write Client ===" << std::endl;
    
    // Initialize RDMA channel
    RdmaChannel channel;
    if (!channel.init()) {
        std::cout << "Failed to initialize RDMA channel" << std::endl;
        return -1;
    }
    
    channel.printInfo();
    
    // Allocate buffer
    if (!channel.allocateBuffer(kBufferSize)) {
        std::cout << "Failed to allocate buffer" << std::endl;
        return -1;
    }
    
    // Exchange addresses
    EfaAddress self_addr = channel.getSelfAddress();
    EfaAddress peer_addr;
    std::cout << "Client EFA address: " << self_addr.toString() << std::endl;
    exchangeAddress(self_addr, peer_addr, ip, port, false);
    
    auto server_addr = channel.addPeerAddress(peer_addr);
    if (!server_addr) {
        std::cout << "Failed to add peer address" << std::endl;
        return -1;
    }
    
    // Exchange buffer information
    uint64_t remote_addr, remote_key;
    exchangeBufferInfo(channel.getBufferAddr(), channel.getBufferKey(), remote_addr, remote_key, ip, port, false);
    channel.setRemoteBuffer(remote_addr, remote_key);
    
    std::cout << "Remote buffer: addr=0x" << std::hex << remote_addr 
              << ", key=0x" << remote_key << std::dec << std::endl;
    
    // Setup completion tracking
    std::vector<RdmaOperation> completed_operations;
    std::atomic<size_t> completed_count{0};
    
    channel.registerCompletionCallback([&](const RdmaOperation& op) {
        completed_operations.push_back(op);
        completed_count.fetch_add(1);
        
        std::string op_type = (op.type == RdmaOpType::kWrite) ? "WRITE" : "READ";
        uint64_t latency = op.completion_time - op.start_time;
        std::cout << "[COMPLETION] " << op_type << " at offset " << op.offset 
                  << ", length " << op.length << ", latency " << ns_to_us(latency) << " μs" << std::endl;
    });
    
    std::cout << "\nStarting RDMA operations..." << std::endl;
    
    // Perform mixed read/write operations
    size_t operations_issued = 0;
    
    for (size_t i = 0; i < kTotalOperations; i++) {
        size_t offset = (i * kMessageSize) % (kBufferSize - kMessageSize);
        
        if (i % 2 == 0) {
            // RDMA Write operation
            std::cout << "[ISSUE] RDMA WRITE at offset " << offset << ", length " << kMessageSize << std::endl;
            if (channel.rdmaWrite(server_addr.value(), offset, kMessageSize)) {
                operations_issued++;
            } else {
                std::cout << "Failed to issue RDMA write" << std::endl;
            }
        } else {
            // RDMA Read operation
            std::cout << "[ISSUE] RDMA READ at offset " << offset << ", length " << kMessageSize << std::endl;
            if (channel.rdmaRead(server_addr.value(), offset, kMessageSize)) {
                operations_issued++;
            } else {
                std::cout << "Failed to issue RDMA read" << std::endl;
            }
        }
        
        // Poll for completions periodically
        if (i % 10 == 0) {
            channel.pollCompletion();
        }
        
        // Small delay between operations
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::cout << "\nIssued " << operations_issued << " operations, waiting for completions..." << std::endl;
    
    // Wait for all completions
    while (completed_count.load() < operations_issued) {
        channel.pollCompletion();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    std::cout << "All operations completed!" << std::endl;
    
    // Calculate and display statistics
    calculateStatistics(completed_operations);
    
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc == 2) {
        return serverMain(argc, argv);
    } else {
        return clientMain(argc, argv);
    }
}