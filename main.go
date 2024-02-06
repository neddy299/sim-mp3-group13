package main

import (
	"bufio"
	"container/list"
	"fmt"
	"log"
	"os"
	"sort"
	"strconv"
	"strings"
)

//TODO
// - execute+issue: ready flag?
// - execute: wakeup dependent instructions
// - dispatch: Rename source + dest operands
// - compare results to validation files

// Debug settings
const devMode = true
const earlyExit = false
const earlyExitCycles = 10
const outputPrefix = "_out_"
const outputSuffix = ".txt"

//1) Define 5 states that an instruction can be in (e.g., use an enumerated
//  type): IF (fetch), ID (dispatch), IS (issue), EX (execute), WB (writeback).

type instructionType int

const (
	iTypeIF instructionType = iota // fetch
	iTypeID                        // dispatch
	iTypeIS                        // issue
	iTypeEX                        // execute
	iTypeWB                        // writeback
)

var instructionName = []string{"IF", "ID", "IS", "EX", "WB"}

// The operation type of an instruction indicates its execution latency:
// Type 0 has a latency of 1 cycle, Type 1 has a latency of 2 cycles, and Type 2 has a latency of 5 cycles
var operandLatency = []int{1, 2, 5}

//2) Define a circular FIFO that holds all active instructions in their program order.
//   ... AKA Fake Re-Order Buffer (ROB)
//   Each entry in the fake-ROB should be a data structure containing per
//   instruction information, for example, state of the instruction (which stage
//    it is in), operation type, operand state, sequence number (tag), etc.

type instruction struct {
	iTypeState instructionType // instruction type state: IF, ID, ...
	opState    string          // operand state //TODO READY state?
	opType     int             // operation type - is either “0”, “1”, or “2”.
	tag        int             // sequence number (auto increment when read)
	startCycle []int           // cycle that the instruction started per instruction type
	endCycle   []int           // cycle that the instruction completed per instruction type

	pc      string // program counter
	destReg int    // destination register
	src1Reg int    // source 1 register
	src2Reg int    // source 2 register
}

var ROB list.List

//3) Define 3 lists:
// a) dispatch_list : This contains a list of instructions in either the IF or ID
//   state. The dispatch_list models the Dispatch Queue. (By including both
//   the IF and ID states, we don’t need to separately model the pipeline
//   registers of the fetch and dispatch stages.)

var dispatch_list list.List

// b) issue_list : This contains a list of instructions in the IS state (waiting for
//   operands or available issue bandwidth). The issue_list models the
//   Scheduling Queue.

var issue_list list.List

// c) execute_list : This contains a list of instructions in the EX state (waiting
//   for the execution latency of the operation). The execute_list models the FUs.

var execute_list list.List

// Registers between 0 and 127
var registers [128]int // registers //TODO are these needed?

// Globals
var SchedulingQueueSize int // <S> Scheduling Queue size
var NPeakFetch int          // <N> Peak Fetch and Dispatch Rate
var dispatchQueueMax int    // Dispatch Queue is 2N instructions in size
var numInstructions int     // number of instructions
var numCycles int           // number of cycles
var tagSequence int         // tag sequence counter
var traceEOF bool           // reached trace End Of File

// Debug globals
var outputFile *os.File // debug output file

func main() {
	numInstructions = 0
	numCycles = 0
	tagSequence = 0
	traceEOF = false

	// Parse command line arguments
	var err error
	if len(os.Args) != 4 {
		log.Fatalf("Usage: %s <S> <N> <tracefile>", os.Args[0])
	}
	SchedulingQueueSize, err = strconv.Atoi(os.Args[1])
	if err != nil {
		log.Fatalf("Invalid type: '%s' for parameter <S> is not an integer", os.Args[1])
	}
	NPeakFetch, err = strconv.Atoi(os.Args[2])
	if err != nil {
		log.Fatalf("Invalid type: '%s' for parameter <N> is not an integer", os.Args[2])
	}
	dispatchQueueMax = NPeakFetch * 2
	traceFileName := os.Args[3]

	// Open trace file
	trace, err := os.Open(traceFileName)
	if err != nil {
		log.Fatalf("Error opening trace file err: %s", err.Error())
	}
	defer trace.Close()
	traceScanner := bufio.NewScanner(trace)

	// Debug logging
	if devMode {
		fmt.Printf("Scheduling Queue size(S): %d\nPeak fetch and dispatch rate(N): %d\nTrace file: %s\n", SchedulingQueueSize, NPeakFetch, traceFileName)
		outputFileName := fmt.Sprintf("%s_%d_%d_%s.%s", outputPrefix, SchedulingQueueSize, NPeakFetch, "test", outputSuffix)
		outputFile, err = os.OpenFile(outputFileName, os.O_CREATE|os.O_WRONLY, 0666)
		if err != nil {
			fmt.Fprintf(os.Stderr, "unable to open output file: "+outputFileName)
		}
	}

	// 4) Call each pipeline stage in reverse order in your main simulator loop, as
	// follows. The detailed comments indicate tasks to be performed. The order
	// among these tasks is important.

	// do { ... } while (Advance_Cycle());
	for ok := true; ok; ok = (Advance_Cycle()) {
		FakeRetire()
		Execute()
		Issue()
		Dispatch()
		Fetch(traceScanner)
	}

	// Instructions completed Per Cycle
	IPC := float64(numInstructions) / float64(numCycles)

	fmt.Printf("number of instructions = %d\n", numInstructions)
	fmt.Printf("number of cycles       = %d\n", numCycles)
	fmt.Printf("IPC                    = %.5f\n", IPC)

	if outputFile != nil {
		fmt.Fprintf(outputFile, "number of instructions = %d\n", numInstructions)
		fmt.Fprintf(outputFile, "number of cycles       = %d\n", numCycles)
		fmt.Fprintf(outputFile, "IPC                    = %.5f\n", IPC)
	}
}

