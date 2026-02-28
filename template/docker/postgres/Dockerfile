FROM postgres:latest

ENV TZ=UTC \
    LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8

RUN set -eux; \
    echo "$TZ" > /etc/timezone; \
    echo "en_US.UTF-8 UTF-8" > /etc/locale.gen; \
    apt-get update && apt-get install -y --no-install-recommends \
        tzdata locales ca-certificates && \
    ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && \
    dpkg-reconfigure -f noninteractive tzdata && \
    dpkg-reconfigure -f noninteractive locales && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

USER postgres
