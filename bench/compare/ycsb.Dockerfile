# lockstep-ycsb: the SINGLE unified workload generator used to drive ALL competitors.
#
# Fairness: ONE identical YCSB client (github.com/pingcap/go-ycsb) drives postgres,
# cockroach (both via the postgresql binding over the pg wire), etcd, and tikv via their
# native bindings. The client is intentionally well-resourced (the SERVER carries the
# BENCH_CPUS pin); this image only ships the go-ycsb binary.
#
# Build:  docker build -f ycsb.Dockerfile -t lockstep-ycsb:latest .
# Verify: docker run --rm lockstep-ycsb:latest go-ycsb --help
#
# arm64 (Apple Silicon) friendly: the golang base is multi-arch; we build from source so the
# resulting binary is native to the build-host arch.

FROM golang:1.22-bookworm AS build
# go-ycsb is NOT cleanly `go install`-able (build tags + module layout); the upstream
# build path is `make build`. Clone a pinned tag and build the binary into ./bin/go-ycsb.
ARG GOYCSB_VERSION=v1.0.1
ENV GOFLAGS=-buildvcs=false
RUN apt-get update && apt-get install -y --no-install-recommends git make ca-certificates \
 && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch ${GOYCSB_VERSION} \
      https://github.com/pingcap/go-ycsb /src \
 && cd /src && make build && test -x ./bin/go-ycsb

FROM debian:bookworm-slim
# ca-certificates: TLS roots in case a binding dials TLS. tini: clean PID-1 signal handling.
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates tini \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/bin/go-ycsb /usr/local/bin/go-ycsb
# Workload property files are mounted at /workloads by the adapter (read-only).
WORKDIR /work
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["go-ycsb", "--help"]