// FakeRetire();
// // Remove instructions from the head of the fake-ROB
// // until an instruction is reached that is not in the WB state.
func FakeRetire() {
	printTask("FakeRetire")

	if ROB.Len() > 0 {
		temp_list := list.New()
		for e := ROB.Front(); e != nil && e.Value.(*instruction).iTypeState == iTypeWB; e = e.Next() {
			// Add instructions to temp_list for removal
			temp_list.PushBack(e.Value)

			//TODO Optimzation: don't use temp_list for removal? manually perform e.Next() iterator before removal?
		}

		//1) Remove the instruction from the ROB.
		for e := temp_list.Front(); e != nil; e = e.Next() {
			for d := ROB.Front(); d != nil; d = d.Next() {
				if d.Value == e.Value {
					i := e.Value.(*instruction)
					i.printInstruction("FakeRetire remove")
					ROB.Remove(d)
				}
			}
		}
	}
}

// Execute();
// // From the execute_list, check for instructions that are finishing execution this
// // cycle, and:
// //
// // 1) Remove the instruction from the execute_list.
// // 2) Transition from EX state to WB state.
// // 3) Update the register file state e.g., ready flag)
// //    and wakeup dependent instructions (set their operand ready flags).
func Execute() {
	printTask("Execute")

	if execute_list.Len() > 0 {
		temp_list := list.New()
		for e := execute_list.Front(); e != nil; e = e.Next() {
			i := e.Value.(*instruction)
			note := fmt.Sprintf("Exec %d of %d", numCycles-i.startCycle[i.iTypeState], operandLatency[i.opType])

			if numCycles < i.startCycle[i.iTypeState]+operandLatency[i.opType] {
				i.printInstruction(note)
			} else {
				// Functional Unit execution has finished
				i.incrementStage() // EX -> WB
				// This is the final stage and will not be incremented further, we must set end cycle here
				i.endCycle[i.iTypeState] = i.startCycle[i.iTypeState] + 1

				//TODO
				// // 3) Update the register file state e.g., ready flag)
				// //    and wakeup dependent instructions (set their operand ready flags).

				i.printInstruction(note + " EX -> WB")
				printInstructionTiming(i)

				// Add instructions to temp_list for removal
				temp_list.PushBack(e.Value)
			}
		}

		//1) Remove the instruction from the execute_list.
		for e := temp_list.Front(); e != nil; e = e.Next() {
			for d := execute_list.Front(); d != nil; d = d.Next() {
				if d.Value == e.Value {
					execute_list.Remove(d)
				}
			}
		}
	}
}

