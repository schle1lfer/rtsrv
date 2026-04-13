#!/bin/bash

BUILD_DIR="server-build"
INSTALL_DIR="server-local"
N_PROC=1

rm -rf ${INSTALL_DIR}/
rm -rf ${BUILD_DIR}/

cmake -S . \
 -B ${BUILD_DIR} \
 -DCMAKE_BUILD_TYPE=Debug \
 -DCMAKE_INSTALL_PREFIX=${INSTALL_DIR} \
&& \
cmake --build ${BUILD_DIR} -j${N_PROC} \
&& \
cmake --install ${BUILD_DIR}

