#!/bin/bash

EXPERIMENT=0

SIM_EXEC="./sim"
#SIM_EXEC="./sim-go"

declare -a N_PEAK_FETCH=( 1 2 4 8 )
declare -a SCHED_QUEUE_SIZE=( 8 16 32 64 128 256 )

run_sim() {
    echo $SIM_EXEC "$1" "$2" "$3" "$EXPERIMENT"
    $SIM_EXEC "$1" "$2" "$3" "$EXPERIMENT" 1>/dev/null
}

process_valfile() {
    echo "Processing $1"

    for N in "${N_PEAK_FETCH[@]}"
    do
        for S in "${SCHED_QUEUE_SIZE[@]}"
        do           
            run_sim $S $N $1
        done
    done
}

process_valfile val_trace_gcc.txt
process_valfile val_trace_perl.txt
