#!/bin/sh
# Build the PS5 payload inside Docker (works on macOS / Apple Silicon & Linux).
set -e
cd "$(dirname "$0")"
docker build -t gtavi-ps5-sdk .
docker run --rm --platform=linux/amd64 -v "$PWD/src:/work" gtavi-ps5-sdk make
ls -la src/*.elf
