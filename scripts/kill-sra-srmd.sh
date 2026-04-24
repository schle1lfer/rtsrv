#!/bin/bash

pkill -x srmd
pkill -x sra

pkill -f 'run-srmd'
pkill -f 'run-sra'
