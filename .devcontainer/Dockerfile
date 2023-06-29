# Ubuntu version for container and ponyc linking
FROM mcr.microsoft.com/vscode/devcontainers/base:ubuntu-22.04

# Pony versions to fetch and link
ARG CHANGELOG_TOOL_VER="0.5.0"
ARG CORRAL_VER="0.7.0"
ARG PONYC_VER="0.55.0"
ARG PONYUP_VER="0.7.0"

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    clang-15 \
    curl \
    g++ \
    git \
    llvm-15 \
    make \
  && ln -s /usr/bin/clang-15 /usr/bin/clang \
  && rm -rf /var/lib/apt/lists/*

RUN curl -o ponyup-init.sh https://raw.githubusercontent.com/ponylang/ponyup/${PONYUP_VER}/ponyup-init.sh
RUN if [ ! -f ponyup-init.sh ]; then echo "ponyup-init.sh not found"; exit 1; fi
RUN export SHELL=/bin/sh \
 && chmod +x ponyup-init.sh \
 && ./ponyup-init.sh \
 && /root/.local/share/ponyup/bin/ponyup default "x86_64-linux-ubuntu22.04" \
 && /root/.local/share/ponyup/bin/ponyup update ponyc release-${PONYC_VER} \
 && /root/.local/share/ponyup/bin/ponyup update corral release-${CORRAL_VER} \
 && /root/.local/share/ponyup/bin/ponyup update changelog-tool release-${CHANGELOG_TOOL_VER} \
 && mkdir -p /usr/local/share \
 && cp -apr /root/.local/share/ponyup /usr/local/share/ponyup \
 && unlink /usr/local/share/ponyup/bin/ponyc \
 && unlink /usr/local/share/ponyup/bin/corral \
 && unlink /usr/local/share/ponyup/bin/changelog-tool \
 && ln -s /usr/local/share/ponyup/corral-release-${CORRAL_VER}-x86_64-linux/bin/corral /usr/local/bin/corral \
 && ln -s /usr/local/share/ponyup/changelog-tool-release-${CHANGELOG_TOOL_VER}-x86_64-linux/bin/changelog-tool /usr/local/bin/changelog-tool \
 && ln -s /usr/local/share/ponyup/ponyc-release-${PONYC_VER}-x86_64-linux-ubuntu22.04/bin/ponyc /usr/local/bin/ponyc