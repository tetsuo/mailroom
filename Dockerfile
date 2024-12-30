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
    listener/src/main.c \
    listener/src/db.c \
    listener/src/hmac.c \
    listener/src/base64.c \
    listener/src/log.c \
    listener/src/config.h \
    listener/src/db.h \
    listener/src/hmac.h \
    listener/src/base64.h \
    listener/src/log.h \
    /build/listener/src/

COPY --chown=builder:builder listener/Makefile /build/listener/

RUN (cd /build/listener && make release)

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

COPY --from=builder /build/listener/listener /home/runner/
COPY --from=builder /build/sender/target/release/sender /home/runner/

USER 666

CMD ["/bin/sh", "-c", "/home/runner/listener | /home/runner/sender"]
