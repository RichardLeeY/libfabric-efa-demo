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

// Message structure with buffer and client info
struct Message {
    char* buffer;
};

constexpr size_t kMessageSize = 64;
constexpr size_t kTotalMessageCount = 10000;
constexpr size_t kBufferPoolCapacity = 100;
constexpr size_t kBufferPoolSlots = 10;
constexpr size_t kTotalBufferPoolSize = kBufferPoolSlots * kMessageSize;
constexpr size_t kCompletionQueueReadCount = 16;

constexpr uint32_t kSendDurationMs = 10000; // 10 seconds
constexpr uint32_t kReceiveTimeoutMs = 20000; // 20 seconds
constexpr uint32_t kSpscQueueSize = 10000;

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

struct MessageData {
    uint64_t queue_timestamp;
    uint64_t send_timestamp;
    uint64_t receive_timestamp;
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

// Convert nanoseconds to microseconds for readable output
double ns_to_us(uint64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

// Debug timing helper class
class DebugTimer {
private:
    std::string name_;
    uint64_t start_time_;
    
public:
    DebugTimer(const std::string& name) : name_(name), start_time_(get_timestamp_ns()) {
        std::cout << "[DEBUG] Starting " << name_ << "..." << std::endl;
    }
    
    ~DebugTimer() {
        uint64_t elapsed = get_timestamp_ns() - start_time_;
        std::cout << "[DEBUG] " << name_ << " completed in " << ns_to_us(elapsed) << " μs" << std::endl;
    }
    
    void checkpoint(const std::string& checkpoint_name) {
        uint64_t elapsed = get_timestamp_ns() - start_time_;
        std::cout << "[DEBUG] " << name_ << " - " << checkpoint_name << ": " << ns_to_us(elapsed) << " μs" << std::endl;
    }
};

class RdmaChannel {
public:
    using RecvCallback = std::function<void(void*)>;
	using SendCallback = std::function<void(void*)>;
    
private:
    struct Endpoint {
        FabricResource<fid_cq> cq;
        FabricResource<fid_av> av;
        FabricResource<fid_ep> ep;
    };
    
    FabricResource<fi_info> info_;
    FabricResource<fid_fabric> fabric_;
    FabricResource<fid_domain> domain_;

    Endpoint sendEndpoint_;
    Endpoint recvEndpoint_;
    
    std::unordered_map<void*, FabricResource<fid_mr>> memoryRegionMap_;

    RecvCallback recvCallback_;
	SendCallback sendCallback_;
    bool sendInProgress_{false};

public:
    bool init() {
        DebugTimer timer("RDMA Channel Initialization");
        
        if (!initInfo()) {
            return false;
        }
        timer.checkpoint("Info initialization");
        
        if (!initNetwork()) {
            return false;
        }
        timer.checkpoint("Network initialization");
        
        return true;
    }

    void printInfo() {
        if (info_.get() == nullptr)
            return;

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
        if (auto ret = fi_getname(&recvEndpoint_.ep->fid, addr, &addrlen); ret != 0) {
            std::cout << "fi_getname failed: " << fi_strerror(-ret) << std::endl;
            std::exit(1);
        }

        EfaAddress address(addr);
        return address;
    }
    
	void registerSendCallback(SendCallback&& callback) {
		sendCallback_ = callback;
	}
	
    void registerRecvCallback(RecvCallback&& callback) {
        recvCallback_ = callback;
    }
    
    bool registerMemory(void* data, size_t size) {
        // DebugTimer timer("Memory Registration (" + std::to_string(size) + " bytes)"); (removed for latency measurement)
        
        FabricResource<fid_mr> resource;
        
        struct fid_mr *mr;
        struct fi_mr_attr mr_attr = {
            .iov_count = 1,
            .access = FI_REMOTE_WRITE | FI_REMOTE_READ | FI_WRITE | FI_READ,
        };
        
        struct iovec iov = {.iov_base = data, .iov_len = size};
        mr_attr.mr_iov = &iov;
        
        // timer.checkpoint("Setup attributes"); (removed for latency measurement)
        
        if (auto ret = fi_mr_regattr(domain_.get(), &mr_attr, 0, &mr); ret != 0)
        {
            std::cout << "[DEBUG] Memory registration failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        
        // timer.checkpoint("fi_mr_regattr call"); (removed for latency measurement)
        
        resource.reset(mr);
        memoryRegionMap_.emplace(data, std::move(resource));

        return true;
    }
    
     std::optional<fi_addr_t> addPeerAddress(const EfaAddress& peer_addr) {
        // DebugTimer timer("Peer Address Addition"); (removed for latency measurement)
        
        fi_addr_t addr = FI_ADDR_UNSPEC;
        if (auto ret = fi_av_insert(sendEndpoint_.av.get(), peer_addr.bytes, 1, &addr, 0, nullptr); ret != 1) {
            std::cout << "fi_av_insert failed: " << fi_strerror(-ret) << std::endl;
            return std::nullopt;
        }
        
        // timer.checkpoint("fi_av_insert completed"); (removed for latency measurement)
        return std::make_optional<fi_addr_t>(addr);
    }
    
    bool postReceive(void* mr_ptr, void* data)
    {
        if (auto iter = memoryRegionMap_.find(mr_ptr); iter != memoryRegionMap_.end())
        {
            auto& mr = iter->second;

            struct iovec iov = {
                .iov_base = data,
                .iov_len = kMessageSize,
            };
        
            struct fi_msg msg = {
                .msg_iov = &iov,
                .desc = &mr->mem_desc,
                .iov_count = 1,
                .addr = FI_ADDR_UNSPEC,
                .context = data,
            };
            
            if (auto ret = fi_recvmsg(recvEndpoint_.ep.get(), &msg, 0); ret != 0)
            {
                std::cout << "fi_recvmsg fail: " << fi_strerror(-ret) << std::endl;
                return false;
            }
            
            return true;
        }
        
        std::cout << "data pointer not found!" << std::endl;
        
        return false;
    }
    
    bool postSend(fi_addr_t addr, void* mr_ptr, void* data)
    {
        if (auto iter = memoryRegionMap_.find(mr_ptr); iter != memoryRegionMap_.end())
        {
            auto& mr = iter->second;
            
            struct iovec iov = {
                .iov_base = data,
                .iov_len = kMessageSize,
            };
            
            struct fi_msg msg = {
                .msg_iov = &iov,
                .desc = &mr->mem_desc,
                .iov_count = 1,
                .addr = addr,
                .context = data,
            };
            
            if (auto ret = fi_sendmsg(sendEndpoint_.ep.get(), &msg, 0); ret != 0)
            {
                std::cout << "fi_sendmsg fail: " << fi_strerror(-ret) << std::endl;
                return false;
            }
            
            return true;
        }

        return false;
    }

    void pollReceive() {
        pollCompletion(recvEndpoint_.cq.get());
    }
    
	void pollSend() {
		pollCompletion(sendEndpoint_.cq.get());
	}
	
private:
    bool initInfo() {
        struct fi_info *hints, *info;
        hints = fi_allocinfo();
        hints->ep_attr->type = FI_EP_RDM;
        hints->fabric_attr->prov_name = strdup("efa");
        hints->domain_attr->threading = FI_THREAD_SAFE;

        if (auto ret = fi_getinfo(FI_VERSION(2, 0), nullptr, nullptr, 0, hints, &info); ret != 0)
        {
            std::cout << "fi_getinfo failed: " << fi_strerror(-ret) << std::endl;
            fi_freeinfo(hints);
            return false;
        }

        info_.reset(info);
        fi_freeinfo(hints);
        return true;
    }
    
    bool initNetwork() {
        DebugTimer timer("Network Initialization");
        
        struct fid_fabric* fabric;
        if (auto ret = fi_fabric(info_->fabric_attr, &fabric, nullptr); ret != 0)
        {
            std::cout << "fi_fabric failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        fabric_.reset(fabric);
        timer.checkpoint("Fabric creation");

        struct fid_domain* domain;
        if (auto ret = fi_domain(fabric_.get(), info_.get(), &domain, nullptr); ret != 0)
        {
            std::cout << "fi_domain failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        domain_.reset(domain);
        timer.checkpoint("Domain creation");
        
        if (!createEndpoint(sendEndpoint_)) {
            return false;
        }
        timer.checkpoint("Send endpoint creation");
        
        if (!createEndpoint(recvEndpoint_)) {
            return false;
        }
        timer.checkpoint("Receive endpoint creation");
        
        return true;
    }
    
    bool createEndpoint(Endpoint& endpoint) {
        DebugTimer timer("Endpoint Creation");
        
        struct fid_cq* cq;
        struct fi_cq_attr cq_attr = {};
        cq_attr.format = FI_CQ_FORMAT_DATA;
        if (auto ret = fi_cq_open(domain_.get(), &cq_attr, &cq, nullptr); ret != 0)
        {
            std::cout << "fi_cq_open failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        timer.checkpoint("Completion queue creation");

        struct fid_av* av;
        struct fi_av_attr av_attr = {};
        if (auto ret = fi_av_open(domain_.get(), &av_attr, &av, nullptr); ret != 0)
        {
            std::cout << "fi_av_open failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        timer.checkpoint("Address vector creation");

        struct fid_ep *ep;
        if (auto ret = fi_endpoint(domain_.get(), info_.get(), &ep, nullptr); ret != 0)
        {
            std::cout << "fi_endpoint failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        timer.checkpoint("Endpoint creation");

        if (auto ret = fi_ep_bind(ep, &cq->fid, FI_SEND | FI_RECV); ret != 0)
        {
            std::cout << "fi_ep_bind to cq failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        timer.checkpoint("Endpoint bind to CQ");

        if (auto ret = fi_ep_bind(ep, &av->fid, 0); ret != 0)
        {
            std::cout << "fi_ep_bind to av failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        timer.checkpoint("Endpoint bind to AV");

        if (auto ret = fi_enable(ep); ret != 0)
        {
            std::cout << "fi_ep_bind to av failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        timer.checkpoint("Endpoint enable");
        
        endpoint.cq.reset(cq);
        endpoint.av.reset(av);
        endpoint.ep.reset(ep);

        return true;
    }

    
    void pollCompletion(fid_cq* cq) {
        struct fi_cq_data_entry cqe[kCompletionQueueReadCount];
        while (true) {
            auto ret = fi_cq_read(cq, cqe, kCompletionQueueReadCount);
            if (ret > 0)
            {
                for (uint32_t i=0; i<ret; ++i) {
                    handleCompletion(cqe[i]);
                }
            }
            else if (ret == -FI_EAVAIL) {
                struct fi_cq_err_entry err_entry;
                ret = fi_cq_readerr(cq, &err_entry, 0);
                if (ret < 0) {
                    fprintf(stderr, "fi_cq_readerr error: %zd (%s)\n", ret, fi_strerror(-ret));
                    std::exit(1);
                }
                else if (ret > 0) {
                    fprintf(stderr, "Failed libfabric operation: %s\n",
                        fi_cq_strerror(cq, err_entry.prov_errno, err_entry.err_data, nullptr, 0));
                }
                else {
                    fprintf(stderr, "fi_cq_readerr returned 0 unexpectedly.\n");
                    std::exit(1);
                }
            }
            else if (ret == -FI_EAGAIN) {
                // No more completions
                break;
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
        if (data == nullptr)
            return;
        
        if (comp_flags & FI_RECV) {
            recvCallback_(data);
        }
        else if (comp_flags & FI_SEND) {
            // TODO: may enhance this when support concurrent send
			sendCallback_(data);
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
    
    void sendAddressToPeer()
    {
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            std::cerr << "Error creating socket" << std::endl;
            return;
        }
        sendto(sockfd, selfAddr_.bytes, sizeof(selfAddr_.bytes),  0, (struct sockaddr*)&peerSockAddr_, sizeof(peerSockAddr_));      
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
            std::cerr << "Error binding socket" << std::endl;
            close(sockfd);
            return;
        }

        std::cout << "Listening peer EFA address on port: " << port << std::endl;

        uint8_t buffer[64];
        socklen_t server_len = sizeof(peerSockAddr_);
        int bytes_received = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr*)&peerSockAddr_, &server_len);
        if (bytes_received != 32) {
            std::cout << "Received invalid EFA address, message length = " << bytes_received << std::endl;
            std::exit(1);
        }
        
        peerAddr_ = EfaAddress(buffer);
        std::cout << "Peer EFA address = " << peerAddr_.toString() << std::endl;
        
        peerSockAddr_.sin_port = htons(port);
    }

    EfaAddress selfAddr_;
    EfaAddress peerAddr_;
    struct sockaddr_in peerSockAddr_;
};

template <typename T, size_t Size>
class SpscQueue {
private:
    std::array<T, Size+1> buffer_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};

    static constexpr size_t CACHE_LINE_SIZE = 64;
    char padding1_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];
    char padding2_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];

public:
    bool push(const T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % (Size+1);

        if (next_tail == head_.load(std::memory_order_acquire)) {
            // Queue is full
            return false;
        }

        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t current_head = head_.load(std::memory_order_relaxed);

        if (current_head == tail_.load(std::memory_order_acquire)) {
            // Queue is empty
            return false;
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

template<size_t MessageCount>
class MessageBufferPool {
private:
    Buffer buffer_pool_;
    size_t message_size_;
	SpscQueue<void*, MessageCount> free_buffer_queue_;
    
public:
    MessageBufferPool(size_t message_size)
        : buffer_pool_(MessageCount * message_size)
        , message_size_(message_size)
    {
		auto data = static_cast<char*>(buffer_pool_.data());
		for (size_t i=0; i<MessageCount; ++i) {
			auto data_slot_ptr = data + i * message_size_;
			free_buffer_queue_.push(data_slot_ptr);
		}
    }
	
	void* getNext() {
		void* data = nullptr;
		if (!free_buffer_queue_.pop(data)) {
			return nullptr; // Return null instead of blocking
		}
		return data;
	}
	
	void freeMessage(void* data) {
		free_buffer_queue_.push(data);
	}
	
	void* data() const {
		return buffer_pool_.data();
	}
	
    size_t size() const {
        return buffer_pool_.size();
    }
    
	size_t capacity() const {
		return MessageCount;
	}
};


// Get human readable timestamp
std::string get_readable_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

void calculate_statistics(const std::vector<MessageData>& message_stats) {
    std::cout << "\n--- Statistics ---\n";
    std::cout << "Total messages received: " << message_stats.size() << " out of "
              << kTotalMessageCount << std::endl;

    if (message_stats.empty()) {
        std::cout << "No statistics available - no messages received." << std::endl;
        return;
    }

    std::vector<uint64_t> qts;
    std::vector<uint64_t> rtts;
    rtts.reserve(message_stats.size());

    // Track high latency messages for debugging
    std::vector<std::pair<size_t, uint64_t>> high_latency_messages;
    const uint64_t HIGH_LATENCY_THRESHOLD_NS = 10000000; // 10ms in nanoseconds

    for (size_t i = 0; i < message_stats.size(); ++i) {
        const auto& data = message_stats[i];
        uint64_t qt = data.send_timestamp - data.queue_timestamp;
        uint64_t rtt = data.receive_timestamp - data.send_timestamp;
        qts.push_back(qt);
        rtts.push_back(rtt);
        
        // Track high latency messages
        if (rtt > HIGH_LATENCY_THRESHOLD_NS) {
            high_latency_messages.emplace_back(i, rtt);
        }
    }

    std::sort(rtts.begin(), rtts.end());
    std::sort(qts.begin(), qts.end());

    std::cout << std::fixed << std::setprecision(3);

    // Queue Time Statistics
    std::cout << "Internal Delay Time Statistics:" << std::endl;
    std::cout << "  Min: " << (static_cast<double>(qts.front()) / 1000.0) << " μs" << std::endl;
    std::cout << "  Max: " << (static_cast<double>(qts.back()) / 1000.0) << " μs" << std::endl;
    std::cout << "  P25: " << (static_cast<double>(qts[qts.size() * 25 / 100]) / 1000.0) << " μs" << std::endl;
    std::cout << "  P50: " << (static_cast<double>(qts[qts.size() * 50 / 100]) / 1000.0) << " μs" << std::endl;
    std::cout << "  P75: " << (static_cast<double>(qts[qts.size() * 75 / 100]) / 1000.0) << " μs" << std::endl;
    std::cout << "  P90: " << (static_cast<double>(qts[qts.size() * 90 / 100]) / 1000.0) << " μs" << std::endl;
    std::cout << "  P99: " << (static_cast<double>(qts[qts.size() * 99 / 100]) / 1000.0) << " μs" << std::endl;

    // RTT Statistics
    std::cout << "Round Trip Time (RTT) Statistics:" << std::endl;
    std::cout << "  Min: " << (static_cast<double>(rtts.front()) / 1000.0) << " μs" << std::endl;
    std::cout << "  Max: " << (static_cast<double>(rtts.back()) / 1000.0) << " μs" << std::endl;
    std::cout << "  P25: " << (static_cast<double>(rtts[rtts.size() * 25 / 100]) / 1000.0) << " μs" << std::endl;
    std::cout << "  P50: " << (static_cast<double>(rtts[rtts.size() * 50 / 100]) / 1000.0) << " μs" << std::endl;
    std::cout << "  P75: " << (static_cast<double>(rtts[rtts.size() * 75 / 100]) / 1000.0) << " μs" << std::endl;
    std::cout << "  P90: " << (static_cast<double>(rtts[rtts.size() * 90 / 100]) / 1000.0) << " μs" << std::endl;
    std::cout << "  P99: " << (static_cast<double>(rtts[rtts.size() * 99 / 100]) / 1000.0) << " μs" << std::endl;

    // Debug: Print high latency messages
    if (!high_latency_messages.empty()) {
        std::cout << "\n--- High Latency Messages (>10ms) ---" << std::endl;
        for (const auto& [msg_idx, rtt] : high_latency_messages) {
            const auto& msg = message_stats[msg_idx];
            std::cout << "Message #" << (msg_idx + 1) << ":" << std::endl;
            std::cout << "  Queue Time: " << ns_to_us(msg.queue_timestamp) << " μs" << std::endl;
            std::cout << "  Send Time: " << ns_to_us(msg.send_timestamp) << " μs" << std::endl;
            std::cout << "  Receive Time: " << ns_to_us(msg.receive_timestamp) << " μs" << std::endl;
            std::cout << "  RTT: " << ns_to_us(rtt) << " μs" << std::endl;
            std::cout << "  Internal Delay: " << ns_to_us(msg.send_timestamp - msg.queue_timestamp) << " μs" << std::endl;
        }
    }
}

struct ReturnMessage {
	uint64_t queueTs;
	uint64_t sendTs;
};

int serverMain(int argc, char *argv[]) {
    if (argc != 2) {
        std::cout << "Server mode usage: rdma_pingpong <communication port>" << std::endl;
        return -1;
    }

    auto port = std::stoi(argv[1]);
    
    // Initialize lib-fabric
    DebugTimer init_timer("Server Initialization");
    RdmaChannel channel;
    if (!channel.init())
        return -1;
    init_timer.checkpoint("RDMA channel initialization");

    channel.printInfo();

    // Exchange EFA address before proceeding
    EfaAddressExchange exchange{channel.getSelfAddress()};
    exchange.waitForPeer(port);
    init_timer.checkpoint("Address exchange");
    
    // Add peer address
    auto server_addr = channel.addPeerAddress(exchange.getPeerAddress());
    if (!server_addr) {
        return -1;
    }
    init_timer.checkpoint("Peer address addition");
    
    // Allocate send/receiver buffers
    auto sendBufferPool = MessageBufferPool<kBufferPoolCapacity>(kMessageSize);
	auto recvBufferPool = MessageBufferPool<kBufferPoolCapacity>(kMessageSize);
    init_timer.checkpoint("Buffer pool allocation");
    
    // register memory
    channel.registerMemory(sendBufferPool.data(), sendBufferPool.size());
	channel.registerMemory(recvBufferPool.data(), recvBufferPool.size());
    init_timer.checkpoint("Memory registration");
    
    for (size_t i=0; i<recvBufferPool.capacity(); ++i) {
		auto data = recvBufferPool.getNext();
        channel.postReceive(recvBufferPool.data(), static_cast<void*>(data));
    }
    init_timer.checkpoint("Initial receive buffer posting");
    
    std::cout << "[DEBUG] Server initialization complete, ready to receive messages" << std::endl;
    
	// Start send thread
    SpscQueue<ReturnMessage, kSpscQueueSize> sendQueue;
    std::thread sendThread([&channel, &sendQueue, &sendBufferPool, &server_addr]() {
        std::cout << "Started send thread" << std::endl;
        uint32_t sendCount = 0;
        bool first_send = true;
        
        while (sendCount < kTotalMessageCount) {
            ReturnMessage message;
            if (sendQueue.pop(message)) {
                auto data = static_cast<uint8_t*>(sendBufferPool.getNext());
		if (!data) {
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                    continue;
                }
                
                // Add timing for first send operation
                std::unique_ptr<DebugTimer> first_send_timer;
                if (first_send) {
                    first_send_timer = std::make_unique<DebugTimer>("First Server Send Operation");
                }
                
                uint64_t server_send_ts = get_timestamp_ns();
                (void)server_send_ts; // Suppress unused variable warning
                std::memcpy(data, &message.queueTs, sizeof(message.queueTs));
                std::memcpy(data + sizeof(message.queueTs), &message.sendTs, sizeof(message.sendTs));
                
                if (first_send) {
                    first_send_timer->checkpoint("Message preparation");
                }
                
                // Debug: Log server send timing (removed for latency measurement)
                
                if (!channel.postSend(server_addr.value(), sendBufferPool.data(), data)) {
                    std::cout << "Error posting data, stop sending" << std::endl;
                    return;
                }
                
                if (first_send) {
                    first_send_timer->checkpoint("fi_sendmsg call");
                    first_send = false;
                }
                
                sendCount++;
            }
        }
        
        std::cout << "Send thread completed - sent " << kTotalMessageCount << " messages" << std::endl;
    });
	
	// Start send completion thread
	uint32_t sendCount = 0;
	channel.registerSendCallback([&](void* data){
		sendBufferPool.freeMessage(data);
		++sendCount;
		//std::cout << "success: " << sendCount << std::endl;
	});
	
	std::thread sendCompletionThread([&sendCount, &channel]() {
		while (sendCount < kTotalMessageCount) {
			channel.pollSend();
		}
	});
	
	// Start receive thread
    uint32_t count = 0;
    bool first_receive = true;
    channel.registerRecvCallback([&](void* data){
        std::unique_ptr<DebugTimer> first_recv_timer;
        if (first_receive) {
            first_recv_timer = std::make_unique<DebugTimer>("First Server Receive Operation");
        }
        
        uint64_t receiveTs = get_timestamp_ns();
        (void)receiveTs; // Suppress unused variable warning
        uint64_t queueTs = 0;
        uint64_t sendTs = 0;
        std::memcpy(&queueTs, data, sizeof(queueTs));
        std::memcpy(&sendTs, static_cast<uint8_t*>(data) + sizeof(queueTs), sizeof(sendTs));
		
        if (first_receive) {
            first_recv_timer->checkpoint("Message parsing");
        }
        
        // Debug: Log message receive with timing (removed for latency measurement)
        
        if (++count < kTotalMessageCount)
        {
            channel.postReceive(recvBufferPool.data(), data);
        }
        
        if (first_receive) {
            first_recv_timer->checkpoint("Buffer reposting");
            first_receive = false;
        }
        
		sendQueue.push(ReturnMessage{ .queueTs = queueTs, .sendTs = sendTs });
    });

    std::thread recvThread([&] {
        while (count < kTotalMessageCount)
        {
            channel.pollReceive();
        }
    });
    
    recvThread.join();
	sendThread.join();
	sendCompletionThread.join();
    
    std::cout << "Done receiving " << kTotalMessageCount << " messages" << std::endl;
    
    return 0;
}

int clientMain(int argc, char *argv[]) {
    if (argc != 3) {
        std::cout << "Client mode usage: rdma_pingpong <remote ip> <communication port>" << std::endl;
        return -1;
    }
    
    auto ip = std::string(argv[1]);
    auto port = std::stoi(argv[2]);
    
    // Initialize lib-fabric
    DebugTimer init_timer("Client Initialization");
    RdmaChannel channel;
    if (!channel.init())
        return -1;
    init_timer.checkpoint("RDMA channel initialization");

    EfaAddressExchange exchange(channel.getSelfAddress());
    exchange.sendToPeer(ip, port);
    init_timer.checkpoint("Address exchange");

    auto server_addr = channel.addPeerAddress(exchange.getPeerAddress());
    if (!server_addr) {
        return -1;
    }
    init_timer.checkpoint("Peer address addition");

    // Allocate send/receiver buffers
    auto sendBufferPool = MessageBufferPool<kBufferPoolCapacity>(kMessageSize);
	auto recvBufferPool = MessageBufferPool<kBufferPoolCapacity>(kMessageSize);
    init_timer.checkpoint("Buffer pool allocation");
    
    // register memory
    channel.registerMemory(sendBufferPool.data(), sendBufferPool.size());
	channel.registerMemory(recvBufferPool.data(), recvBufferPool.size());
    init_timer.checkpoint("Memory registration");
    
    for (size_t i=0; i<recvBufferPool.capacity(); ++i) {
		auto data = recvBufferPool.getNext();
        channel.postReceive(recvBufferPool.data(), static_cast<void*>(data));
    }
    init_timer.checkpoint("Initial receive buffer posting");
    
    std::cout << "[DEBUG] Client initialization complete, ready to send messages" << std::endl;

    // Start receive thread
    std::atomic_bool done = false;
    std::vector<MessageData> message_stats;
    message_stats.reserve(kTotalMessageCount);
    bool first_receive = true;
    channel.registerRecvCallback([&](void* data){
        std::unique_ptr<DebugTimer> first_recv_timer;
        if (first_receive) {
            first_recv_timer = std::make_unique<DebugTimer>("First Client Receive Operation");
        }
        
        uint64_t queueTs = 0;
        uint64_t sendTs = 0;
        uint64_t receiveTs = get_timestamp_ns();
        std::memcpy(&queueTs, data, sizeof(queueTs));
        std::memcpy(&sendTs, static_cast<uint8_t*>(data) + sizeof(queueTs), sizeof(sendTs));
        
        if (first_receive) {
            first_recv_timer->checkpoint("Message parsing");
        }
        
        // Debug: Calculate and log timing details (removed for latency measurement)

        // Store results
        MessageData msg{queueTs, sendTs, receiveTs};
        message_stats.push_back(msg);
        
        if (message_stats.size() < kTotalMessageCount)
        {
            channel.postReceive(recvBufferPool.data(), static_cast<void*>(data));
        }
        else {
            done.store(true, std::memory_order_release);
        }
        
        if (first_receive) {
            first_recv_timer->checkpoint("Statistics and buffer reposting");
            first_receive = false;
        }
    });
    
    std::thread recvThread([&done, &channel]() {
        while (!done.load(std::memory_order_acquire)) {
            channel.pollReceive();
        }
    });
	
	// Start send completion thread
	channel.registerSendCallback([&](void* data){
		sendBufferPool.freeMessage(data);
	});
	
	std::thread sendCompletionThread([&done, &channel]() {
		while (!done.load(std::memory_order_acquire)) {
			channel.pollSend();
		}
	});

    // Start send thread
    SpscQueue<uint64_t, kSpscQueueSize> sendQueue;
    std::thread sendThread([&done, &channel, &sendQueue, &sendBufferPool, &server_addr]() {
        std::cout << "Started send thread" << std::endl;
        uint32_t sendCount = 0;
        bool first_send = true;
        
        while (sendCount < kTotalMessageCount) {
            uint64_t queueTs = 0;
            if (sendQueue.pop(queueTs)) {
                auto data = static_cast<uint8_t*>(sendBufferPool.getNext());
		if (!data) {
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                    continue;
                }
                
                // Add timing for first send operation
                std::unique_ptr<DebugTimer> first_send_timer;
                if (first_send) {
                    first_send_timer = std::make_unique<DebugTimer>("First Send Operation");
                }
                
                auto sendTs = get_timestamp_ns();
                std::memcpy(data, &queueTs, sizeof(queueTs));
                std::memcpy(data + sizeof(queueTs), &sendTs, sizeof(sendTs));
                
                if (first_send) {
                    first_send_timer->checkpoint("Message preparation");
                }
                
                // Debug: Log client send timing (removed for latency measurement)
                
                if (!channel.postSend(server_addr.value(), sendBufferPool.data(), data)) {
                    std::cout << "Error posting data, stop sending" << std::endl;
                    return;
                }
                
                if (first_send) {
                    first_send_timer->checkpoint("fi_sendmsg call");
                    first_send = false;
                }
                
                sendCount++;
            }
        }
        
        std::cout << "Send thread completed - sent " << kTotalMessageCount << " messages" << std::endl;
    });

    // Start push thread
    std::thread pushThread([&sendQueue](){
        auto const startTime = std::chrono::steady_clock::now();
        auto const sendDurationUs = kSendDurationMs * 1000;
        auto const sleepIntervalUs = sendDurationUs / kTotalMessageCount;
        
        std::cout << "Starting to send " << kTotalMessageCount << " messages over " << (static_cast<double>(kSendDurationMs) / 1000) << " seconds" << std::endl;
        
        for (size_t i=0; i<kTotalMessageCount; ++i) {
            sendQueue.push(get_timestamp_ns());
            
            // Sleep to pace the messages evenly
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
    sendThread.join();
    recvThread.join();
	sendCompletionThread.join();
    
    calculate_statistics(message_stats);
    
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

