#!/bin/bash

# Script to download libfabric-efa-demo.zip from S3 to EC2 instances via SSM
# Usage: ./download-to-instances.sh [instance-id1] [instance-id2] ...
# If no instance IDs provided, will attempt to get them from terraform output

set -e

S3_BUCKET="alb-spotweb-alb"
S3_KEY="libfabric-efa-demo.zip"
TARGET_DIR="~/"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to check if AWS CLI is available
check_aws_cli() {
    if ! command -v aws &> /dev/null; then
        log_error "AWS CLI is not installed or not in PATH"
        exit 1
    fi
}

# Function to get instance IDs from terraform output
get_terraform_instances() {
    if [ -d "terraform" ] && [ -f "terraform/terraform.tfstate" ]; then
        log_info "Getting instance IDs from terraform output..."
        cd terraform
        terraform output -json instance_ids 2>/dev/null | jq -r '.[]' 2>/dev/null || true
        cd ..
    fi
}

# Function to download file to a single instance
download_to_instance() {
    local instance_id=$1
    
    log_info "Downloading to instance: $instance_id"
    
    # Create the SSM command to download from S3
    local command="cd ~ && aws s3 cp s3://${S3_BUCKET}/${S3_KEY} ./ && echo 'Download completed successfully to \$(pwd)/${S3_KEY}'"
    
    # Execute the command via SSM
    local command_id=$(aws ssm send-command \
        --instance-ids "$instance_id" \
        --document-name "AWS-RunShellScript" \
        --parameters "commands=[\"$command\"]" \
        --query 'Command.CommandId' \
        --output text)
    
    if [ $? -eq 0 ]; then
        log_info "Command sent to $instance_id (Command ID: $command_id)"
        
        # Wait for command completion and show status
        log_info "Waiting for command completion on $instance_id..."
        
        local max_attempts=30
        local attempt=0
        
        while [ $attempt -lt $max_attempts ]; do
            local status=$(aws ssm get-command-invocation \
                --command-id "$command_id" \
                --instance-id "$instance_id" \
                --query 'Status' \
                --output text 2>/dev/null || echo "InProgress")
            
            case $status in
                "Success")
                    log_info "✓ Download completed successfully on $instance_id"
                    
                    # Get command output
                    local output=$(aws ssm get-command-invocation \
                        --command-id "$command_id" \
                        --instance-id "$instance_id" \
                        --query 'StandardOutputContent' \
                        --output text 2>/dev/null || echo "")
                    
                    if [ -n "$output" ]; then
                        echo "Output: $output"
                    fi
                    return 0
                    ;;
                "Failed")
                    log_error "✗ Download failed on $instance_id"
                    
                    # Get error details
                    local error_output=$(aws ssm get-command-invocation \
                        --command-id "$command_id" \
                        --instance-id "$instance_id" \
                        --query 'StandardErrorContent' \
                        --output text 2>/dev/null || echo "Unknown error")
                    
                    log_error "Error details: $error_output"
                    return 1
                    ;;
                "InProgress"|"Pending")
                    echo -n "."
                    sleep 2
                    ;;
                *)
                    log_warn "Unknown status: $status"
                    sleep 2
                    ;;
            esac
            
            ((attempt++))
        done
        
        log_error "Timeout waiting for command completion on $instance_id"
        return 1
    else
        log_error "Failed to send command to $instance_id"
        return 1
    fi
}

# Function to verify instances are accessible via SSM
verify_ssm_access() {
    local instance_id=$1
    
    log_info "Verifying SSM access to $instance_id..."
    
    local status=$(aws ssm describe-instance-information \
        --filters "Key=InstanceIds,Values=$instance_id" \
        --query 'InstanceInformationList[0].PingStatus' \
        --output text 2>/dev/null || echo "Unknown")
    
    if [ "$status" = "Online" ]; then
        log_info "✓ Instance $instance_id is online and accessible via SSM"
        return 0
    else
        log_error "✗ Instance $instance_id is not accessible via SSM (Status: $status)"
        log_error "  Make sure the instance has:"
        log_error "  - SSM agent installed and running"
        log_error "  - IAM role with AmazonSSMManagedInstanceCore policy"
        log_error "  - Outbound internet access or VPC endpoints for SSM"
        return 1
    fi
}

# Main execution
main() {
    log_info "Starting S3 download to EC2 instances via SSM"
    log_info "S3 Source: s3://${S3_BUCKET}/${S3_KEY}"
    log_info "Target Directory: ${TARGET_DIR}"
    
    check_aws_cli
    
    # Get instance IDs
    local instance_ids=()
    
    if [ $# -gt 0 ]; then
        # Use provided instance IDs
        instance_ids=("$@")
        log_info "Using provided instance IDs: ${instance_ids[*]}"
    else
        # Try to get from terraform
        log_info "No instance IDs provided, checking terraform output..."
        local terraform_instances=$(get_terraform_instances)
        
        if [ -n "$terraform_instances" ]; then
            readarray -t instance_ids <<< "$terraform_instances"
            log_info "Found instances from terraform: ${instance_ids[*]}"
        else
            log_error "No instance IDs provided and couldn't get them from terraform"
            log_error "Usage: $0 [instance-id1] [instance-id2] ..."
            log_error "Or run from directory with terraform/ subdirectory containing tfstate"
            exit 1
        fi
    fi
    
    if [ ${#instance_ids[@]} -eq 0 ]; then
        log_error "No instances to process"
        exit 1
    fi
    
    # Verify SSM access to all instances first
    log_info "Verifying SSM access to all instances..."
    local accessible_instances=()
    
    for instance_id in "${instance_ids[@]}"; do
        if verify_ssm_access "$instance_id"; then
            accessible_instances+=("$instance_id")
        fi
    done
    
    if [ ${#accessible_instances[@]} -eq 0 ]; then
        log_error "No instances are accessible via SSM"
        exit 1
    fi
    
    log_info "Proceeding with ${#accessible_instances[@]} accessible instances"
    
    # Download to each accessible instance
    local success_count=0
    local total_count=${#accessible_instances[@]}
    
    for instance_id in "${accessible_instances[@]}"; do
        echo
        if download_to_instance "$instance_id"; then
            ((success_count++))
        fi
    done
    
    echo
    log_info "Download Summary:"
    log_info "  Total instances: $total_count"
    log_info "  Successful downloads: $success_count"
    log_info "  Failed downloads: $((total_count - success_count))"
    
    if [ $success_count -eq $total_count ]; then
        log_info "✓ All downloads completed successfully!"
        exit 0
    else
        log_error "✗ Some downloads failed"
        exit 1
    fi
}

# Run main function with all arguments
main "$@"