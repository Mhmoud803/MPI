FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# 1. Install all dependencies and SSH in a single layer to optimize image size
RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    build-essential \
    gcc \
    make \
    openmpi-bin \
    libopenmpi-dev \
    python3 \
    python3-matplotlib \
    python3-numpy \
    openssh-server \
    openssh-client \
    && rm -rf /var/lib/apt/lists/*

# 2. Configure SSH daemon
RUN mkdir -p /var/run/sshd && \
    echo 'root:root' | chpasswd && \
    sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config

# 3. Setup Passwordless SSH (Crucial for MPI)
RUN mkdir -p /root/.ssh && \
    ssh-keygen -t rsa -N "" -f /root/.ssh/id_rsa && \
    cp /root/.ssh/id_rsa.pub /root/.ssh/authorized_keys && \
    chmod 600 /root/.ssh/authorized_keys

# 4. Disable Strict Host Checking so MPI doesn't get stuck asking "yes/no"
RUN echo "Host *\n\tStrictHostKeyChecking no\n" >> /root/.ssh/config

WORKDIR /app

EXPOSE 22

# 5. Keep container alive and ready for MPI over SSH
CMD ["/usr/sbin/sshd", "-D"]