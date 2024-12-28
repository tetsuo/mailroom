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

COPY --chown=builder:builder src/main.c src/db.c src/hmac.c src/base64.c src/log.c src/config.h src/db.h src/hmac.h src/base64.h src/log.h /build/src/
COPY --chown=builder:builder Makefile /build/

RUN make release

COPY --chown=builder:builder sender/Cargo.toml sender/Cargo.lock /build/sender/

RUN (cd /build/sender && cargo fetch)

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

COPY --from=builder /build/listener /home/runner/
COPY --from=builder /build/sender/target/release/sender /home/runner/

USER 666

CMD ["/bin/sh", "-c", "/home/runner/listener | /home/runner/sender"]