// Issue();
// // From the issue_list, construct a temp list of instructions whose
// // operands are ready – these are the READY instructions.
// // Scan the READY instructions in ascending order of
// // tags and issue up to N+1 of them. To issue an instruction:
// //
// // 1) Remove the instruction from the issue_list and add it to the execute_list.
// // 2) Transition from the IS state to the EX state.
// // 3) Free up the scheduling queue entry (e.g., decrement a count of the number of
// //	  instructions in the scheduling queue)
// // 4) Set a timer in the instruction’s data structure that will allow you to model
// //    the execution latency.
func Issue() {
	printTask("Issue")

	// From the issue_list, construct a temp list of instructions whose
	// operands are ready – these are the READY instructions.
	if issue_list.Len() > 0 {
		temp_list := []*instruction{}

		// From the issue_list, construct a temp list of instructions whose
		// operands are ready – these are the READY instructions.
		//TODO all source operands are ready? Check instructions for more info
		for e := issue_list.Front(); e != nil; e = e.Next() {
			temp_list = append(temp_list, e.Value.(*instruction))
		}

		// Scan the READY instructions in ascending order of tags
		sort.SliceStable(temp_list, func(i, j int) bool {
			return temp_list[i].tag < temp_list[j].tag
		})
		for issueCt, i := range temp_list {
			if issueCt >= NPeakFetch+1 {
				i.printInstruction("Issue stalled")
				continue
			}
			// and issue up to N+1 of them
			i.incrementStage() // IS -> EX

			// // 4) Set a timer in the instruction’s data structure that will allow you to model
			// //    the execution latency.
			// NOTE: timer is started with i.startCycle[i.iTypeState] and execution is monitored in Execute()

			i.printInstruction("Issue IS -> EX")

			//1) Remove the instruction from the issue_list and add it to the execute_list.
			execute_list.PushBack(i)
			for d := issue_list.Front(); d != nil; d = d.Next() {
				if d.Value == i {
					issue_list.Remove(d)
				}
			}
		}
	}
}

// Dispatch();
// // From the dispatch_list, construct a temp list of instructions in the ID
// // state (don’t include those in the IF state – you must model the
// // 1 cycle fetch latency). Scan the temp list in ascending order of
// // tags and, if the scheduling queue is not full, then:
// //
// // 1) Remove the instruction from the dispatch_list and add it to the
// //    issue_list. Reserve a schedule queue entry (e.g. increment a
// //    count of the number of instructions in the scheduling
// //    queue) and free a dispatch queue entry (e.g. decrement a count of
// //    the number of instructions in the dispatch queue).
// // 2) Transition from the ID state to the IS state.
// // 3) Rename source operands by looking up state in the register file;
// //    Rename destination by updating state in the register file.
// //
// // For instructions in the dispatch_list that are in the IF state,
// // unconditionally transition to the ID state (models the 1 cycle
// // latency for instruction fetch).
func Dispatch() {
	printTask("Dispatch")

	if dispatch_list.Len() > 0 {
		temp_list := []*instruction{}

		for e := dispatch_list.Front(); e != nil; e = e.Next() {
			i := e.Value.(*instruction)
			switch i.iTypeState {
			case iTypeIF:
				i.incrementStage() // IF -> ID  (models the 1 cycle latency for instruction fetch)
				i.printInstruction("Dispatch IF -> ID")
			case iTypeID:
				temp_list = append(temp_list, e.Value.(*instruction))
			}
		}

		// Scan the temp list in ascending order of tags
		sort.SliceStable(temp_list, func(i, j int) bool {
			return temp_list[i].tag < temp_list[j].tag
		})
		for _, i := range temp_list {
			if issue_list.Len() >= SchedulingQueueSize {
				i.printInstruction("Dispatch stalled")
				continue
			}
			// if the scheduling queue is not full, then:
			i.incrementStage() // ID -> IS

			//TODO
			// // 3) Rename source operands by looking up state in the register file;
			// //    Rename destination by updating state in the register file.

			i.printInstruction("Dispatch ID -> IS")

			//1) Remove the instruction from the dispatch_list and add it to the issue_list.
			issue_list.PushBack(i)
			for d := dispatch_list.Front(); d != nil; d = d.Next() {
				if d.Value == i {
					dispatch_list.Remove(d)
				}
			}
		}
	}
}

// Fetch();
// // Read new instructions from the trace as long as
// // 1) you have not reached the end-of-file,
// // 2) the fetch bandwidth is not exceeded, and
// // 3) the dispatch queue is not full.
// //
// // Then, for each incoming instruction:
// // 1) Push the new instruction onto the fake-ROB. Initialize the
// //    instruction’s data structure, including setting its state to IF.
// // 2) Add the instruction to the dispatch_list and reserve a
// //    dispatch queue entry (e.g., increment a count of the number
// //    of instructions in the dispatch queue).
func Fetch(traceScanner *bufio.Scanner) {
	printTask("Fetch")
	fetchCt := 0

	// 2) the fetch bandwidth is not exceeded, and
	// 3) the dispatch queue is not full.
	for fetchCt < NPeakFetch &&
		dispatch_list.Len() < dispatchQueueMax {
		if traceScanner.Scan() {
			i := readTrace(traceScanner.Text())
			i.printInstruction(fmt.Sprintf("Fetch %d of %d", fetchCt+1, NPeakFetch))
			fetchCt++
			numInstructions++

			// 1) Push the new instruction onto the fake-ROB
			ROB.PushBack(i)

			// 2) Add the instruction to the dispatch_list
			dispatch_list.PushBack(i)
		} else {
			// 1) you have not reached the end-of-file
			traceEOF = true
			return
		}
	}
}

