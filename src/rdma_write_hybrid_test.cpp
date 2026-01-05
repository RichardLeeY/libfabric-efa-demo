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

// Match the exact constants from rdma_send_recv.cpp for comparison
constexpr size_t kMessageSize = 64;
constexpr size_t kTotalMessageCount = 10000;
constexpr size_t kBufferPoolCapacity = 100;
constexpr uint32_t kSendDurationMs = 10000; // 10 seconds
constexpr uint32_t kSpscQueueSize = 10000;
constexpr size_t kCompletionQueueReadCount = 16;

// Hybrid latency message structure
struct HybridLatencyMessage {
    std::atomic<uint64_t> sequence_number{0};
    uint64_t client_send_timestamp;
    uint64_t server_memory_timestamp;    // When server detected data in memory
    uint64_t server_completion_timestamp; // When completion event fired
    char padding[32];
} __attribute__((aligned(64)));

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
            std::cout << "Unexpected efa address length " << str.size() << std::endl;;
            std::exit(1);
        }
        uint8_t bytes[32];
        for (size_t i = 0; i < 32; i++) {
            sscanf(str.c_str() + 2 * i, "%02hhx", &bytes[i]);
        }
        return EfaAddress(bytes);
    }
};

class Buffer {
private:
    void* data_;
    size_t size_;
    
public:
    Buffer(size_t size) {
        data_ = std::malloc(size);
        size_ = size;
        std::memset(data_, 0, size_);
    }
    
    ~Buffer() {
        std::free(data_);
    }
    
    void* data() const {
        return data_;
    }
    
    size_t size() const {
        return size_;
    }
};

// RAII wrapper for libfabric resource
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
            }
            else {
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

    // Disable copy, allow move
    FabricResource(const FabricResource&) = delete;
    FabricResource& operator=(const FabricResource&) = delete;
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

uint64_t get_timestamp_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

double ns_to_us(uint64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

template <typename T, size_t Size>
class SpscQueue {
private:
    std::array<T, Size+1> buffer_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};

public:
    bool push(const T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % (Size+1);

        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; // Queue is full
        }

        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t current_head = head_.load(std::memory_order_relaxed);

        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false; // Queue is empty
        }

        item = buffer_[current_head];
        head_.store((current_head + 1) % (Size+1), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }
};

class RdmaHybridChannel {
public:
    using WriteCallback = std::function<void(void*)>;
    
private:
    FabricResource<fi_info> info_;
    FabricResource<fid_fabric> fabric_;
    FabricResource<fid_domain> domain_;
    FabricResource<fid_cq> cq_;
    FabricResource<fid_av> av_;
    FabricResource<fid_ep> ep_;
    
    std::unordered_map<void*, FabricResource<fid_mr>> memoryRegionMap_;
    WriteCallback writeCallback_;

public:
    bool init() {
        if (!initInfo()) return false;
        if (!initNetwork()) return false;
        return true;
    }

    void printInfo() {
        if (info_.get() == nullptr) return;
        std::cout << "--------------------------------" << std::endl;
        std::cout << "domain: " << info_->domain_attr->name << std::endl;
        std::cout << "nic: " << info_->nic->device_attr->name << std::endl;
        std::cout << "fabric: " << info_->fabric_attr->prov_name << std::endl;
        std::cout << "link: " << (info_->nic->link_attr->speed / 1e9) << "Gbps" << std::endl;
        std::cout << "--------------------------------" << std::endl;
    }

    EfaAddress getSelfAddress() {
        uint8_t addr[64];
        size_t addrlen = sizeof(addr);
        if (auto ret = fi_getname(&ep_->fid, addr, &addrlen); ret != 0) {
            std::cout << "fi_getname failed: " << fi_strerror(-ret) << std::endl;
            std::exit(1);
        }
        return EfaAddress(addr);
    }
    
    void registerWriteCallback(WriteCallback&& callback) {
        writeCallback_ = callback;
    }
    
