FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    build-essential \
    gcc \
    make \
    mpich \
    python3 \
    python3-matplotlib \
    python3-numpy \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Keep container ready for interactive execution.
CMD ["/bin/bash"]