// } while (Advance_Cycle());
//
//	// Advance_Cycle performs several functions.
//	// It advances the simulator cycle. Also, when it becomes
//	// known that the fake-ROB is empty AND the trace is
//	// depleted, the function returns “false” to terminate
//	// the loop.
func Advance_Cycle() bool {
	printTask("Advance_Cycle")

	if traceEOF && ROB.Len() == 0 {
		return false
	}
	numCycles++

	if earlyExit {
		if numCycles > earlyExitCycles {
			fmt.Printf("[EARLY EXIT - SANITY CHECK]\n")
			return false
		}
	}
	return true
}

// readTrace() reads a line from the trace file and returns an instruction
// The simulator reads a trace file in the following format:
// <PC> <operation type> <dest reg #> <src1 reg #> <src2 reg #>
// Fatal error handling only, application halts on invalid input
func readTrace(line string) *instruction {
	//fmt.Printf("- Read: %s\n", line)
	s := strings.Split(line, " ")
	if len(s) != 5 {
		log.Fatalf("Error parsing trace file err: expected 5 parameters, got %d. line: %s\n", len(s), s)
	}

	tag := tagSequence
	tagSequence++

	i := &instruction{
		iTypeState: iTypeIF,
		opState:    "unknown",
		opType:     convertTraceStrToInt(s[1]),
		tag:        tag,
		startCycle: make([]int, len(instructionName)),
		endCycle:   make([]int, len(instructionName)),

		pc:      s[0],
		destReg: convertTraceStrToInt(s[2]),
		src1Reg: convertTraceStrToInt(s[3]),
		src2Reg: convertTraceStrToInt(s[4]),
	}

	i.startCycle[iTypeIF] = numCycles
	i.endCycle[iTypeIF] = numCycles

	return i
}

// convertTraceStrToInt
// Fatal error handling only, application halts on invalid input
func convertTraceStrToInt(reg string) int {
	i, err := strconv.Atoi(reg)
	if err != nil {
		log.Fatalf("Error parsing converting regsiter to int, got %s\n", reg)
	}
	return i
}

// printInstructionTiming - prints functional unit and instruction stage timing values
// Output format:
//
//	0 fu{0} src{29,14} dst{-1} IF{0,1} ID{1,1} IS{2,1} EX{3,1} WB{4,1}
func printInstructionTiming(i *instruction) {
	output := fmt.Sprintf("%d fu{%d} src{%d,%d} dst{%d} IF{%d,%d} ID{%d,%d} IS{%d,%d} EX{%d,%d} WB{%d,%d}\n", i.tag, i.opType, i.src1Reg,
		i.src2Reg, i.destReg, i.startCycle[0], i.endCycle[0]-i.startCycle[0], i.startCycle[1], i.endCycle[1]-i.startCycle[1],
		i.startCycle[2], i.endCycle[2]-i.startCycle[2], i.startCycle[3], i.endCycle[3]-i.startCycle[3], i.startCycle[4], i.endCycle[4]-i.startCycle[4])
	fmt.Print(output)
	if outputFile != nil {
		fmt.Fprint(outputFile, output)
	}
}

func (i *instruction) incrementStage() {
	i.endCycle[i.iTypeState] = numCycles
	i.iTypeState++
	i.startCycle[i.iTypeState] = numCycles
}

// Debug instructions follow

func (i *instruction) printInstruction(note string) {
	if devMode {
		fmt.Printf("- %-20s - tag: %5d (%s) pc: %s op: %d dst: %2d src1: %2d src2: %2d oState: %s\n", note, i.tag, instructionName[i.iTypeState], i.pc, i.opType,
			i.destReg, i.src1Reg, i.src2Reg, i.opState)
	}
}

func printTask(taskName string) {
	if devMode {
		fmt.Printf("%-20s - dispatch: %d(%d) issue: %d(%d) execute: %d ROB: %d cycle: %d\n", taskName, dispatch_list.Len(), dispatchQueueMax, issue_list.Len(), SchedulingQueueSize,
			execute_list.Len(), ROB.Len(), numCycles)
	}
	// if devMode {
	// 	fmt.Printf("%d) %-20s - dispatch: %d(%d) issue: %d(%d) execute: %d ROB: %d\n", numCycles, taskName, dispatch_list.Len(), dispatchQueueMax, issue_list.Len(), SchedulingQueueSize,
	// 		execute_list.Len(), ROB.Len())
	// }
}

// if traceScanner.Err() != nil {
// 	log.Fatal(traceScanner.Err())
// }
