#include <iostream>
#include <cstring>
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

constexpr size_t kMessageSize = 64;
constexpr size_t kTotalMessageCount = 10;
constexpr size_t kBufferSize = 1024 * 1024; // 1MB buffer

struct EfaAddress {
    uint8_t bytes[32];
    
    std::string toString() const {
        char buf[65];
        for (size_t i = 0; i < 32; i++) {
            snprintf(buf + 2 * i, 3, "%02x", bytes[i]);
        }
        return std::string(buf, 64);
    }
    
    static EfaAddress parse(const std::string &str) {
        EfaAddress addr;
        for (size_t i = 0; i < 32; i++) {
            sscanf(str.c_str() + 2 * i, "%02hhx", &addr.bytes[i]);
        }
        return addr;
    }
};

template<typename T>
class FabricResource {
    T* resource_;
public:
    FabricResource() : resource_(nullptr) {}
    explicit FabricResource(T* resource) : resource_(resource) {}
    ~FabricResource() {
        if (resource_) {
            if constexpr (std::is_same_v<T,fi_info>) {
                fi_freeinfo(resource_);
            } else {
                fi_close(&resource_->fid);
            }
        }
    }
    T* get() const { return resource_; }
    void reset(T* resource) {
        if (resource_) {
            if constexpr (std::is_same_v<T, fi_info>) {
                fi_freeinfo(resource_);
            } else {
                fi_close(&resource_->fid);
            }
        }
        resource_ = resource;
    }
    T* operator->() const { return resource_; }
    operator bool() const { return resource_ != nullptr; }
    FabricResource(const FabricResource&) = delete;
    FabricResource& operator=(const FabricResource&) = delete;
};

class RdmaChannel {
    FabricResource<fi_info> info_;
    FabricResource<fid_fabric> fabric_;
    FabricResource<fid_domain> domain_;
    FabricResource<fid_cq> cq_;
    FabricResource<fid_av> av_;
    FabricResource<fid_ep> ep_;
    FabricResource<fid_mr> mr_;
    
    void* buffer_;
    size_t buffer_size_;
    uint64_t remote_addr_;
    uint64_t remote_key_;
    
public:
    RdmaChannel() : buffer_(nullptr), buffer_size_(0), remote_addr_(0), remote_key_(0) {}
    
    ~RdmaChannel() {
        if (buffer_) std::free(buffer_);
    }
    
