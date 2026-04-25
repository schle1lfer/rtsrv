#!/bin/bash

BUILD_DIR="server-build"
INSTALL_DIR="server-local"

TARGET_BUILD_DIR="target-build"
TARGET_INSTALL_DIR="target-local"

N_PROC=1

# local dir
rm -rf local/

# servers dirs
rm -rf ${INSTALL_DIR}/
rm -rf ${BUILD_DIR}/

# targets dirs
rm -rf ${TARGET_INSTALL_DIR}/
rm -rf ${TARGET_BUILD_DIR}/

cmake -S . \
 -B ${BUILD_DIR} \
 -DCMAKE_BUILD_TYPE=Debug \
 -DCMAKE_INSTALL_PREFIX=${INSTALL_DIR} \
&& \
cmake --build ${BUILD_DIR} -j${N_PROC} \
&& \
cmake --install ${BUILD_DIR}

