FROM ubuntu:22.10

RUN TZ=UTC
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone
RUN apt-get update && \
    apt-get install --no-install-recommends -y tzdata make gcc libc6-dev postgresql-14 libpq-dev postgresql-doc-14 git ca-certificates && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

WORKDIR /root
RUN git clone https://github.com/laurenz/pgreplay.git
WORKDIR /root/pgreplay
RUN ./configure --with-postgres=/usr/bin
RUN make
RUN make install
RUN ln -s /root/pgreplay/pgreplay /usr/local/bin

