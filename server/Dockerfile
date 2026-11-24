FROM ubuntu:22.04 AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libmysqlclient-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Source/MuServer /src/Source/MuServer
COPY cmake /src/cmake
WORKDIR /src/Source/MuServer

RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build -j1

# mu-patch: publica los parches del operador (Data/Patches, MU_UPDATE_ENABLED=1).
# Su código vive en el cliente (client/tools/mu-patch y el núcleo src/update),
# así que la imagen necesita el contexto con nombre "client":
#   docker build --build-context client=client server
# (docker-compose.yml lo pasa con additional_contexts).
FROM ubuntu:22.04 AS mupatch

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/client
COPY --from=client cmake/MuUpdate.cmake cmake/MuUpdate.cmake
COPY --from=client tools/mu-patch tools/mu-patch
COPY --from=client src/update src/update
COPY --from=client src/net/crypto src/net/crypto

RUN cmake -S tools/mu-patch -B /build -DCMAKE_BUILD_TYPE=Release -DMU_PATCH_TESTS=OFF \
    && cmake --build /build --target mu-patch -j2

FROM ubuntu:22.04

LABEL org.opencontainers.image.version="0.97.46" \
      org.opencontainers.image.source="https://github.com/EmanuelCatania/Mu-Linux" \
      org.opencontainers.image.description="Servidor MU Online 0.97k para Linux; texto UTF-8, traducciones por idioma y MySQL por la API en C (Fase 4)" \
      mu-linux.client-release="https://github.com/EmanuelCatania/Mu-Linux/releases/tag/0.97.46"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        netcat-openbsd \
        libmysqlclient21 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/mu

COPY --from=build /build/bin/ConnectServer /opt/mu/ConnectServer/ConnectServer
COPY --from=build /build/bin/JoinServer /opt/mu/JoinServer/JoinServer
COPY --from=build /build/bin/DataServer /opt/mu/DataServer/DataServer
COPY --from=build /build/bin/GameServer /opt/mu/GameServer/GameServer
COPY --from=build /build/bin/mu-data /opt/mu/mu-data
COPY --from=build /build/bin/mu-trace /opt/mu/mu-trace
COPY --from=build /build/bin/mu-guard /opt/mu/mu-guard
COPY --from=mupatch /build/mu-patch /opt/mu/mu-patch

COPY MuServer/ConnectServer/ConnectServer.ini /opt/mu/ConnectServer/
COPY MuServer/ConnectServer/ServerList.dat /opt/mu/ConnectServer/

COPY MuServer/MySQL/JoinServer/JoinServer.ini /opt/mu/JoinServer/
COPY MuServer/MySQL/JoinServer/AllowableIpList.txt /opt/mu/JoinServer/

COPY MuServer/MySQL/DataServer/DataServer.ini /opt/mu/DataServer/
COPY MuServer/MySQL/DataServer/AllowableIpList.txt /opt/mu/DataServer/
COPY MuServer/MySQL/DataServer/BadSyntax.txt /opt/mu/DataServer/

COPY MuServer/GameServer/DATA /opt/mu/GameServer/DATA
COPY MuServer/Data /opt/mu/Data

COPY docker/entrypoint.sh /opt/mu/entrypoint.sh

RUN chmod +x /opt/mu/entrypoint.sh \
    /opt/mu/ConnectServer/ConnectServer \
    /opt/mu/JoinServer/JoinServer \
    /opt/mu/DataServer/DataServer \
    /opt/mu/GameServer/GameServer \
    /opt/mu/mu-data \
    /opt/mu/mu-trace \
    /opt/mu/mu-guard \
    /opt/mu/mu-patch \
    && ln -s /opt/mu/mu-data /usr/local/bin/mu-data \
    && ln -s /opt/mu/mu-trace /usr/local/bin/mu-trace \
    && ln -s /opt/mu/mu-patch /usr/local/bin/mu-patch \
    && mv /opt/mu/GameServer/DATA /opt/mu/GameServer/Data

EXPOSE 44405/tcp 44406/tcp 55601/udp 55901/tcp

ENTRYPOINT ["/opt/mu/entrypoint.sh"]
