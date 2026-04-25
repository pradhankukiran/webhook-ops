FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --config Release

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/webhookops-tunnel /usr/local/bin/webhookops-tunnel
COPY --from=build /src/build/webhookops-agent /usr/local/bin/webhookops-agent

EXPOSE 9700

CMD ["sh", "-c", "webhookops-tunnel run --control 0.0.0.0:9700 --proxy 0.0.0.0:9080 --secret \"${WEBHOOKOPS_TUNNEL_SECRET:-dev-secret}\" --state /tmp/webhookops-tunnel.state"]
