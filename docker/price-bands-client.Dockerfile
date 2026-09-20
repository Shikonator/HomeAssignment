# syntax=docker/dockerfile:1.7
#
# Container file for the price-bands publisher service.
#
# See docker/aggregator.Dockerfile for why these files are thin. Build the two
# shared images once:
#   docker build -f docker/Dockerfile --target builder -t md-builder .
#   docker build -f docker/Dockerfile --target runtime -t md-runtime .
# then this service:
#   docker build -f docker/price-bands-client.Dockerfile -t md/price-bands .
#   docker run --rm --network md md/price-bands --server=aggregator:50051
ARG BUILDER=md-builder
ARG RUNTIME=md-runtime

FROM ${BUILDER} AS build
FROM ${RUNTIME}

COPY --from=build /out/price_bands_client /usr/local/bin/
ENTRYPOINT ["price_bands_client"]
