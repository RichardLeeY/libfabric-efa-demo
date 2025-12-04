#!/bin/bash
# RDMA Development Helper Script

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
echo_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
echo_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Function to check EFA configuration
check_efa() {
    echo_info "Checking EFA configuration..."
    
    # Check if fi_info is available
    if ! command -v fi_info &> /dev/null; then
        echo_error "fi_info not found. Install libfabric-dev"
        return 1
    fi
    
    # Check EFA provider
    if fi_info -p efa &> /dev/null; then
        echo_info "✅ EFA provider available"
    else
        echo_warn "❌ EFA provider not available"
    fi
    
    # Check environment variables
    echo_info "Environment variables:"
    echo "  FI_EFA_USE_DEVICE_RDMA: ${FI_EFA_USE_DEVICE_RDMA:-'Not set'}"
    echo "  FI_PROVIDER: ${FI_PROVIDER:-'Not set'}"
}

# Function to analyze RDMA code for blocking issues
analyze_code() {
    local file="$1"
    if [[ -z "$file" ]]; then
        echo_error "Usage: $0 analyze <cpp_file>"
        return 1
    fi
    
    echo_info "Analyzing $file for RDMA issues..."
    
    # Check for blocking patterns
    if grep -q "fi_sendmsg" "$file" && ! grep -q "FI_EAGAIN" "$file"; then
        echo_warn "⚠️  Missing -FI_EAGAIN handling in fi_sendmsg"
    fi
    
    if grep -q "return false" "$file" && grep -q "fi_sendmsg" "$file"; then
        echo_warn "⚠️  Early return on send failure may block thread"
    fi
    
    # Check completion handling
    if grep -q "fi_cq_read" "$file" && ! grep -q "FI_EAVAIL" "$file"; then
        echo_warn "⚠️  Missing error completion handling"
    fi
    
    echo_info "Analysis complete"
}

# Function to build with proper flags
build_project() {
    echo_info "Building RDMA project..."
    
    # Set optimal environment
    export FI_EFA_USE_DEVICE_RDMA=1
    export FI_PROVIDER=efa
    
    if [[ -f "Makefile" ]]; then
        make clean && make
        echo_info "✅ Build complete"
    else
        echo_error "No Makefile found"
        return 1
    fi
}

# Function to run quick diagnostics
diagnose() {
    echo_info "Running RDMA diagnostics..."
    
    echo_info "1. EFA Provider Status:"
    fi_info -p efa | head -10 || echo_warn "EFA not available"
    
    echo_info "2. Available Providers:"
    fi_info -l | grep -E "(efa|verbs|shm)" || echo_warn "No RDMA providers found"
    
    echo_info "3. Code Issues:"
    find src/ -name "*.cpp" -exec grep -l "fi_sendmsg" {} \; | while read -r file; do
        if ! grep -q "FI_EAGAIN" "$file"; then
            echo_warn "  $file: Missing -FI_EAGAIN handling"
        fi
    done
}

# Main command dispatcher
case "$1" in
    "check"|"efa")
        check_efa
        ;;
    "analyze")
        analyze_code "$2"
        ;;
    "build")
        build_project
        ;;
    "diagnose"|"diag")
        diagnose
        ;;
    *)
        echo "RDMA Development Helper"
        echo "Usage: $0 {check|analyze <file>|build|diagnose}"
        echo ""
        echo "Commands:"
        echo "  check     - Check EFA configuration"
        echo "  analyze   - Analyze C++ file for RDMA issues"
        echo "  build     - Build project with optimal settings"
        echo "  diagnose  - Run comprehensive diagnostics"
        ;;
esac
