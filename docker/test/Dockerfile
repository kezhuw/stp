FROM ubuntu:bionic

MAINTAINER Kezhu Wang kezhuw@gmail.com

RUN apt-get update && \
    apt-get install -y \
    git \
    gnupg \
    curl

RUN echo "deb [arch=amd64] http://storage.googleapis.com/bazel-apt stable jdk1.8" > /etc/apt/sources.list.d/bazel.list && \
    curl https://bazel.build/bazel-release.pub.gpg | apt-key add - && \
	apt-get update && \
    apt-get install -y \
	bazel && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* && \
    groupadd -r test && \
    useradd -m -g test test

USER test
WORKDIR /home/test/stp

COPY ./ ./

CMD git submodule update --init; bazel build "//:stp" "//examples/echo:client" "examples/echo:server"
