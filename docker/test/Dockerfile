FROM kezhuw/bazel:0.1.3

MAINTAINER Kezhu Wang kezhuw@gmail.com

RUN apt-get update && \
    apt-get install -y \
    git \
    vim \
    && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* && \
    groupadd -r test && \
    useradd -m -g test test

USER test
WORKDIR /home/test/stp

COPY ./ ./

CMD git submodule update --init; bazel build "//:stp" "//examples/echo:client" "examples/echo:server"
