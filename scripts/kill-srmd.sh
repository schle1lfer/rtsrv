#!/bin/bash

pkill -x srmd

pkill -f 'run-srmd'

ps aux | grep -ie srmd | awk '{print $2}' | xargs kill -9
