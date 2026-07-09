# Multi-stage build: the "build" stage carries the full compiler toolchain
# (~600 MB); the final image copies out only the compiled binary, so what
# we ship is a few dozen MB. Users of the image never download g++.

# ---------- stage 1: build ----------
FROM ubuntu:24.04 AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the Makefile first, then sources. Docker caches each step (layer);
# ordering stable things first means edits to source don't re-run apt-get.
COPY Makefile ./
COPY src/ src/
COPY tests/ tests/

# Tests run AS PART of the image build: a broken build or failing test
# aborts `docker build`, so a bad image can never be produced/shipped.
RUN make && make test

# ---------- stage 2: runtime ----------
FROM ubuntu:24.04

COPY --from=build /src/build/kvstore /usr/local/bin/kvstore

# Documents the port the server listens on (actual publishing happens at
# `docker run -p`); 6379 is the Redis convention.
EXPOSE 6379

# ENTRYPOINT = the fixed program; CMD = default args a user may override:
#   docker run kvstore                          -> kvstore --port 6379
#   docker run kvstore --port 7000 --appendonly yes   (flags replace CMD)
ENTRYPOINT ["kvstore"]
CMD ["--port", "6379"]
