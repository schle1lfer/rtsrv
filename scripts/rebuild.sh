#!/bin/bash

BUILD_DIR="build"
INSTALL_DIR="local"
N_PROC=1

rm -rf ${INSTALL_DIR}/
rm -rf build/

cmake -S . \
 -B ${BUILD_DIR} \
 -DCMAKE_BUILD_TYPE=Debug \
 -DCMAKE_INSTALL_PREFIX=${INSTALL_DIR} \
&& \
cmake --build ${BUILD_DIR} -j${N_PROC} \
&& \
cmake --install ${BUILD_DIR}