    bool init() {
        struct fi_info *hints = fi_allocinfo();
        hints->ep_attr->type = FI_EP_RDM;
        hints->fabric_attr->prov_name = strdup("efa");
        hints->fabric_attr->name = strdup("efa-direct");
        hints->mode = FI_CONTEXT2;
        hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY;
        hints->caps = FI_MSG | FI_RMA;
        
        struct fi_info *info;
        if (auto ret = fi_getinfo(FI_VERSION(2, 1), nullptr, nullptr, 0, hints, &info); ret != 0) {
            std::cout << "fi_getinfo failed: " << fi_strerror(-ret) << std::endl;
            fi_freeinfo(hints);
            return false;
        }
        fi_freeinfo(hints);
        info_.reset(info);
        
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
        
        struct fid_cq* cq;
        struct fi_cq_attr cq_attr = {};
        cq_attr.format = FI_CQ_FORMAT_DATA;
        if (auto ret = fi_cq_open(domain_.get(), &cq_attr, &cq, nullptr); ret != 0) {
            std::cout << "fi_cq_open failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        cq_.reset(cq);
        
        struct fid_av* av;
        struct fi_av_attr av_attr = {};
        if (auto ret = fi_av_open(domain_.get(), &av_attr, &av, nullptr); ret != 0) {
            std::cout << "fi_av_open failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        av_.reset(av);
        
        struct fid_ep *ep;
        if (auto ret = fi_endpoint(domain_.get(), info_.get(), &ep, nullptr); ret != 0) {
            std::cout << "fi_endpoint failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        ep_.reset(ep);
        
        if (auto ret = fi_ep_bind(ep, &cq_->fid, FI_TRANSMIT | FI_RECV); ret != 0) {
            std::cout << "fi_ep_bind cq failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        
        if (auto ret = fi_ep_bind(ep, &av_->fid, 0); ret != 0) {
            std::cout << "fi_ep_bind av failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        
        if (auto ret = fi_enable(ep); ret != 0) {
            std::cout << "fi_enable failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        
        return true;
    }
    
    EfaAddress getSelfAddress() {
        uint8_t addr[64];
        size_t addrlen = sizeof(addr);
        fi_getname(&ep_->fid, addr, &addrlen);
        EfaAddress address;
        memcpy(address.bytes, addr, 32);
        return address;
    }
    
    fi_addr_t addPeerAddress(const EfaAddress& peer_addr) {
        fi_addr_t addr = FI_ADDR_UNSPEC;
        fi_av_insert(av_.get(), peer_addr.bytes, 1, &addr, 0, nullptr);
        return addr;
    }
    
    bool allocateBuffer(size_t size) {
        buffer_size_ = size;
        buffer_ = std::malloc(size);
        std::memset(buffer_, 0, size);
        
        struct fid_mr *mr;
        struct fi_mr_attr mr_attr = {};
        mr_attr.iov_count = 1;
        mr_attr.access = FI_SEND | FI_RECV | FI_REMOTE_WRITE | FI_REMOTE_READ | FI_WRITE | FI_READ;
        struct iovec iov = {.iov_base = buffer_, .iov_len = size};
        mr_attr.mr_iov = &iov;
        
        if (auto ret = fi_mr_regattr(domain_.get(), &mr_attr, 0, &mr); ret != 0) {
            std::cout << "fi_mr_regattr failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        mr_.reset(mr);
        return true;
    }
    
    void* getBuffer() const { return buffer_; }
    uint64_t getBufferAddr() const { return (uint64_t)buffer_; }
    uint64_t getBufferKey() const { return fi_mr_key(mr_.get()); }
    
    void setRemoteBuffer(uint64_t addr, uint64_t key) {
        remote_addr_ = addr;
        remote_key_ = key;
    }
    
    bool rdmaWrite(fi_addr_t dest_addr, void* local_buf, size_t len, uint64_t remote_offset) {
        struct iovec iov = {.iov_base = local_buf, .iov_len = len};
        struct fi_rma_iov rma_iov = {
            .addr = remote_addr_ + remote_offset,
            .len = len,
            .key = remote_key_
        };
        void* desc = fi_mr_desc(mr_.get());
        struct fi_msg_rma msg = {
            .msg_iov = &iov,
            .desc = &desc,
            .iov_count = 1,
            .addr = dest_addr,
            .rma_iov = &rma_iov,
            .rma_iov_count = 1,
            .context = local_buf,
        };
        
        if (auto ret = fi_writemsg(ep_.get(), &msg, 0); ret != 0) {
            std::cout << "fi_writemsg failed: " << fi_strerror(-ret) << std::endl;
            return false;
        }
        return true;
    }
    
    void pollCompletion() {
        struct fi_cq_data_entry cqe;
        auto ret = fi_cq_read(cq_.get(), &cqe, 1);
        if (ret == -FI_EAGAIN) return;
        if (ret < 0) {
            std::cout << "fi_cq_read error: " << fi_strerror(-ret) << std::endl;
        }
    }
};

void exchangeAddress(const EfaAddress& self, EfaAddress& peer, const std::string& ip, uint16_t port, bool is_server) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr, peer_addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (is_server) {
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
        std::cout << "Listening on port " << port << std::endl;
        
        uint8_t buffer[64];
        socklen_t len = sizeof(peer_addr);
        recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr*)&peer_addr, &len);
        memcpy(peer.bytes, buffer, 32);
        
        sendto(sockfd, self.bytes, 32, 0, (struct sockaddr*)&peer_addr, len);
    } else {
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        sendto(sockfd, self.bytes, 32, 0, (struct sockaddr*)&addr, sizeof(addr));
        
        uint8_t buffer[64];
        socklen_t len = sizeof(peer_addr);
        recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr*)&peer_addr, &len);
        memcpy(peer.bytes, buffer, 32);
    }
    close(sockfd);
}

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
        int client = accept(sockfd, nullptr, nullptr);
        
        send(client, &local_addr, sizeof(local_addr), 0);
        send(client, &local_key, sizeof(local_key), 0);
        recv(client, &remote_addr, sizeof(remote_addr), 0);
        recv(client, &remote_key, sizeof(remote_key), 0);
        close(client);
    } else {
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
        
        recv(sockfd, &remote_addr, sizeof(remote_addr), 0);
        recv(sockfd, &remote_key, sizeof(remote_key), 0);
        send(sockfd, &local_addr, sizeof(local_addr), 0);
        send(sockfd, &local_key, sizeof(local_key), 0);
    }
    close(sockfd);
}

