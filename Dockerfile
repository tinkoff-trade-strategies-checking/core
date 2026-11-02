# Runtime-образ Simulation Worker (используется в backend/deploy/docker-compose.yml).
# Сборка: docker build -t simulation-worker .

FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential g++-13 cmake ninja-build pkg-config ca-certificates \
        libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY proto proto
COPY src src
COPY tests tests

# Тесты в образ не собираем: их гоняет CI отдельно.
RUN CC=gcc-13 CXX=g++-13 cmake --preset release -DCORE_BUILD_TESTS=OFF \
    && cmake --build --preset release -j2

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends libgrpc++1.51t64 libprotobuf32t64 libcurl4t64 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home --shell /usr/sbin/nologin worker

COPY --from=build /src/build/release/src/worker/simulation_worker /usr/local/bin/simulation_worker
COPY --from=build /src/build/release/src/cli/sim_cli /usr/local/bin/sim_cli

USER worker
EXPOSE 50051
ENTRYPOINT ["simulation_worker"]
CMD ["--listen", "0.0.0.0:50051"]
