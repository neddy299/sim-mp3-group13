sim: sim.c
	gcc sim.c -o sim

test: sim
	./run-test.sh 8 8 val_trace_gcc.txt pipe_8_8_gcc.txt
	./run-test.sh 2 8 val_trace_gcc.txt pipe_2_8_gcc.txt
	./run-test.sh 64 1 val_trace_perl.txt pipe_64_1_perl.txt
	./run-test.sh 128 8 val_trace_perl.txt pipe_128_8_perl.txt

memcheck: sim
	valgrind ./sim 8 8 val_trace_gcc.txt > /dev/null
	valgrind ./sim 2 8 val_trace_gcc.txt > /dev/null
	valgrind ./sim 64 1 val_trace_perl.txt > /dev/null
	valgrind ./sim 128 8 val_trace_perl.txt > /dev/null

sim-go: main.go
	go build main.go

parity: sim sim-go
	@./sim-mp3-group13 8 8 val_trace_gcc.txt > /tmp/sim_go.txt
	@./sim 8 8 val_trace_gcc.txt > /tmp/sim_c.txt
	@diff -qs /tmp/sim_go.txt /tmp/sim_c.txt
	
	@./sim-mp3-group13 2 8 val_trace_gcc.txt > /tmp/sim_go.txt
	@./sim 2 8 val_trace_gcc.txt > /tmp/sim_c.txt
	@diff -qs /tmp/sim_go.txt /tmp/sim_c.txt
	
	@./sim-mp3-group13 64 1 val_trace_perl.txt > /tmp/sim_go.txt
	@./sim 64 1 val_trace_perl.txt > /tmp/sim_c.txt
	@diff -qs /tmp/sim_go.txt /tmp/sim_c.txt
	
	@./sim-mp3-group13 128 8 val_trace_perl.txt > /tmp/sim_go.txt
	@./sim 128 8 val_trace_perl.txt > /tmp/sim_c.txt
	@diff -qs /tmp/sim_go.txt /tmp/sim_c.txt