#!/bin/bash

# AWS Role Assumption Script
# Usage: ./assume-role.sh <role-arn> [session-name] [duration]

set -e

# Default values
DEFAULT_SESSION_NAME="kiro-session-$(date +%s)"
DEFAULT_DURATION=3600  # 1 hour

# Parse arguments
ROLE_ARN="$1"
SESSION_NAME="${2:-$DEFAULT_SESSION_NAME}"
DURATION="${3:-$DEFAULT_DURATION}"

# Validate required arguments
if [ -z "$ROLE_ARN" ]; then
    echo "Usage: $0 <role-arn> [session-name] [duration-seconds]"
    echo "Example: $0 arn:aws:iam::123456789012:role/MyRole my-session 3600"
    exit 1
fi

echo "Assuming role: $ROLE_ARN"
echo "Session name: $SESSION_NAME"
echo "Duration: $DURATION seconds"

# Assume the role and capture the output
ASSUME_ROLE_OUTPUT=$(aws sts assume-role \
    --role-arn "$ROLE_ARN" \
    --role-session-name "$SESSION_NAME" \
    --duration-seconds "$DURATION" \
    --output json)

# Extract credentials from the JSON response
ACCESS_KEY_ID=$(echo "$ASSUME_ROLE_OUTPUT" | jq -r '.Credentials.AccessKeyId')
SECRET_ACCESS_KEY=$(echo "$ASSUME_ROLE_OUTPUT" | jq -r '.Credentials.SecretAccessKey')
SESSION_TOKEN=$(echo "$ASSUME_ROLE_OUTPUT" | jq -r '.Credentials.SessionToken')
EXPIRATION=$(echo "$ASSUME_ROLE_OUTPUT" | jq -r '.Credentials.Expiration')

# Validate that we got valid credentials
if [ "$ACCESS_KEY_ID" = "null" ] || [ -z "$ACCESS_KEY_ID" ]; then
    echo "Error: Failed to assume role. Check your permissions and role ARN."
    exit 1
fi

echo "✅ Successfully assumed role!"
echo "Credentials expire at: $EXPIRATION"
echo ""
echo "Setting environment variables..."

# Export the credentials
export AWS_ACCESS_KEY_ID="$ACCESS_KEY_ID"
export AWS_SECRET_ACCESS_KEY="$SECRET_ACCESS_KEY"
export AWS_SESSION_TOKEN="$SESSION_TOKEN"

# Also unset any profile that might interfere
unset AWS_PROFILE

echo "✅ Environment variables set!"
echo ""
echo "You can now run AWS commands with the assumed role."
echo "To use these credentials in other shells, run:"
echo ""
echo "export AWS_ACCESS_KEY_ID=\"$ACCESS_KEY_ID\""
echo "export AWS_SECRET_ACCESS_KEY=\"$SECRET_ACCESS_KEY\""
echo "export AWS_SESSION_TOKEN=\"$SESSION_TOKEN\""
echo ""
echo "Or source this script: source $0 <role-arn>"