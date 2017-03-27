FROM ubuntu:14.04
MAINTAINER Alex Whan <alex.whan@csiro.au>
RUN apt-get update
RUN apt-get install -y build-essential libgsl0-dev automake make
RUN mkdir /biosolutions
ADD . /biosolutions
WORKDIR "/biosolutions"
RUN autoreconf -f -i
RUN ./configure
RUN make

