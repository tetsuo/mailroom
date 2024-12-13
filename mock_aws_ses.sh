#!/bin/bash

# Mock AWS SES send-bulk-templated-email
# Simulates the AWS CLI command, logs parameters, and outputs mock success/failure responses.

LOG_FILE="mock_aws_ses.log"

# Check if the correct command is being mocked
if [[ $1 == "ses" && $2 == "send-bulk-templated-email" ]]; then
    echo "---- BEGIN ----" >> "$LOG_FILE"
    echo "Command: aws ses send-bulk-templated-email" >> "$LOG_FILE"

    while [[ $# -gt 0 ]]; do
        key="$1"
        case $key in
            --source)
                SOURCE="$2"
                echo "Source: $SOURCE" >> "$LOG_FILE"
                shift; shift
                ;;
            --reply-to-addresses)
                REPLY_TO="$2"
                echo "Reply-To: $REPLY_TO" >> "$LOG_FILE"
                shift; shift
                ;;
            --template)
                TEMPLATE="$2"
                echo "Template: $TEMPLATE" >> "$LOG_FILE"
                shift; shift
                ;;
            --configuration-set-name)
                CONFIG_SET="$2"
                echo "Configuration Set: $CONFIG_SET" >> "$LOG_FILE"
                shift; shift
                ;;
            --default-template-data)
                DEFAULT_TEMPLATE_DATA="$2"
                echo "Default Template Data: $DEFAULT_TEMPLATE_DATA" >> "$LOG_FILE"
                shift; shift
                ;;
            --destinations)
                DESTINATIONS="$2"
                echo "Destinations: $DESTINATIONS" >> "$LOG_FILE"
                shift; shift
                ;;
            *)
                echo "Unknown parameter: $1" >> "$LOG_FILE"
                shift
                ;;
        esac
    done

    echo "---- END ----" >> "$LOG_FILE"

    # Simulate a response with success and failure
    RESPONSE='{
        "Status": [
            {
                "Status": "Success",
                "MessageId": "0101017f18example",
                "Destination": {
                    "ToAddresses": ["success1@example.com"]
                }
            },
            {
                "Status": "Success",
                "MessageId": "0101017f18example",
                "Destination": {
                    "ToAddresses": ["success2@example.com"]
                }
            },
            {
                "Status": "Failed",
                "Error": "Account is suppressed.",
                "Code": "AccountSuppressed",
                "Destination": {
                    "ToAddresses": ["failed1@example.com"]
                }
            },
            {
                "Status": "Failed",
                "Error": "Account is suppressed.",
                "Code": "AccountSuppressed",
                "Destination": {
                    "ToAddresses": ["failed2@example.com"]
                }
            }
        ]
    }'

    # Output the response to STDOUT
    echo "$RESPONSE"
else
    echo "[ERROR] unsupported AWS command" >&2
    exit 1
fi
