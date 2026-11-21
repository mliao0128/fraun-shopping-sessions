FROM docker.io/library/python:3.12-slim-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
ARG ARCH_FLAGS="-march=cascadelake -mtune=cascadelake"
WORKDIR /src
COPY *.cpp *.hpp build.sh training.py ./
RUN ARCH_FLAGS="$ARCH_FLAGS" sh build.sh /usr/local/bin/shopping-sessions && strip /usr/local/bin/shopping-sessions

FROM docker.io/library/debian:bookworm-slim
COPY --from=build /usr/local/bin/shopping-sessions /usr/local/bin/shopping-sessions
ENV NCPU=8
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/shopping-sessions"]
