# Infrastructure & Deployment

## Instance Access

### AWS Systems Manager (SSM)

**Always use SSM for EC2 instance access** instead of SSH when possible.

#### Benefits
- No need to manage SSH keys
- No need to open SSH ports (port 22) in security groups
- Centralized access logging and auditing
- Works with private instances without public IPs

#### Access Pattern

```bash
# Start SSM session
aws ssm start-session --target <instance-id>

# Run single command
aws ssm send-command \
  --instance-ids <instance-id> \
  --document-name "AWS-RunShellScript" \
  --parameters 'commands=["cd /path/to/repo && bash script.sh"]'

# Copy files to instance
aws ssm start-session --target <instance-id> \
  --document-name AWS-StartPortForwardingSession \
  --parameters '{"portNumber":["22"],"localPortNumber":["9999"]}'
```

#### Prerequisites
- EC2 instances must have SSM agent installed (pre-installed on Amazon Linux 2/2023)
- Instance IAM role must have `AmazonSSMManagedInstanceCore` policy
- Instance must have outbound internet access (or VPC endpoints for SSM)

## Infrastructure as Code

### Terraform for Complex RDMA Test Environments

**Use Terraform to provision AWS infrastructure** for RDMA testing scenarios.

#### When to Use Terraform
- Multi-instance RDMA test topologies
- Complex networking setups (VPCs, subnets, placement groups)
- EFA-enabled instance clusters
- Reproducible test environments
- Infrastructure that needs version control

#### Terraform Structure

```
terraform/
├── main.tf           # Main infrastructure definition
├── variables.tf      # Input variables
├── outputs.tf        # Output values (instance IPs, IDs)
├── efa.tf           # EFA-specific resources
└── security.tf      # Security groups and IAM roles
```

#### Key Resources for RDMA Testing

**EC2 Instances**
```hcl
resource "aws_instance" "rdma_node" {
  ami           = data.aws_ami.amazon_linux_2023.id
  instance_type = "c5n.18xlarge"  # EFA-enabled instance type
  
  network_interface {
    network_interface_id = aws_network_interface.efa.id
    device_index         = 0
  }
  
  iam_instance_profile = aws_iam_instance_profile.ssm_profile.name
  
  user_data = file("${path.module}/scripts/install-deps.sh")
  
  tags = {
    Name = "rdma-test-node"
  }
}
```

**EFA Network Interface**
```hcl
resource "aws_network_interface" "efa" {
  subnet_id       = aws_subnet.private.id
  security_groups = [aws_security_group.efa.id]
  
  interface_type = "efa"
  
  tags = {
    Name = "efa-interface"
  }
}
```

**Placement Group** (for low latency)
```hcl
resource "aws_placement_group" "rdma_cluster" {
  name     = "rdma-cluster"
  strategy = "cluster"
}
```

**Security Group** (EFA traffic)
```hcl
resource "aws_security_group" "efa" {
  name        = "efa-rdma-sg"
  description = "Allow EFA traffic between instances"
  vpc_id      = aws_vpc.main.id
  
  # Allow all traffic within security group for EFA
  ingress {
    from_port = 0
    to_port   = 0
    protocol  = "-1"
    self      = true
  }
  
  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
}
```

**IAM Role for SSM**
```hcl
resource "aws_iam_role" "ssm_role" {
  name = "ec2-ssm-role"
  
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Action = "sts:AssumeRole"
      Effect = "Allow"
      Principal = {
        Service = "ec2.amazonaws.com"
      }
    }]
  })
}

resource "aws_iam_role_policy_attachment" "ssm_policy" {
  role       = aws_iam_role.ssm_role.name
  policy_arn = "arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore"
}

resource "aws_iam_instance_profile" "ssm_profile" {
  name = "ec2-ssm-profile"
  role = aws_iam_role.ssm_role.name
}
```

#### Common Terraform Commands

```bash
# Initialize Terraform
terraform init

# Plan infrastructure changes
terraform plan

# Apply infrastructure
terraform apply

# Get outputs (instance IDs, IPs)
terraform output

# Destroy infrastructure
terraform destroy
```

#### Terraform Outputs for Testing

```hcl
output "instance_ids" {
  description = "EC2 instance IDs for SSM access"
  value       = aws_instance.rdma_node[*].id
}

output "private_ips" {
  description = "Private IPs for RDMA communication"
  value       = aws_instance.rdma_node[*].private_ip
}

output "efa_interface_ids" {
  description = "EFA network interface IDs"
  value       = aws_network_interface.efa[*].id
}
```

## Deployment Workflow

### Standard RDMA Test Setup

1. **Provision Infrastructure**
   ```bash
   cd terraform/
   terraform init
   terraform apply
   ```

2. **Get Instance Information**
   ```bash
   INSTANCE_IDS=$(terraform output -json instance_ids | jq -r '.[]')
   PRIVATE_IPS=$(terraform output -json private_ips | jq -r '.[]')
   ```

3. **Access Instances via SSM**
   ```bash
   # Access first instance
   aws ssm start-session --target $(echo $INSTANCE_IDS | awk '{print $1}')
   ```

4. **Deploy Code to Instances**
   ```bash
   # Use SSM to run deployment script
   for instance_id in $INSTANCE_IDS; do
     aws ssm send-command \
       --instance-ids $instance_id \
       --document-name "AWS-RunShellScript" \
       --parameters 'commands=["cd /opt && git clone <repo> && cd <repo> && bash install-deps.sh && make"]'
   done
   ```

5. **Run RDMA Tests**
   ```bash
   # Server on first instance
   aws ssm start-session --target <instance-1-id>
   # In session: ./build/16_write_cpu
   
   # Client on second instance
   aws ssm start-session --target <instance-2-id>
   # In session: ./build/16_write_cpu <server-private-ip>
   ```

6. **Cleanup**
   ```bash
   terraform destroy
   ```

## Instance Type Recommendations

### EFA-Enabled Instances for RDMA

- **c5n.18xlarge**: 72 vCPUs, 192 GB RAM, 100 Gbps network, 1 EFA
- **c6gn.16xlarge**: 64 vCPUs (Graviton2), 128 GB RAM, 100 Gbps, 1 EFA
- **p4d.24xlarge**: 96 vCPUs, 1152 GB RAM, 8x A100 GPUs, 400 Gbps, 4 EFAs
- **p5.48xlarge**: 192 vCPUs, 2048 GB RAM, 8x H100 GPUs, 3200 Gbps, 32 EFAs

### Selection Criteria

- **CPU-to-CPU RDMA**: c5n.18xlarge, c6gn.16xlarge
- **GPU-to-GPU RDMA**: p4d.24xlarge, p5.48xlarge
- **Cost-effective testing**: c5n.9xlarge (half size)
- **Maximum performance**: p5.48xlarge

## Helper Scripts Integration

### Existing Scripts

- `launch-instances.sh` - Quick instance launcher (consider migrating to Terraform)
- `assume-role.sh` - AWS role assumption for cross-account access
- `rdma-helper.sh` - RDMA utility functions

### Migration Path

For complex setups, prefer Terraform over `launch-instances.sh`:
- Better state management
- Easier to modify and version control
- Supports complex dependencies
- Reproducible across environments

Keep `launch-instances.sh` for quick, one-off testing scenarios.
