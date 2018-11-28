#!/bin/bash

export SICM_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null && cd ../.. && pwd )"
source $SICM_DIR/scripts/all/pebs.sh
cd $SICM_DIR/examples/high/lulesh

pebs "128" "./lulesh2.0 -s 220 -i 20 -r 11 -b 0 -c 64 -p"
