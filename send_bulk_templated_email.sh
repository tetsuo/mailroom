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

send_bulk_activation_emails() {
    local group_activation entries bulk_activation outfile
    group_activation=("$@")
    echo "[INFO] processing ${#group_activation[@]} activation emails..."

    entries=()
    for row in "${group_activation[@]}"; do
        local _action email username secret _code
        IFS=',' read -r _action email username secret _code <<< "$row"
        entries+=("{\"Destination\":{\"ToAddresses\":[\"$email\"]},\"ReplacementTemplateData\":{\"login\":\"$username\",\"secret\":\"$secret\"}}")
    done

    bulk_activation=$(printf '[%s]' "$(IFS=,; echo "${entries[*]}")")
    outfile="$BASE_OUTDIR/$(date '+%Y%m%d%H%M%S')_$(printf '%04x' $((RANDOM & 0xffff))).json"

    echo "$bulk_activation" | jq '.' > "$outfile"
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
}

send_bulk_password_recovery_emails() {
    local group_password_recovery entries bulk_password_recovery outfile
    group_password_recovery=("$@")
    echo "[INFO] processing ${#group_password_recovery[@]} password recovery emails..."

    entries=()
    for row in "${group_password_recovery[@]}"; do
        local _action email username secret code
        IFS=',' read -r _action email username secret code <<< "$row"
        entries+=("{\"Destination\":{\"ToAddresses\":[\"$email\"]},\"ReplacementTemplateData\":{\"login\":\"$username\",\"secret\":\"$secret\",\"code\":\"$code\"}}")
    done

    bulk_password_recovery=$(printf '[%s]' "$(IFS=,; echo "${entries[*]}")")
    outfile="$BASE_OUTDIR/$(date '+%Y%m%d%H%M%S')_$(printf '%04x' $((RANDOM & 0xffff))).json"

    echo "$bulk_password_recovery" | jq '.' > "$outfile"
    echo "[INFO] written ${#group_password_recovery[@]} destinations to $outfile"

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
}

process_line() {
    local group_activation group_password_recovery chunk data i row user_action field
    group_activation=()
    group_password_recovery=()
    chunk="$1"

    IFS=',' read -r -a data <<< "$chunk"

    for ((i=0; i<${#data[@]}; i+=FIELDS_PER_RECORD)); do
        if (( i + FIELDS_PER_RECORD > ${#data[@]} )); then
            continue
        fi

        row=""
        user_action=""
        for ((j=0; j<FIELDS_PER_RECORD; j++)); do
            field="${data[i+j]}"
            row+="$field,"
            if (( j == 0 )); then
                user_action=$field
            fi
        done

        row=${row%,}

        if [[ "$user_action" == "activation" ]]; then
            group_activation+=("$row")
        elif [[ "$user_action" == "password_recovery" ]]; then
            group_password_recovery+=("$row")
        fi
    done

    if [[ ${#group_activation[@]} -gt 0 ]]; then
        send_bulk_activation_emails "${group_activation[@]}"
    fi

    if [[ ${#group_password_recovery[@]} -gt 0 ]]; then
        send_bulk_password_recovery_emails "${group_password_recovery[@]}"
    fi

    unset group_activation group_password_recovery

    sleep 1
}

mkdir -p $BASE_OUTDIR

while IFS= read -r line; do
    process_line "$line"
done
