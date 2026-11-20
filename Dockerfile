FROM docker.io/library/python:3.12-slim-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir --no-deps pyarrow==20.0.0
WORKDIR /src
COPY *.cpp *.hpp build.sh ./
RUN sh build.sh /usr/local/bin/shopping-sessions && strip /usr/local/bin/shopping-sessions && \
    mkdir -p /runtime-libs && cp /usr/local/lib/python3.12/site-packages/pyarrow/libarrow.so.2000 \
    /usr/local/lib/python3.12/site-packages/pyarrow/libparquet.so.2000 /runtime-libs/

FROM docker.io/library/debian:bookworm-slim
COPY --from=build /runtime-libs/ /opt/shopping/lib/
COPY --from=build /usr/local/bin/shopping-sessions /usr/local/bin/shopping-sessions
ENV LD_LIBRARY_PATH=/opt/shopping/lib NCPU=8
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/shopping-sessions"]
