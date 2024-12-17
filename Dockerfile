FROM alpine:latest AS builder

RUN apk add --no-cache \
    build-base \
    clang \
    postgresql-dev \
    openssl-dev \
    make \
    && addgroup -g 666 builder \
    && adduser -u 666 -G builder -h /home/builder -D builder

USER builder
WORKDIR /build

COPY --chown=builder:builder main.c db.c hmac.c base64.c config.h db.h hmac.h base64.h Makefile /build/

RUN make release

FROM alpine:latest

RUN apk add --no-cache \
    bash \
    ca-certificates \
    postgresql-libs \
    aws-cli \
    jq \
    && addgroup -g 666 runner \
    && adduser -u 666 -G runner -h /home/runner -D runner

RUN mkdir -p /app/data && chown -R runner:runner /app
VOLUME /app
WORKDIR /app

COPY --from=builder /build/token_harvester /app/
COPY --chown=runner:runner /send_bulk_templated_email.sh /app/
COPY --chown=runner:runner /mock_aws_ses.sh /app/

USER 666

CMD ["/bin/sh", "-c", "/app/token_harvester | /app/send_bulk_templated_email.sh"]
