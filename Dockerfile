FROM alpine:latest AS builder

RUN apk add --no-cache \
    build-base \
    clang \
    postgresql-dev \
    openssl-dev \
    make \
    curl \
    && addgroup -g 666 builder \
    && adduser -u 666 -G builder -h /home/builder -D builder

USER builder
WORKDIR /build

RUN curl https://sh.rustup.rs -sSf | sh -s -- -y --default-toolchain stable --profile minimal

ENV PATH="/home/builder/.cargo/bin:${PATH}"

COPY --chown=builder:builder sender/Cargo.toml sender/Cargo.lock /build/sender/

RUN (cd /build/sender && cargo fetch)

COPY --chown=builder:builder \
    collector/src/main.c \
    collector/src/db.c \
    collector/src/hmac.c \
    collector/src/base64.c \
    collector/src/log.c \
    collector/src/config.h \
    collector/src/db.h \
    collector/src/hmac.h \
    collector/src/base64.h \
    collector/src/log.h \
    /build/collector/src/

COPY --chown=builder:builder collector/Makefile /build/collector/

RUN (cd /build/collector && make release)

COPY --chown=builder:builder sender/src/main.rs /build/sender/src/

RUN (cd /build/sender && cargo build --release)

FROM alpine:latest

RUN apk add --no-cache \
    bash \
    ca-certificates \
    postgresql-libs \
    && addgroup -g 666 runner \
    && adduser -u 666 -G runner -h /home/runner -D runner

RUN mkdir -p /home/runner/output && chown -R runner:runner /home/runner
VOLUME /home/runner
WORKDIR /home/runner

COPY --from=builder /build/collector/collector /home/runner/
COPY --from=builder /build/sender/target/release/sender /home/runner/

USER 666

CMD ["/bin/sh", "-c", "/home/runner/collector | /home/runner/sender"]
