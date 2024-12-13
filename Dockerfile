FROM debian:bullseye-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang \
    libpq-dev \
    make \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --gid 666 builder && useradd --uid 666 --gid 666 --shell /bin/bash --create-home builder
USER builder
WORKDIR /build

COPY --chown=builder:builder token_harvester.c /build
COPY --chown=builder:builder Makefile /build

RUN make

FROM debian:bullseye-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    ca-certificates \
    libpq5 \
    awscli \
    jq \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --gid 666 runner && useradd --uid 666 --gid 666 --shell /bin/bash --create-home runner

RUN mkdir -p /app/data && chown -R runner:runner /app
VOLUME /app
WORKDIR /app

COPY --from=builder /build/token_harvester /app/
COPY --chown=runner:runner /send_bulk_templated_email.sh /app/
COPY --chown=runner:runner /mock_aws_ses.sh /app/

USER 666

CMD ["/bin/sh", "-c", "/app/token_harvester | /app/send_bulk_templated_email.sh"]
