# Netplay relay server image. Build from this directory:
#   docker build -t spaghettikart-relay tools/netplay-relay
#   docker run -d --name relay -p 7777:7777/udp -p 7777:7777/tcp --restart unless-stopped spaghettikart-relay
# UDP is for the desktop game, TCP for browsers' WebSockets.
FROM debian:bookworm-slim AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates cmake g++ git make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt main.cpp RelayProtocol.h WebSocket.h ./
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

FROM debian:bookworm-slim
COPY --from=build /src/build/netplay-relay /usr/local/bin/netplay-relay
USER nobody
EXPOSE 7777/udp 7777/tcp
ENTRYPOINT ["netplay-relay"]
CMD ["--port", "7777"]
