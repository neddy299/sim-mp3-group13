sim: sim.c
	gcc sim.c -o sim

clean: 
	rm sim sim-go _test_*.txt _results_*.csv

test: sim
	./run-test.sh 8 8 val_trace_gcc.txt ./validation_runs/pipe_8_8_gcc.txt
	./run-test.sh 2 8 val_trace_gcc.txt ./validation_runs/pipe_2_8_gcc.txt
	./run-test.sh 64 1 val_trace_perl.txt ./validation_runs/pipe_64_1_perl.txt
	./run-test.sh 128 8 val_trace_perl.txt ./validation_runs/pipe_128_8_perl.txt

memcheck: sim
	valgrind ./sim 8 8 val_trace_gcc.txt > /dev/null
	valgrind ./sim 2 8 val_trace_gcc.txt > /dev/null
	valgrind ./sim 64 1 val_trace_perl.txt > /dev/null
	valgrind ./sim 128 8 val_trace_perl.txt > /dev/null

sim-go: main.go
	go build -o sim-go main.go

parity: sim sim-go
	./run-parity-check.sh

csv-report: sim
	./run-csv-report.sh
