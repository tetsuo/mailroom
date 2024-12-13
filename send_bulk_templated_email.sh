#!/bin/bash

if [[ -n "$AWS_MOCK_DEBUG" ]]; then
    aws="./mock_aws_ses.sh"
    echo "[WARN] debug mode enabled: mock AWS CLI is being used"
else
    aws=$(which aws)
fi

readonly FIELDS_PER_RECORD=5

readonly FROM_EMAIL_ACTIVATION="Mercury <noreply@example.com>"
readonly TEMPLATE_ACTIVATION="ActivationEmail"
readonly CONFIG_SET_ACTIVATION="default"

readonly FROM_EMAIL_PASSWORD_RECOVERY="Mercury <noreply@example.com>"
readonly REPLY_TO_PASSWORD_RECOVERY="Mercury Support <support@example.com>"
readonly TEMPLATE_PASSWORD_RECOVERY="PasswordRecoveryEmail"
readonly CONFIG_SET_PASSWORD_RECOVERY="default"

readonly BASE_OUTDIR="data"

# Function to send bulk activation emails
send_bulk_activation_emails() {
    local group_activation=("$@")
    echo "[INFO] processing ${#group_activation[@]} activation emails..."

    local bulk_activation=""
    for row in "${group_activation[@]}"; do
        # Extract fields
        IFS=',' read -r _action email username secret _code <<< "$row"
        # Construct the bulk entry
        bulk_activation+="{\"Destination\":{\"ToAddresses\":[\"$email\"]},\"ReplacementTemplateData\":{\"login\":\"$username\",\"secret\":\"$secret\"}},"
    done

    # Remove trailing comma
    bulk_activation=${bulk_activation%,}

    local outdir=""
    local outfile=""

    outdir="$BASE_OUTDIR/activation/$(date '+%Y%m%d')"
    mkdir -p "$outdir"

    outfile="$outdir/$(date '+%H%M%S').json"

    echo "[$bulk_activation]" | jq '.' > "$outfile"
    echo "[INFO] written ${#group_activation[@]} destinations to $outfile"

    # Send the bulk activation email using AWS CLI
    if ! output=$($aws ses send-bulk-templated-email \
        --source "$FROM_EMAIL_ACTIVATION" \
        --template "$TEMPLATE_ACTIVATION" \
        --configuration-set-name "$CONFIG_SET_ACTIVATION" \
        --default-template-data "{\"login\":\"\",\"secret\":\"\"}" \
        --destinations file://"$outfile" 2>&1); then
        echo "[ERROR] AWS CLI command failed for activation emails in file: $outfile" >&2
        echo "[ERROR] CLI output: $output" >&2
    fi

    local failed_destinations
    failed_destinations=$(echo "$output" | jq -r '.Status[] | select(.Status=="Failed") | .Destination.ToAddresses[]' 2>/dev/null)
    if [[ -n "$failed_destinations" ]]; then
        echo "[ERROR] Failed destinations for file $outfile:" >&2
        echo "$failed_destinations" >&2
    fi

    local successful_destinations
    successful_destinations=$(echo "$output" | jq -r '.Status[] | select(.Status=="Success") | .Destination.ToAddresses[]' 2>/dev/null)
    if [[ -n "$successful_destinations" ]]; then
        echo "[INFO] Successful destinations for file $outfile:" >&2
        echo "$successful_destinations" >&2
    fi
}

# Function to send bulk password recovery emails
send_bulk_password_recovery_emails() {
    local group_password_recovery=("$@")
    echo "[INFO] processing ${#group_password_recovery[@]} password recovery emails..."

    local bulk_password_recovery=""
    for row in "${group_password_recovery[@]}"; do
        IFS=',' read -r _action email username secret code <<< "$row"
        bulk_password_recovery+="{\"Destination\":{\"ToAddresses\":[\"$email\"]},\"ReplacementTemplateData\":{\"login\":\"$username\",\"secret\":\"$secret\",\"code\":\"$code\"}},"
    done

    # Remove trailing comma
    bulk_password_recovery=${bulk_password_recovery%,}

    local outdir=""
    local outfile=""

    outdir="$BASE_OUTDIR/password_recovery/$(date '+%Y%m%d')"
    mkdir -p "$outdir"

    outfile="$outdir/$(date '+%H%M%S').json"

    echo "[$bulk_password_recovery]" | jq '.' > "$outfile"
    echo "[INFO] written ${#group_password_recovery[@]} destinations to $outfile"

    # Send the bulk email using AWS CLI
    if ! output=$($aws ses send-bulk-templated-email \
        --source "$FROM_EMAIL_PASSWORD_RECOVERY" \
        --reply-to-addresses "$REPLY_TO_PASSWORD_RECOVERY" \
        --template "$TEMPLATE_PASSWORD_RECOVERY" \
        --configuration-set-name "$CONFIG_SET_PASSWORD_RECOVERY" \
        --default-template-data "{\"login\":\"\",\"secret\":\"\",\"code\":\"\"}" \
        --destinations file://"$outfile" 2>&1); then
        echo "[ERROR] AWS CLI command failed for password recovery emails in file: $outfile" >&2
        echo "[ERROR] CLI output: $output" >&2
    fi

    local failed_destinations
    failed_destinations=$(echo "$output" | jq -r '.Status[] | select(.Status=="Failed") | .Destination.ToAddresses[]' 2>/dev/null)
    if [[ -n "$failed_destinations" ]]; then
        echo "[ERROR] Failed destinations for file $outfile:" >&2
        echo "$failed_destinations" >&2
    fi

    local successful_destinations
    successful_destinations=$(echo "$output" | jq -r '.Status[] | select(.Status=="Success") | .Destination.ToAddresses[]' 2>/dev/null)
    if [[ -n "$successful_destinations" ]]; then
        echo "[INFO] Successful destinations for file $outfile:" >&2
        echo "$successful_destinations" >&2
    fi
}

process_line() {
    local group_activation=()
    local group_password_recovery=()
    local chunk=$1
    local data
    IFS=',' read -r -a data <<< "$chunk"

    local row
    local user_action

    # Parse the array into records of FIELDS_PER_RECORD fields
    for ((i=0; i<${#data[@]}; i+=FIELDS_PER_RECORD)); do
        # If we don't have enough fields to form a complete record, skip
        if (( i + FIELDS_PER_RECORD > ${#data[@]} )); then
            continue
        fi

        # Build the row from the next set of fields
        row=""
        user_action=""
        for ((j=0; j<FIELDS_PER_RECORD; j++)); do
            field="${data[i+j]}"
            row+="$field,"
            # Capture the first field as the grouping key
            if (( j == 0 )); then
                user_action=$field
            fi
        done

        # Remove the trailing comma from the row
        row=${row%,}

        # Append the row to the appropriate group
        if [ "$user_action" == "activation" ]; then
            group_activation+=("$row")
        elif [ "$user_action" == "password_recovery" ]; then
            group_password_recovery+=("$row")
        fi
    done

    # Process the grouped data

    if [ ${#group_activation[@]} -gt 0 ]; then
        send_bulk_activation_emails "${group_activation[@]}"
    fi

    if [ ${#group_password_recovery[@]} -gt 0 ]; then
        send_bulk_password_recovery_emails "${group_password_recovery[@]}"
    fi

    sleep 1
}

while IFS= read -r line; do
    process_line "$line"
done
