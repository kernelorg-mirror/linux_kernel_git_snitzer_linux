#!/bin/bash
# bql-strobe.sh <n> <bytes>: txdrain.sh <n> N (the READ strobe with
# skbshape.bt, BQL samples and the mesh's memory traffic) with every
# transmit queue's BQL limit_max at <bytes>, put back to the kernel's
# default after, whatever happens.  settled.sh does not hold BQL.  Round
# 104, 17 November (Mike: "try a lower BQL limit at N").
n=${1:?bql-strobe.sh <n> <bytes>} max=${2:?}
H=$(cd "$(dirname "$0")" && pwd)
Q="/sys/class/net/enp161s0f0/queues/tx-* /sys/class/net/enp164s0f0/queues/tx-*"
trap 'for q in $Q; do echo 1879048192 > $q/byte_queue_limits/limit_max; done; echo "limit_max back to $(cat /sys/class/net/enp161s0f0/queues/tx-0/byte_queue_limits/limit_max)"' EXIT
for q in $Q; do echo $max > $q/byte_queue_limits/limit_max || exit 1; done
echo "limit_max $max on every transmit queue"
PROBE=skbshape $H/txdrain.sh $n N
