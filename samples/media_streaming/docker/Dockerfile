#
# Image for the http-c media-streaming sample. Adds ffmpeg (for
# generating throwaway sample.mp4 / sample.mp3) and openssl (for the
# self-signed cert) to the standard build environment used by the
# devcontainer.
#
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        libssl-dev \
        libcmocka-dev \
        ca-certificates \
        openssl \
        ffmpeg && \
    rm -rf /var/lib/apt/lists/*

# Source is bind-mounted at /src by `docker run`. The container does its
# own out-of-tree build in /build so the host filesystem stays clean.
WORKDIR /build

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

EXPOSE 8086

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
