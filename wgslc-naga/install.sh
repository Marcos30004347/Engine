#!/usr/bin/env bash
set -e

BIN_NAME=wgslc-naga

echo "Building ${BIN_NAME}..."
cargo build --release

echo "Installing to /usr/local/bin..."
install -m 755 target/release/${BIN_NAME} /usr/local/bin/${BIN_NAME}

echo "Installed ${BIN_NAME}"