    bool registerMemory(void* data, size_t size) {
        FabricResource<fid_mr> resource;
        
        struct fid_mr *mr;
        struct fi_mr_attr mr_attr = {
            .iov_count = 1,
            .access = FI_REMOTE_WRITE | FI_REMOTE_READ | FI_WRITE | FI_READ,
        };
        
        struct iovec iov = {.iov_base = data, .iov_len = size};
        mr_attr.mr_iov = &iov;
        
        if (auto ret = fi_mr_regattr(domain_.get(), &mr_attr, 0, &mr); ret != 0) {
            std::cout << "[DEBUG] Memory registration failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        
        resource.reset(mr);
        memoryRegionMap_.emplace(data, std::move(resource));
        return true;
    }
    
    std::optional<fi_addr_t> addPeerAddress(const EfaAddress& peer_addr) {
        fi_addr_t addr = FI_ADDR_UNSPEC;
        if (auto ret = fi_av_insert(av_.get(), peer_addr.bytes, 1, &addr, 0, nullptr); ret != 1) {
            std::cout << "fi_av_insert failed: " << fi_strerror(-ret) << " (returned " << ret << ")" << std::endl;
            return std::nullopt;
        }
        return std::make_optional<fi_addr_t>(addr);
    }
    
    bool postWriteWithImmediate(fi_addr_t dest_addr, void* local_mr_ptr, void* local_data,
                               uint64_t remote_addr, uint64_t remote_key, size_t size, 
                               uint32_t immediate_data, void* context = nullptr) {
        if (auto iter = memoryRegionMap_.find(local_mr_ptr); iter != memoryRegionMap_.end()) {
            auto& mr = iter->second;

            struct iovec iov = {
                .iov_base = local_data,
                .iov_len = size,
            };
            
            struct fi_rma_iov rma_iov = {
                .addr = remote_addr,
                .len = size,
                .key = remote_key
            };
        
            struct fi_msg_rma msg = {
                .msg_iov = &iov,
                .desc = &mr->mem_desc,
                .iov_count = 1,
                .addr = dest_addr,
                .rma_iov = &rma_iov,
                .rma_iov_count = 1,
                .context = context ? context : local_data,
                .data = immediate_data  // Immediate data
            };
            
            if (auto ret = fi_writemsg(ep_.get(), &msg, FI_REMOTE_CQ_DATA); ret != 0) {
                std::cout << "fi_writemsg with immediate failed: " << fi_strerror(-ret) << std::endl;
                return false;
            }
            return true;
        }
        
        std::cout << "local memory region not found!" << std::endl;
        return false;
    }

    void pollWrite() {
        pollCompletion();
    }
    
    fid_cq* getCompletionQueue() {
        return cq_.get();
    }
    
    uint64_t getRemoteKey(void* mr_ptr) {
        if (auto iter = memoryRegionMap_.find(mr_ptr); iter != memoryRegionMap_.end()) {
            return fi_mr_key(iter->second.get());
        }
        return 0;
    }
    
private:
    bool initInfo() {
        struct fi_info *hints, *info;
        hints = fi_allocinfo();
        hints->ep_attr->type = FI_EP_RDM;
        hints->fabric_attr->prov_name = strdup("efa");
        hints->domain_attr->threading = FI_THREAD_SAFE;
        hints->caps = FI_MSG | FI_RMA;

        if (auto ret = fi_getinfo(FI_VERSION(2, 0), nullptr, nullptr, 0, hints, &info); ret != 0) {
            std::cout << "fi_getinfo failed: " << fi_strerror(-ret) << std::endl;
            fi_freeinfo(hints);
            return false;
        }

        info_.reset(info);
        fi_freeinfo(hints);
        return true;
    }
    
    bool initNetwork() {
        struct fid_fabric* fabric;
        if (auto ret = fi_fabric(info_->fabric_attr, &fabric, nullptr); ret != 0) {
            std::cout << "fi_fabric failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        fabric_.reset(fabric);

        struct fid_domain* domain;
        if (auto ret = fi_domain(fabric_.get(), info_.get(), &domain, nullptr); ret != 0) {
            std::cout << "fi_domain failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        domain_.reset(domain);
        
        // Create completion queue with larger size
        struct fid_cq* cq;
        struct fi_cq_attr cq_attr = {};
        cq_attr.format = FI_CQ_FORMAT_DATA;
        cq_attr.size = 4096;  // Set CQ size to 4096 entries
        if (auto ret = fi_cq_open(domain_.get(), &cq_attr, &cq, nullptr); ret != 0) {
            std::cout << "fi_cq_open failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        cq_.reset(cq);
        
        std::cout << "CQ Size: requested=" << cq_attr.size << std::endl;

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
        if (auto ret = fi_ep_bind(ep, &cq_->fid, FI_TRANSMIT | FI_RECV); ret != 0) {
            std::cout << "[LINE " << __LINE__ << "] fi_ep_bind to cq failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }

        // Bind endpoint to address vector
        if (auto ret = fi_ep_bind(ep, &av_->fid, 0); ret != 0) {
            std::cout << "[LINE " << __LINE__ << "] fi_ep_bind to av failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }

        // Enable endpoint
        if (auto ret = fi_enable(ep); ret != 0) {
            std::cout << "fi_enable failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        
        std::cout << "Endpoint enabled successfully" << std::endl;
        
        return true;
    }
    
    void pollCompletion() {
        struct fi_cq_data_entry cqe[kCompletionQueueReadCount];
        while (true) {
            auto ret = fi_cq_read(cq_.get(), cqe, kCompletionQueueReadCount);
            if (ret > 0) {
                // Successfully read and consumed 'ret' completion entries
                for (uint32_t i=0; i<ret; ++i) {
                    handleCompletion(cqe[i]);
                }
                // Entries are now automatically deleted/consumed from CQ
            }
            else if (ret == -FI_EAVAIL) {
                struct fi_cq_err_entry err_entry;
                ret = fi_cq_readerr(cq_.get(), &err_entry, 0);
                if (ret < 0) {
                    fprintf(stderr, "fi_cq_readerr error: %zd (%s)\n", ret, fi_strerror(-ret));
                    std::exit(1);
                }
                else if (ret > 0) {
                    fprintf(stderr, "Failed libfabric operation: %s\n",
                        fi_cq_strerror(cq_.get(), err_entry.prov_errno, err_entry.err_data, nullptr, 0));
                }
                else {
                    fprintf(stderr, "fi_cq_readerr returned 0 unexpectedly.\n");
                    std::exit(1);
                }
            }
            else if (ret == -FI_EAGAIN) {
                break; // No more completions available
            }
            else {
                fprintf(stderr, "fi_cq_read error: %zd (%s)\n", ret, fi_strerror(-ret));
                std::exit(1);
            }
        }
    }
    
    void handleCompletion(const struct fi_cq_data_entry &cqe) {
        auto comp_flags = cqe.flags;
        auto data = cqe.op_context;
        if (data == nullptr) return;
        
        if (comp_flags & FI_WRITE) {
            writeCallback_(data);
        }
        else {
            fprintf(stderr, "Unhandled completion type. comp_flags=%lx\n", comp_flags);
            std::exit(1);
        }
    }
};

class EfaAddressExchange {
public:
    EfaAddressExchange(const EfaAddress& addr) : selfAddr_(addr) {
        std::cout << "Self EFA address = " << selfAddr_.toString() << std::endl;
    };
    
    void waitForPeer(uint16_t port) {
        listenAddressFromPeer(port);
        sendAddressToPeer();
    }
    
    void sendToPeer(const std::string& ip, uint16_t port) {
        std::thread recvThread([this, port](){
            listenAddressFromPeer(port);
        });

        std::this_thread::sleep_for(std::chrono::seconds{1});       
        sendAddressToPeer(ip, port);
        recvThread.join();
    }
    
    const EfaAddress& getPeerAddress() const {
        return peerAddr_;
    }
    
private:
    void sendAddressToPeer(const std::string& ip, uint16_t port) {
        memset(&peerSockAddr_, 0, sizeof(peerSockAddr_));
        peerSockAddr_.sin_family = AF_INET;
        peerSockAddr_.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &peerSockAddr_.sin_addr);
        sendAddressToPeer();
    }
    
    void sendAddressToPeer() {
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            std::cerr << "Error creating socket" << std::endl;
            return;
        }
        sendto(sockfd, selfAddr_.bytes, sizeof(selfAddr_.bytes), 0, 
               (struct sockaddr*)&peerSockAddr_, sizeof(peerSockAddr_));      
        close(sockfd);
    }
    
    void listenAddressFromPeer(uint16_t port) {
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            std::cerr << "Error creating socket" << std::endl;
            return;
        }

        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = INADDR_ANY;
        local_addr.sin_port = htons(port);

        if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            std::cerr << "Error binding socket: " << strerror(errno) << std::endl;
            close(sockfd);
            return;
        }

        std::cout << "Listening peer EFA address on port: " << port << std::endl;

        uint8_t buffer[64];
        socklen_t server_len = sizeof(peerSockAddr_);
        int bytes_received = recvfrom(sockfd, buffer, sizeof(buffer), 0, 
                                    (struct sockaddr*)&peerSockAddr_, &server_len);
        if (bytes_received != 32) {
            std::cout << "Received invalid EFA address, message length = " << bytes_received << std::endl;
            std::exit(1);
        }
        
        peerAddr_ = EfaAddress(buffer);
        std::cout << "Peer EFA address = " << peerAddr_.toString() << std::endl;
        
        peerSockAddr_.sin_port = htons(port);
        close(sockfd);
    }

    EfaAddress selfAddr_;
    EfaAddress peerAddr_;
    struct sockaddr_in peerSockAddr_;
};

// Hybrid server-side latency measurement
class HybridServerLatencyMeasurement {
private:
    struct ServerLatencyData {
        uint64_t client_send_time;
        uint64_t memory_detect_time;
        uint64_t completion_event_time;
        uint64_t sequence;
        
        uint64_t end_to_end_latency() const {
            return memory_detect_time - client_send_time;
        }
        
        uint64_t completion_vs_memory_delta() const {
            return completion_event_time - memory_detect_time;
        }
    };
    
    std::vector<ServerLatencyData> measurements_;
    std::atomic<bool> stop_polling_{false};
    
public:
    void memoryPollingThread(HybridLatencyMessage* msg_ptr, size_t expected_messages) {
        std::cout << "Server: Starting memory polling thread..." << std::endl;
        
        uint64_t last_sequence = 0;
        size_t detected_count = 0;
        
        while (detected_count < expected_messages && !stop_polling_.load()) {
            uint64_t current_seq = msg_ptr->sequence_number.load(std::memory_order_acquire);
            
            if (current_seq > last_sequence) {
                uint64_t memory_detect_time = get_timestamp_ns();
                msg_ptr->server_memory_timestamp = memory_detect_time;
                
                if (detected_count < 5) {
                    if (detected_count == 0) {
                        std::cout << "\n--- Server Memory Detection: First 5 Messages ---" << std::endl;
                        std::cout << std::setw(5) << "Msg#" 
                                 << std::setw(15) << "Client Send (μs)" 
                                 << std::setw(15) << "Memory Detect (μs)" 
                                 << std::setw(15) << "E2E Latency (μs)" << std::endl;
                        std::cout << std::string(65, '-') << std::endl;
                    }
                    
                    uint64_t e2e_latency = memory_detect_time - msg_ptr->client_send_timestamp;
                    
                    std::cout << std::fixed << std::setprecision(3);
                    std::cout << std::setw(5) << (detected_count + 1)
                             << std::setw(15) << (msg_ptr->client_send_timestamp / 1000.0)
                             << std::setw(15) << (memory_detect_time / 1000.0)
                             << std::setw(15) << (e2e_latency / 1000.0) << std::endl;
                             
                    if (detected_count == 4) {
                        std::cout << std::string(65, '-') << std::endl;
                    }
                }
                
                last_sequence = current_seq;
                detected_count++;
                
                if (detected_count % 1000 == 0) {
                    std::cout << "Memory polling detected " << detected_count << " messages" << std::endl;
                }
            }
        }
        
        std::cout << "Memory polling thread completed - detected " << detected_count << " messages" << std::endl;
    }
    
    void completionPollingThread(fid_cq* cq, HybridLatencyMessage* msg_ptr, size_t expected_messages) {
        std::cout << "Server: Starting completion polling thread..." << std::endl;
        
        size_t completion_count = 0;
        struct fi_cq_data_entry cqe;
        
        while (completion_count < expected_messages) {
            auto ret = fi_cq_read(cq, &cqe, 1);
            if (ret > 0) {
                uint64_t completion_time = get_timestamp_ns();
                
                if (cqe.flags & FI_REMOTE_CQ_DATA) {
                    ServerLatencyData data = {
                        .client_send_time = msg_ptr->client_send_timestamp,
                        .memory_detect_time = msg_ptr->server_memory_timestamp,
                        .completion_event_time = completion_time,
                        .sequence = cqe.data
                    };
                    
                    measurements_.push_back(data);
                    completion_count++;
                    
                    if (completion_count <= 5) {
                        if (completion_count == 1) {
                            std::cout << "\n--- Server Completion Events: First 5 Messages ---" << std::endl;
                            std::cout << std::setw(5) << "Msg#" 
                                     << std::setw(18) << "Memory (μs)" 
                                     << std::setw(18) << "Completion (μs)" 
                                     << std::setw(15) << "Delta (μs)" << std::endl;
                            std::cout << std::string(56, '-') << std::endl;
                        }
                        
                        uint64_t delta = data.completion_vs_memory_delta();
                        
                        std::cout << std::fixed << std::setprecision(3);
                        std::cout << std::setw(5) << completion_count
                                 << std::setw(18) << (data.memory_detect_time / 1000.0)
                                 << std::setw(18) << (completion_time / 1000.0)
                                 << std::setw(15) << (delta / 1000.0) << std::endl;
                                 
                        if (completion_count == 5) {
                            std::cout << std::string(56, '-') << std::endl;
                        }
                    }
                    
                    if (completion_count % 1000 == 0) {
                        std::cout << "Completion polling received " << completion_count << " events" << std::endl;
                    }
                }
            } else if (ret == -FI_EAGAIN) {
                continue;
            } else {
                std::cout << "fi_cq_read error: " << fi_strerror(-ret) << std::endl;
                break;
            }
        }
        
        stop_polling_.store(true);
        std::cout << "Completion polling thread completed - received " << completion_count << " events" << std::endl;
    }
    
    void printHybridStatistics() {
        if (measurements_.empty()) return;
        
        std::vector<uint64_t> e2e_latencies;
        std::vector<uint64_t> completion_deltas;
        std::vector<uint64_t> rdma_latencies; // Pure RDMA write latency
        
        for (const auto& data : measurements_) {
            e2e_latencies.push_back(data.end_to_end_latency());
            completion_deltas.push_back(data.completion_vs_memory_delta());
            rdma_latencies.push_back(data.end_to_end_latency()); // Same as e2e for this test
        }
        
        std::sort(e2e_latencies.begin(), e2e_latencies.end());
        std::sort(completion_deltas.begin(), completion_deltas.end());
        std::sort(rdma_latencies.begin(), rdma_latencies.end());
        
        std::cout << "\n=== Hybrid Server-Side RDMA Write Latency Analysis ===" << std::endl;
        std::cout << "Total measurements: " << measurements_.size() << std::endl;
        
        std::cout << std::fixed << std::setprecision(3);
        
        std::cout << "\nRDMA Write Latency (Client Send → Server Memory Detection):" << std::endl;
        std::cout << "  Min: " << (rdma_latencies.front() / 1000.0) << " μs" << std::endl;
        std::cout << "  Max: " << (rdma_latencies.back() / 1000.0) << " μs" << std::endl;
        std::cout << "  P50: " << (rdma_latencies[rdma_latencies.size() * 50 / 100] / 1000.0) << " μs" << std::endl;
        std::cout << "  P90: " << (rdma_latencies[rdma_latencies.size() * 90 / 100] / 1000.0) << " μs" << std::endl;
        std::cout << "  P99: " << (rdma_latencies[rdma_latencies.size() * 99 / 100] / 1000.0) << " μs" << std::endl;
        
        std::cout << "\nEnd-to-End Latency (Client Send → Server Memory Detection):" << std::endl;
        std::cout << "  Min: " << (e2e_latencies.front() / 1000.0) << " μs" << std::endl;
        std::cout << "  Max: " << (e2e_latencies.back() / 1000.0) << " μs" << std::endl;
        std::cout << "  P50: " << (e2e_latencies[e2e_latencies.size() * 50 / 100] / 1000.0) << " μs" << std::endl;
        std::cout << "  P90: " << (e2e_latencies[e2e_latencies.size() * 90 / 100] / 1000.0) << " μs" << std::endl;
        std::cout << "  P99: " << (e2e_latencies[e2e_latencies.size() * 99 / 100] / 1000.0) << " μs" << std::endl;
        
        std::cout << "\nCompletion Event vs Memory Detection Delta:" << std::endl;
        std::cout << "  Min: " << (completion_deltas.front() / 1000.0) << " μs" << std::endl;
        std::cout << "  Max: " << (completion_deltas.back() / 1000.0) << " μs" << std::endl;
        std::cout << "  P50: " << (completion_deltas[completion_deltas.size() * 50 / 100] / 1000.0) << " μs" << std::endl;
        std::cout << "  P90: " << (completion_deltas[completion_deltas.size() * 90 / 100] / 1000.0) << " μs" << std::endl;
        std::cout << "  P99: " << (completion_deltas[completion_deltas.size() * 99 / 100] / 1000.0) << " μs" << std::endl;
        
        // Final RDMA latency summary
        std::cout << "\n=== FINAL RDMA LATENCY RESULTS ===" << std::endl;
        std::cout << "P50: " << (rdma_latencies[rdma_latencies.size() * 50 / 100] / 1000.0) << " μs" << std::endl;
        std::cout << "P90: " << (rdma_latencies[rdma_latencies.size() * 90 / 100] / 1000.0) << " μs" << std::endl;
        std::cout << "P99: " << (rdma_latencies[rdma_latencies.size() * 99 / 100] / 1000.0) << " μs" << std::endl;
    }
};

int serverMain(int argc, char *argv[]) {
    if (argc != 2) {
        std::cout << "Server mode usage: rdma_write_hybrid_test <communication port>" << std::endl;
        return -1;
    }

    auto port = std::stoi(argv[1]);
    
    RdmaHybridChannel channel;
    if (!channel.init()) return -1;
    channel.printInfo();

    EfaAddressExchange exchange{channel.getSelfAddress()};
    exchange.waitForPeer(port);
    
    auto client_addr = channel.addPeerAddress(exchange.getPeerAddress());
    if (!client_addr) return -1;
    
    std::cout << "[DEBUG] Server: Registering memory region..." << std::endl;
    auto receiveBuffer = Buffer(sizeof(HybridLatencyMessage));
    if (!channel.registerMemory(receiveBuffer.data(), receiveBuffer.size())) {
        std::cout << "[ERROR] Failed to register memory" << std::endl;
        return -1;
    }
    
    auto remote_key = channel.getRemoteKey(receiveBuffer.data());
    std::cout << "[DEBUG] Server: Memory registered, key=" << remote_key << std::endl;
    
    // Send memory region info to client
    struct MemoryRegionInfo {
        uint64_t addr;
        uint64_t key;
    } mr_info;
    
    mr_info.addr = reinterpret_cast<uint64_t>(receiveBuffer.data());
    mr_info.key = remote_key;
    
    std::cout << "[DEBUG] Server: Setting up UDP socket for memory region exchange..." << std::endl;
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cout << "[ERROR] Server: Failed to create socket: " << strerror(errno) << std::endl;
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port + 1);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cout << "[ERROR] Server: Failed to bind socket on port " << (port + 1) << ": " << strerror(errno) << std::endl;
        close(sockfd);
        return -1;
    }
    
    std::cout << "[DEBUG] Server: Socket bound successfully on port " << (port + 1) << std::endl;
    
    struct sockaddr_in client_sock_addr;
    std::cout << "[DEBUG] Server: Waiting for client memory region request on port " << (port + 1) << "..." << std::endl;
    char dummy_buffer[1];
    socklen_t client_len = sizeof(client_sock_addr);
    ssize_t recv_bytes = recvfrom(sockfd, dummy_buffer, sizeof(dummy_buffer), 0, 
                                  (struct sockaddr*)&client_sock_addr, &client_len);
    
    if (recv_bytes < 0) {
        std::cout << "[ERROR] Server: Failed to receive request: " << strerror(errno) << std::endl;
        close(sockfd);
        return -1;
    }
    
    std::cout << "[DEBUG] Server: Received client request (" << recv_bytes << " bytes)" << std::endl;
    std::cout << "[DEBUG] Server: Sending memory region info..." << std::endl;
    
    ssize_t sent_bytes = sendto(sockfd, &mr_info, sizeof(mr_info), 0, 
                               (struct sockaddr*)&client_sock_addr, sizeof(client_sock_addr));
    if (sent_bytes < 0) {
        std::cout << "[ERROR] Server: Failed to send response: " << strerror(errno) << std::endl;
        close(sockfd);
        return -1;
    }
    
    close(sockfd);
    std::cout << "[DEBUG] Server: Memory region exchange completed (" << sent_bytes << " bytes sent)" << std::endl;
    
    std::cout << "[DEBUG] Server initialization complete, starting hybrid measurement" << std::endl;
    
    auto msg_ptr = static_cast<HybridLatencyMessage*>(receiveBuffer.data());
    HybridServerLatencyMeasurement measurement;
    
    // Start both measurement threads
    std::thread memoryThread([&measurement, msg_ptr]() {
        measurement.memoryPollingThread(msg_ptr, kTotalMessageCount);
    });
    
    std::thread completionThread([&measurement, &channel, msg_ptr]() {
        measurement.completionPollingThread(channel.getCompletionQueue(), msg_ptr, kTotalMessageCount);
    });
    
    memoryThread.join();
    completionThread.join();
    
    measurement.printHybridStatistics();
    
    return 0;
}

int clientMain(int argc, char *argv[]) {
    if (argc != 3) {
        std::cout << "Client mode usage: rdma_write_hybrid_test <remote ip> <communication port>" << std::endl;
        return -1;
    }
    
    auto ip = std::string(argv[1]);
    auto port = std::stoi(argv[2]);
    
    RdmaHybridChannel channel;
    if (!channel.init()) return -1;

    EfaAddressExchange exchange(channel.getSelfAddress());
    exchange.sendToPeer(ip, port);

    auto server_addr = channel.addPeerAddress(exchange.getPeerAddress());
    if (!server_addr) return -1;

    std::cout << "[DEBUG] Client: Registering memory region..." << std::endl;
    auto sendBuffer = Buffer(sizeof(HybridLatencyMessage));
    if (!channel.registerMemory(sendBuffer.data(), sendBuffer.size())) {
        std::cout << "[ERROR] Failed to register memory" << std::endl;
        return -1;
    }
    
    std::cout << "[DEBUG] Client: Getting remote memory region info..." << std::endl;
    
    // Get remote memory region info
    struct MemoryRegionInfo {
        uint64_t addr;
        uint64_t key;
    } remote_mr_info;
    
    std::cout << "[DEBUG] Client: Creating UDP socket for memory region exchange..." << std::endl;
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cout << "[ERROR] Client: Failed to create socket: " << strerror(errno) << std::endl;
        return -1;
    }
    
