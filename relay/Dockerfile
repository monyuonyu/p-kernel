# p-kernel public relay — minimal container.
#
# The relay is two zero-dependency C sources (relay.c + sha256.c; no openssl).
# Build is a throwaway stage; the runtime image carries only the static-ish
# binary and runs as an unprivileged user. UDP only.
#
#   docker compose up -d --build       # see docker-compose.yml + .env

# ---- build stage ----
FROM alpine:3.20 AS build
RUN apk add --no-cache gcc musl-dev
WORKDIR /src
COPY relay.c sha256.c sha256.h ./
RUN cc -Wall -Wextra -O2 -std=gnu11 -fno-stack-protector -o relay relay.c sha256.c

# ---- runtime stage ----
FROM alpine:3.20
RUN adduser -D -H -s /sbin/nologin relay
COPY --from=build /src/relay /usr/local/bin/relay
USER relay
EXPOSE 7400/udp
# PKERNEL_RELAY_KEY (64 hex) must be supplied via the environment, or the
# relay refuses to start. Override the port with -p if you forward a different one.
ENTRYPOINT ["/usr/local/bin/relay"]
CMD ["-p", "7400", "-v"]
