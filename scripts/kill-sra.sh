#!/bin/bash

pkill -x sra

pkill -f 'run-sra'

ps aux | grep -ie sra | awk '{print $2}' | xargs kill -9