int serverMain(int argc, char *argv[]) {
    if (argc != 2) {
        std::cout << "Server usage: rdma_direct_write <port>" << std::endl;
        return -1;
    }
    
    auto port = std::stoi(argv[1]);
    
    RdmaChannel channel;
    if (!channel.init()) return -1;
    if (!channel.allocateBuffer(kBufferSize)) return -1;
    
    EfaAddress self_addr = channel.getSelfAddress();
    EfaAddress peer_addr;
    exchangeAddress(self_addr, peer_addr, "", port, true);
    
    channel.addPeerAddress(peer_addr);
    
    uint64_t remote_addr, remote_key;
    exchangeBufferInfo(channel.getBufferAddr(), channel.getBufferKey(), remote_addr, remote_key, "", port, true);
    
    std::cout << "Server ready. Waiting for RDMA writes..." << std::endl;
    
    auto* buffer = static_cast<uint64_t*>(channel.getBuffer());
    uint32_t count = 0;
    
    while (count < kTotalMessageCount) {
        if (buffer[0] > count) {
            count = buffer[0];
            std::cout << "Received message " << count << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::cout << "Server done" << std::endl;
    return 0;
}

int clientMain(int argc, char *argv[]) {
    if (argc != 3) {
        std::cout << "Client usage: rdma_direct_write <server_ip> <port>" << std::endl;
        return -1;
    }
    
    auto ip = std::string(argv[1]);
    auto port = std::stoi(argv[2]);
    
    RdmaChannel channel;
    if (!channel.init()) return -1;
    if (!channel.allocateBuffer(kBufferSize)) return -1;
    
    EfaAddress self_addr = channel.getSelfAddress();
    EfaAddress peer_addr;
    exchangeAddress(self_addr, peer_addr, ip, port, false);
    
    fi_addr_t server_addr = channel.addPeerAddress(peer_addr);
    
    uint64_t remote_addr, remote_key;
    exchangeBufferInfo(channel.getBufferAddr(), channel.getBufferKey(), remote_addr, remote_key, ip, port, false);
    channel.setRemoteBuffer(remote_addr, remote_key);
    
    std::cout << "Client ready. Starting RDMA writes..." << std::endl;
    
    auto* buffer = static_cast<uint64_t*>(channel.getBuffer());
    
    for (uint32_t i = 1; i <= kTotalMessageCount; i++) {
        buffer[0] = i;
        
        if (!channel.rdmaWrite(server_addr, buffer, kMessageSize, 0)) {
            std::cout << "RDMA write failed" << std::endl;
            return -1;
        }
        
        channel.pollCompletion();
        std::cout << "Sent message " << i << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    std::cout << "Client done" << std::endl;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc == 2) {
        return serverMain(argc, argv);
    } else {
        return clientMain(argc, argv);
    }
}
