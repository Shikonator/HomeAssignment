# syntax=docker/dockerfile:1.7
#
# One Dockerfile, five stages: a shared builder plus one thin runtime image per
# service. All four services come from the same `builder` stage, so gRPC,
# Boost and protobuf are compiled ONCE per `docker compose build` rather than
# four times.
#
# The first build on a clean machine compiles gRPC, Boost and protobuf from
# source and takes roughly 15-25 minutes. Subsequent builds reuse the BuildKit
# cache mount below and take seconds. See README, "Build times".

# ---------------------------------------------------------------------------
# builder
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        curl \
        python3 \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/*

# Bazelisk reads .bazelversion and fetches the matching Bazel, so the container
# and a developer's machine build with the same toolchain rather than whatever
# the base image happens to ship.
ARG BAZELISK_VERSION=v1.25.0
RUN arch="$(dpkg --print-architecture)" \
    && curl -fsSL -o /usr/local/bin/bazel \
        "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-${arch}" \
    && chmod +x /usr/local/bin/bazel

WORKDIR /src
COPY . .

# The cache mount is what makes rebuilds cheap. It is a BuildKit feature; if
# BuildKit is disabled the build still works, it is just slow every time.
# --jobs is capped deliberately. Translation units in gRPC, protobuf and Abseil
# routinely want 1-2 GB each, so an uncapped parallel build can ask for more RAM
# than the Docker VM has. Colima defaults to 2 GB and Docker Desktop to 8 GB;
# exceeding either produces an opaque `cc1plus: signal 9` with nothing pointing
# at memory. See README, "Build requirements".
#
# Deliberately only --jobs: --local_ram_resources was REMOVED in Bazel 9, and
# passing it fails the build outright rather than being ignored.
RUN --mount=type=cache,target=/root/.cache/bazel,sharing=locked \
    bazel build \
        --jobs=4 \
        //src/aggregator:aggregator \
        //src/clients:bbo_client \
        //src/clients:volume_bands_client \
        //src/clients:price_bands_client \
        //src/clients:status_client \
    && mkdir -p /out \
    && cp -L bazel-bin/src/aggregator/aggregator /out/ \
    && cp -L bazel-bin/src/clients/bbo_client /out/ \
    && cp -L bazel-bin/src/clients/volume_bands_client /out/ \
    && cp -L bazel-bin/src/clients/price_bands_client /out/ \
    && cp -L bazel-bin/src/clients/status_client /out/

# ---------------------------------------------------------------------------
# runtime base: shared by every service image
# ---------------------------------------------------------------------------
#
# MUST be the same distribution release as the builder. glibc is backward
# compatible, not forward: a binary records the minimum symbol version it needs
# and the loader refuses it on anything older. GCC 13 on glibc 2.38+ emits
# references to __isoc23_strtol and friends for ordinary strtol/sscanf calls,
# and those symbols do not exist before 2.38 -- so building on ubuntu:24.04
# (glibc 2.39) and running on debian:bookworm (2.36) fails at exec with
# `version GLIBC_2.38 not found`, AFTER a successful twenty-minute build.
#
# Verify after any base-image change:
#   objdump -T /out/aggregator | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -3
FROM ubuntu:24.04 AS runtime

# ca-certificates is not optional: the aggregator verifies the exchanges' TLS
# certificates rather than disabling verification, so without a trust store
# every venue handshake fails.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 app

USER app
WORKDIR /home/app

# ---------------------------------------------------------------------------
# service images
# ---------------------------------------------------------------------------
FROM runtime AS aggregator
# status_client ships alongside so the health check needs no extra binary.
COPY --from=builder /out/aggregator /out/status_client /usr/local/bin/
EXPOSE 50051
# Declared on the image as well as in compose, so `docker run` gets it too.
HEALTHCHECK --interval=3s --timeout=5s --retries=20 --start-period=10s \
    CMD status_client --server=127.0.0.1:50051 --quiet
ENTRYPOINT ["aggregator"]
CMD ["--listen=0.0.0.0:50051"]

FROM runtime AS bbo-client
COPY --from=builder /out/bbo_client /usr/local/bin/
ENTRYPOINT ["bbo_client"]

FROM runtime AS volume-bands-client
COPY --from=builder /out/volume_bands_client /usr/local/bin/
ENTRYPOINT ["volume_bands_client"]

FROM runtime AS price-bands-client
COPY --from=builder /out/price_bands_client /usr/local/bin/
ENTRYPOINT ["price_bands_client"]
