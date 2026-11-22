FROM docker.io/library/gcc:15-bookworm@sha256:9ca91b05c7b07d2979f16413e8b2cd6ec8a7c80ffca4121ccab0aeba33f90460 AS toolchain
RUN apt-get update && apt-get install -y --no-install-recommends python3 liblz4-dev && rm -rf /var/lib/apt/lists/*
ADD https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/generator.zip /tmp/profile-generator.zip
RUN echo "78f973f75d14b48e792628284f19c75de71fa597d20952e16a9ced93c78bee4a  /tmp/profile-generator.zip" | sha256sum -c - && python3 -m zipfile -e /tmp/profile-generator.zip /opt/profile-source

FROM docker.io/library/rust:1.85.1-slim-bookworm AS generator
WORKDIR /work
COPY --from=toolchain /opt/profile-source/generator ./
RUN cargo build --locked --offline --release

FROM toolchain AS build
COPY --from=generator /work/target/release/generate /usr/local/bin/shopping-profile-generator
ARG ARCH_FLAGS="-march=cascadelake -mtune=cascadelake"
WORKDIR /src
COPY *.cpp *.hpp build.sh training.py ./
RUN ARCH_FLAGS="$ARCH_FLAGS" sh build.sh /usr/local/bin/shopping-sessions && strip /usr/local/bin/shopping-sessions

FROM docker.io/library/debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libmimalloc2.0 && rm -rf /var/lib/apt/lists/*
ENV LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libmimalloc.so.2
COPY --from=build /usr/local/bin/shopping-sessions /usr/local/bin/shopping-sessions
ENV NCPU=8
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/shopping-sessions"]