    struct sockaddr_in server_sock_addr;
    memset(&server_sock_addr, 0, sizeof(server_sock_addr));
    server_sock_addr.sin_family = AF_INET;
    server_sock_addr.sin_port = htons(port + 1);
    inet_pton(AF_INET, ip.c_str(), &server_sock_addr.sin_addr);
    
    std::cout << "[DEBUG] Client: Sending request to server on port " << (port + 1) << "..." << std::endl;
    char dummy = 1;
    if (sendto(sockfd, &dummy, sizeof(dummy), 0, 
               (struct sockaddr*)&server_sock_addr, sizeof(server_sock_addr)) < 0) {
        std::cout << "[ERROR] Client: Failed to send request: " << strerror(errno) << std::endl;
        close(sockfd);
        return -1;
    }
    
    std::cout << "[DEBUG] Client: Waiting for server response..." << std::endl;
    socklen_t server_len = sizeof(server_sock_addr);
    if (recvfrom(sockfd, &remote_mr_info, sizeof(remote_mr_info), 0,
                 (struct sockaddr*)&server_sock_addr, &server_len) < 0) {
        std::cout << "[ERROR] Client: Failed to receive response: " << strerror(errno) << std::endl;
        close(sockfd);
        return -1;
    }
    close(sockfd);
    
