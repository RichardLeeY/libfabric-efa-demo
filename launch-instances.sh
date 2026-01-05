#!/bin/bash

if [ -z "$1" ]; then
    echo "Usage: $0 <instance-type>"
    echo "Example: $0 p5.48xlarge"
    exit 1
fi

INSTANCE_TYPE=$1
REGION="us-east-1"
PLACEMENT_GROUP="rdma-cluster"
SECURITY_GROUP="sg-04278aad4636be358"
AMI_ID="ami-024bc6a3c7c7b78b2"

echo "Launching 2 instances of type: $INSTANCE_TYPE"

aws ec2 run-instances \
    --region $REGION \
    --image-id $AMI_ID \
    --instance-type $INSTANCE_TYPE \
    --count 2 \
    --placement "GroupName=$PLACEMENT_GROUP" \
    --network-interfaces "DeviceIndex=0,NetworkCardIndex=0,InterfaceType=efa,Groups=$SECURITY_GROUP,SubnetId=subnet-09e378b50fe0c7de9,EnaSrdSpecification={EnaSrdEnabled=true}" \
    --metadata-options "HttpTokens=optional,HttpPutResponseHopLimit=2" \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=libfabric-demo}]"
