FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        libssl-dev \
        libcmocka-dev \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /work