    std::cout << "[DEBUG] Client: Received memory region info - addr=" << remote_mr_info.addr 
              << ", key=" << remote_mr_info.key << std::endl;
    
    std::cout << "[DEBUG] Client initialization complete, ready to send RDMA writes with immediate data" << std::endl;

    std::atomic<uint32_t> writeCount{0};
    std::atomic<uint32_t> completionPollCount{0};
    channel.registerWriteCallback([&](void* data){
        writeCount.fetch_add(1, std::memory_order_relaxed);
        if (writeCount.load() % 1000 == 0) {
            std::cout << "[DEBUG] Client: Processed " << writeCount.load() << " write completions" << std::endl;
        }
    });
    
    std::atomic_bool done = false;
    std::thread writeCompletionThread([&done, &channel, &completionPollCount]() {
        std::cout << "[DEBUG] Client: Starting completion polling thread" << std::endl;
        uint32_t poll_count = 0;
        auto start_time = std::chrono::steady_clock::now();
        
        while (!done.load(std::memory_order_acquire)) {
            channel.pollWrite();
            poll_count++;
            
            if (poll_count % 100000 == 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                
                // Safety timeout - if polling for more than 30 seconds, something is wrong
                if (elapsed > 30) {
                    std::cout << "[ERROR] Client: Completion polling timeout after 30s, stopping" << std::endl;
                    break;
                }
            }
        }
        std::cout << "[DEBUG] Client: Completion polling thread finished" << std::endl;
    });

