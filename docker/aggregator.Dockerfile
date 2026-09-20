# syntax=docker/dockerfile:1.7
#
# Container file for the AGGREGATOR service.
#
# Every service has one of these. They are thin on purpose: the expensive work
# -- compiling gRPC, Boost and protobuf -- happens once in the shared `builder`
# stage of docker/Dockerfile, and each service file just selects its binary out
# of it. Four self-contained Dockerfiles would compile that dependency tree four
# times, turning a 25-minute build into a 100-minute one.
#
# Build the two shared images once:
#   docker build -f docker/Dockerfile --target builder -t md-builder .
#   docker build -f docker/Dockerfile --target runtime -t md-runtime .
# then this service:
#   docker build -f docker/aggregator.Dockerfile -t md/aggregator .
#   docker run --rm --name aggregator --network md -p 50051:50051 md/aggregator
#
# `docker compose up` does all of this for you and is the easier path.
ARG BUILDER=md-builder
ARG RUNTIME=md-runtime

FROM ${BUILDER} AS build
FROM ${RUNTIME}

# status_client ships alongside so the health check needs no extra binary.
COPY --from=build /out/aggregator /out/status_client /usr/local/bin/
EXPOSE 50051
HEALTHCHECK --interval=3s --timeout=5s --retries=20 --start-period=10s \
    CMD status_client --server=127.0.0.1:50051 --quiet
ENTRYPOINT ["aggregator"]
CMD ["--listen=0.0.0.0:50051"]
