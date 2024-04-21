#!/bin/bash

C_EXEC="./sim"
GO_EXEC="./sim-go"

declare -a EXPERIMENTS=( 0 1 2 3 4 5 )
declare -a N_PEAK_FETCH=( 1 2 4 8 )
declare -a SCHED_QUEUE_SIZE=( 8 16 32 64 128 256 )

run_sim() {
    if [ $4 == 0 ]
    then
    	echo $C_EXEC "$1" "$2" "$3"
    	$C_EXEC "$1" "$2" "$3" > /tmp/sim_c.txt
    	$GO_EXEC "$1" "$2" "$3" > /tmp/sim_go.txt
    else
    	echo $C_EXEC "$1" "$2" "$3" "$4"
    	$C_EXEC "$1" "$2" "$3" "$4" > /tmp/sim_c.txt
    	$GO_EXEC "$1" "$2" "$3" "$4" > /tmp/sim_go.txt
    fi

    diff -qs /tmp/sim_go.txt /tmp/sim_c.txt
   
    if [ $? -ne 0 ]; then
    echo -e "\033[0;31m[$1 $2 $3]: Parity check failed!\033[0m"
    
    tail -1  /tmp/sim_go.txt
    tail -1  /tmp/sim_c.txt

    exit 1
    fi

    tail -1  /tmp/sim_go.txt
    tail -1  /tmp/sim_c.txt
    echo ""
}

process_valfile() {
    echo "Processing $1"

    for N in "${N_PEAK_FETCH[@]}"
    do
        for S in "${SCHED_QUEUE_SIZE[@]}"
        do
	    for E in "${EXPERIMENTS[@]}"
	    do
		run_sim $S $N $1 $E
	    done
        done
    done
}

process_valfile val_trace_gcc.txt
process_valfile val_trace_perl.txt