    SpscQueue<uint64_t, kSpscQueueSize> writeQueue;
    std::thread writeThread([&done, &channel, &writeQueue, &sendBuffer, &server_addr, &remote_mr_info, &writeCount]() {
        std::cout << "Started write thread with immediate data" << std::endl;
        
        // Wait for system to be ready
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        uint32_t sendCount = 0;
        uint32_t successCount = 0;
        uint32_t failCount = 0;
        auto start_time = std::chrono::steady_clock::now();
        
        auto msg_ptr = static_cast<HybridLatencyMessage*>(sendBuffer.data());
        
        while (sendCount < kTotalMessageCount) {
            // Check for timeout - if write thread runs too long, give up
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed > 25) {  // 25 second timeout for write thread
                std::cout << "[ERROR] Client: Write thread timeout after 25s, sent " << successCount << "/" << kTotalMessageCount << std::endl;
                break;
            }
            
            uint64_t queueTs = 0;
            if (writeQueue.pop(queueTs)) {
                auto sendTs = get_timestamp_ns();
                
                msg_ptr->sequence_number.store(sendCount + 1, std::memory_order_release);
                msg_ptr->client_send_timestamp = sendTs;
                msg_ptr->server_memory_timestamp = 0;
                msg_ptr->server_completion_timestamp = 0;
                
                // Try to send with flow control
                int retry_count = 0;
                bool sent = false;
                while (retry_count < 10 && !sent) {  // Reduced retries
                    if (channel.postWriteWithImmediate(server_addr.value(), sendBuffer.data(), sendBuffer.data(),
                                                       remote_mr_info.addr, remote_mr_info.key, 
                                                       sizeof(HybridLatencyMessage), sendCount + 1)) {
                        successCount++;
                        sendCount++;
                        sent = true;
                        
                        if (successCount % 1000 == 0) {
                            std::cout << "[DEBUG] Client: Successfully sent " << successCount << " writes" << std::endl;
                        }
                    } else {
                        retry_count++;
                        failCount++;
                        
                        // Poll completions to drain queue
                        for (int i = 0; i < 50; i++) {
                            channel.pollWrite();
                        }
                        
                        // Longer delay to let queue drain
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                }
                
                if (!sent) {
                    std::cout << "[ERROR] Client: Failed to send message " << (sendCount + 1) << " after 10 retries" << std::endl;
                    sendCount++;  // Skip this message and continue
                }
            }
        }
        
        std::cout << "Write thread completed - sent " << successCount << "/" << kTotalMessageCount 
                  << " RDMA writes (failed: " << failCount << ")" << std::endl;
        std::cout << "[DEBUG] Client: Write thread setting done flag" << std::endl;
    });

    std::thread pushThread([&writeQueue](){
        auto const startTime = std::chrono::steady_clock::now();
        auto const sendDurationUs = kSendDurationMs * 1000;
        auto const sleepIntervalUs = sendDurationUs / kTotalMessageCount;
        
        std::cout << "Starting to send " << kTotalMessageCount << " RDMA writes over " << (static_cast<double>(kSendDurationMs) / 1000) << " seconds" << std::endl;
        
        for (size_t i=0; i<kTotalMessageCount; ++i) {
            writeQueue.push(get_timestamp_ns());
            
            auto const currentTime = std::chrono::steady_clock::now();
            auto const elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                currentTime - startTime).count());
            auto const targetElapsed = static_cast<uint64_t>(i + 1) * sleepIntervalUs;
            if (elapsed < targetElapsed) {
                std::this_thread::sleep_for(std::chrono::microseconds(targetElapsed - elapsed));
            }
        }
        
        std::cout << "Push message thread completed - pushed " << kTotalMessageCount << " messages" << std::endl;
    });
    
    pushThread.join();
    std::cout << "[DEBUG] Client: Push thread joined" << std::endl;
    writeThread.join();
    std::cout << "[DEBUG] Client: Write thread joined" << std::endl;
    done.store(true, std::memory_order_release);
    std::cout << "[DEBUG] Client: Done flag set to true" << std::endl;
    writeCompletionThread.join();
    std::cout << "[DEBUG] Client: Completion thread joined" << std::endl;
    
    std::cout << "Client completed sending all RDMA writes with immediate data" << std::endl;
    
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc == 2) {
        return serverMain(argc, argv);
    }
    else {
        return clientMain(argc, argv);
    }
}