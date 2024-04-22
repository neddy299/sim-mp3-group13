//go:build ignore

// Dynamic Instruction Scheduling Simulator

// Usage:  sim <S> <N> <tracefile> [experiment]
// Example: ./sim 2 8 val_trace_gcc.txt 3

// <S> is the Scheduling Queue size
// <N> is the peak fetch and dispatch rate, issue rate will be up to N+1 and
// <tracefile> is the filename
// [experiment] optional experiment number to run (Valid range 1 to 4)

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

// Experimental settings
int experiment = 0;

typedef struct Node {
  void* data;
  struct Node* prev;
  struct Node* next;
} Node;

typedef struct List {
  Node* head;
  Node* tail;
  int len;
} List;

int listLen(List* list) {
  return list ? list->len : 0;
}

// Add a new node to the end of the given list
void listAdd(List* list, void* data) {
  if(list) {
    Node* node = (Node*)malloc(sizeof(Node));
    node->data = data;
    node->prev = NULL;
    node->next = NULL;

    if(!list->head) {
      list->head = node;
      list->tail = node;
    } else {
      list->tail->next = node;
      node->prev = list->tail;
      list->tail = node;
    }

    list->len++;
  }
}

// Remove a node from the list
void listRemove(List* list, Node* node) {
  if(list) {
    Node* item = list->head;

    while(item) {
      if(item == node) {
        Node* tmp = item;
        if(tmp == list->head) {
          list->head = tmp->next;
        } else {
          tmp->prev->next = tmp->next;
        }

        if(tmp->next) tmp->next->prev = tmp->prev;

        list->len--;
        if(list->tail == tmp) {
          list->tail = tmp->prev;
        }

        free(tmp);
        break;
      }

      item = item->next;
    }
  }
}

// Delete entire list, freeing underlying data if list owns the data
void listDelete(List* list, bool ownData) {
  Node* node = list->head;

  while(node) {
    Node* next = node->next;
    if(ownData) {
      free(node->data);
    }

    free(node);
    node = next;
  }
}

// Wrapping snprintf into a fn to dynamically create string
// safer than sprintf, won't overflow -> need to release mem
char* formatString(const char* format, ...) {
  va_list args;
  va_start(args, format);

  va_list argscpy;
  va_copy(argscpy, args);

  // Retrieve bytes so we know how much space to allocate
  size_t bytes = vsnprintf(NULL, 0, format, argscpy) + 1;
  va_end(argscpy);

  char* str = (char*)malloc(bytes);

  vsnprintf(str, bytes, format, args);

  va_end(args);

  return str;
}

// Debug settings
int devMode = false;               // print out extra information such as stage and instruction progress (disable for final submission)
int earlyExitCycleLimit = 0;       // execute specified amount of clock cycles and then terminate (set to 0 to disable)
int enableIssueProfiling = false;   // profile order of issued instructions
const char* outputPrefix = "_out"; // output debug filename prefix
const char* outputSuffix = "txt";  // output debug file extension
const char* logCSVReport = "_results_exp"; // Execution results CSV report

//1) Define 5 states that an instruction can be in (e.g., use an enumerated
//  type): IF (fetch), ID (dispatch), IS (issue), EX (execute), WB (writeback).

enum instructionType {
  iTypeIF, // fetch
  iTypeID, // dispatch
  iTypeIS, // issue
  iTypeEX, // execute
  iTypeWB  // writeback
};

const char* instructionName[] = { "IF", "ID", "IS", "EX", "WB" };

// The operation type of an instruction indicates its execution latency:
// Type 0 has a latency of 1 cycle, Type 1 has a latency of 2 cycles, and Type 2 has a latency of 5 cycles
const int operandLatency[] = { 1, 2, 5 };

//2) Define a circular FIFO that holds all active instructions in their program order.
//   ... AKA Fake Re-Order Buffer (ROB)
//   Each entry in the fake-ROB should be a data structure containing per
//   instruction information, for example, state of the instruction (which stage
//    it is in), operation type, operand state, sequence number (tag), etc.
typedef struct instruction {
  enum instructionType iTypeState; // instruction type state: IF, ID, ...
  int opType;                      // operation type - is either "0", "1", or "2".
  int tag;                         // sequence number (auto increment when read)
  int startCycle[5];               // cycle that the instruction started per instruction type
  int endCycle[5];                 // cycle that the instruction completed per instruction type
  int rs;                          // reservation station number for this operation

  char pc[64];                     // program counter
  int destReg;                     // destination register
  int src1Reg;                     // source 1 register
  int src2Reg;                     // source 2 register
} instruction;

List ROB = { NULL, NULL, 0 };

//3) Define 3 lists:
// a) dispatch_list : This contains a list of instructions in either the IF or ID
//   state. The dispatch_list models the Dispatch Queue. (By including both
//   the IF and ID states, we don’t need to separately model the pipeline
//   registers of the fetch and dispatch stages.)

List dispatch_list = { NULL, NULL, 0 };

// b) issue_list : This contains a list of instructions in the IS state (waiting for
//   operands or available issue bandwidth). The issue_list models the
//   Scheduling Queue.

List issue_list = { NULL, NULL, 0 };

// c) execute_list : This contains a list of instructions in the EX state (waiting
//   for the execution latency of the operation). The execute_list models the FUs.

List execute_list = { NULL, NULL, 0 };

// instruction_list is the complete list of instructions (in order) processed by the simulation
List instruction_list = { NULL, NULL, 0 };

// registerStatus - track busy registers, not concerned with the actual loading and storing of values
typedef struct registerStatus {
  int Qi; // reservation station number that will produce the result that will be stored at this register location
  int Count; // reference counter for experiments
  int Count2; // reference counter for experiments
} registerStatus;

// Registers between 0 and 127
#define REGISTER_STATUS_LEN 128
registerStatus RegisterStat[REGISTER_STATUS_LEN];

// Tomasulo reservation station
typedef struct reservationStation {
  int Op;   // operation to perform (functional unit)
  int Qj;   // reservation station that will produce source 1
  int Qk;   // reservation station that will produce source 2
  int Vj;   // value of source 1 operand (V? values and regs are not implemented in this sim)
  int Vk;   // value of source 2 operand (V? values and regs are not implemented in this sim)
  int A;    // memory address calculation (unused by this sim)
  int Busy; // reservation station is busy
} reservationStation;

// Reservation Stations
#define RESERVATION_STATIONS 1000
reservationStation RS[RESERVATION_STATIONS];

// Globals
int SchedulingQueueSize; // <S> Scheduling Queue size
int NPeakFetch;          // <N> Peak Fetch and Dispatch Rate
int dispatchQueueMax;    // Dispatch Queue is 2N instructions in size
int numInstructions;     // number of instructions
int numCycles;           // number of cycles
int tagSequence;         // tag sequence counter
int traceEOF;            // reached trace End Of File

// Debug/Trace globals
FILE* outputFile = NULL;
FILE* trace = NULL;

// Function declarations
void FakeRetire();
void Execute();
void Issue();
void Dispatch();
void Fetch(FILE* traceFile);

int Advance_Cycle();

instruction* readTrace(const char* line);

void incrementStage(instruction* i);

void printTask(const char* taskName);
void printInstruction(const instruction* i, const char* note);

void printInstructionTiming(instruction* i);

int getAvailableReservationStation();

void regRefCount(int reg, int inc, int destinationRegister);
void sortInstructions(instruction* list[], int len);
void prioritySort(instruction* list[], int len);

void profileIssue(instruction* list[], int);
void writeCSVReport(const char* traceFileName, int SchedulingQueueSize, int NPeakFetch, int numInstructions, int numCycles, double IPC);

// Free dynamically allocated memory
void freeData() {
  listDelete(&dispatch_list, false);
  listDelete(&issue_list, false);
  listDelete(&execute_list, false);
  listDelete(&ROB, false);

  listDelete(&instruction_list, true);
}

int main(int argc, const char* argv[]) {
  numInstructions = 0;
  numCycles = 0;
  tagSequence = 0;
  traceEOF = false;

  // Parse command line arguments
  if(argc < 4) {
    fprintf(stderr, "Usage: %s <S> <N> <tracefile> [experimentNumber]\n", argv[0]);
    return 1;
  } else if(argc == 5) {
    if((experiment = atoi(argv[4])) == 0) {
      if (!(strlen(argv[4]) == 1 && argv[4][0] == '0')) {  
        fprintf(stderr, "Invalid experiment '%s' for parameter <experiment> is not an integer\n", argv[4]);
        return 1;
      }
    }
  }

  if((SchedulingQueueSize = atoi(argv[1])) == 0) {
    fprintf(stderr, "Invalid type: '%s' for parameter <S> is not an integer\n", argv[1]);
    return 1;
  }

  if((NPeakFetch = atoi(argv[2])) == 0) {
    fprintf(stderr, "Invalid type: '%s' for parameter <N> is not an integer\n", argv[2]);
    return 1;
  }

  dispatchQueueMax = NPeakFetch * 2;
  const char* traceFileName = argv[3];

  // Open trace file
  trace = fopen(traceFileName, "r");
  if(trace == NULL) {
    fprintf(stderr, "Error opening trace file\n");
    return 1;
  }

  // Debug logging
  if(devMode) {
    printf("Scheduling Queue size(S): %d\nPeak fetch and dispatch rate(N): %d\nTrace file: %s\n",
           SchedulingQueueSize, NPeakFetch, traceFileName);

    char* outputFileName = formatString("%s_%d_%d_%s.%s",
                                outputPrefix, SchedulingQueueSize, NPeakFetch, "test", outputSuffix);

    outputFile = fopen(outputFileName, "w+");

    free(outputFileName);
    if(outputFile == NULL) {
      fprintf(stderr, "unable to open output file: %s", outputFileName);
    }
  }

  // 4) Call each pipeline stage in reverse order in your main simulator loop, as
  // follows. The detailed comments indicate tasks to be performed. The order
  // among these tasks is important.

  do {
    FakeRetire();
    Execute();
    Issue();
    Dispatch();
    Fetch(trace);
  } while(Advance_Cycle());

  fclose(trace);

  // Display final output
  Node* node = instruction_list.head;
  while(node) {
    printInstructionTiming((instruction*)node->data);
    node = node->next;
  }

  // Instructions completed Per Cycle
  double IPC = (double)numInstructions / (double)numCycles;

  printf("number of instructions = %d\n", numInstructions);
  printf("number of cycles       = %d\n", numCycles);
  printf("IPC                    = %.5f\n", IPC);

  if(outputFile) {
    fprintf(outputFile, "number of instructions = %d\n", numInstructions);
    fprintf(outputFile, "number of cycles       = %d\n", numCycles);
    fprintf(outputFile, "IPC                    = %.5f\n", IPC);
    fclose(outputFile);
  }

  writeCSVReport(traceFileName, SchedulingQueueSize, NPeakFetch, numInstructions, numCycles, IPC);

  freeData();

  return 0;
}

// FakeRetire();
// // Remove instructions from the head of the fake-ROB
// // until an instruction is reached that is not in the WB state.
void FakeRetire() {
  printTask("FakeRetire");

  if(listLen(&ROB) > 0) {
    Node* e = ROB.head;
    while(e) {
      Node* next = e->next;

      //1) Remove the instruction from the ROB.
      if(((instruction*)(e->data))->iTypeState == iTypeWB) {
        printInstruction((instruction*)e->data, "FakeRetire remove");
        listRemove(&ROB, e);
      } else break;

      e = next;
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
void Execute() {
  printTask("Execute");

  if(listLen(&execute_list) > 0) {
    Node* e = execute_list.head;
    while(e) {
      Node* next = e->next;

      instruction* i = (instruction*)(e->data);
      char* note = formatString("Exec %d of %d",
                                numCycles - i->startCycle[i->iTypeState], operandLatency[i->opType]);

      if(numCycles < i->startCycle[i->iTypeState] + operandLatency[i->opType]) {
        printInstruction(i, note);
      } else {
        // Functional Unit execution has finished
        incrementStage(i);
        // This is the final stage and will not be incremented further, we must set end cycle here
        i->endCycle[i->iTypeState] = i->startCycle[i->iTypeState] + 1;

        char* str = formatString("%s EX -> WB", note);
        printInstruction(i, str);
        free(str);

        // 3) Update the register file state e.g., ready flag)
        //    and wakeup dependent instructions (set their operand ready flags).

        // Tomasulo Stage 3: write result
        int r = i->rs;

        for(int x = 0; x < REGISTER_STATUS_LEN; ++x) {
          if(RegisterStat[x].Qi == r) {
            RegisterStat[x].Qi = 0;
          }
        }

        for(int x = 0; x < RESERVATION_STATIONS; ++x) {
          if(RS[x].Qj == r) {
            RS[x].Qj = 0;
          }

          if(RS[x].Qk == r) {
            RS[x].Qk = 0;
          }
        }

        RS[r].Busy = false;

        // decrement register ref counters
        regRefCount(i->destReg, false, true);
        regRefCount(i->src1Reg, false, false);
        regRefCount(i->src2Reg, false, false);

        //1) Remove the instruction from the execute_list.
        listRemove(&execute_list, e);
      }

      e = next;

      free(note);
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
// //    instructions in the scheduling queue)
// // 4) Set a timer in the instruction’s data structure that will allow you to model
// //    the execution latency.
void Issue() {
  printTask("Issue");

  // From the issue_list, construct a temp list of instructions whose
  // operands are ready – these are the READY instructions.
  if(listLen(&issue_list) > 0) {
    instruction* temp_list[listLen(&issue_list)];
    int list_len = 0;

    Node* e = issue_list.head;
    while(e) {
      instruction* i = (instruction*)e->data;

      // Tomasulo Stage 2: execute
      if(RS[i->rs].Qj == 0 && RS[i->rs].Qk == 0) {
        temp_list[list_len++] = i;
      }

      e = e->next;
    }

    // Scan the READY instructions in ascending order of tags
    prioritySort(temp_list, list_len);

    profileIssue(temp_list, list_len);

    for(int issueCt = 0; issueCt < list_len; ++issueCt) {
      instruction* i = temp_list[issueCt];
      if(issueCt >= NPeakFetch + 1) {
        printInstruction(i, "Issue stalled");
        continue;
      }

      // and issue up to N+1 of them
      incrementStage(i); // IS -> EX

      // // 4) Set a timer in the instruction’s data structure that will allow you to model
      // //    the execution latency.
      // NOTE: timer is started with i.startCycle[i.iTypeState] and execution is monitored in Execute()

      printInstruction(i, "Issue IS -> EX");

      //1) Remove the instruction from the issue_list and add it to the execute_list.
      listAdd(&execute_list, i);

      Node* d = issue_list.head;
      while(d) {
        Node* next = d->next;

        if((instruction*)d->data == i) listRemove(&issue_list, d);

        d = next;
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
void Dispatch() {
  printTask("Dispatch");

  if(listLen(&dispatch_list) > 0) {
    instruction* temp_list[listLen(&dispatch_list)];
    int list_len = 0;

    Node* e = dispatch_list.head;
    while(e) {
      instruction* i = (instruction*)(e->data);

      switch(i->iTypeState) {
        case iTypeIF:
          incrementStage(i); // IF -> ID  (models the 1 cycle latency for instruction fetch)
          printInstruction(i, "Dispatch IF -> ID");
          break;

        case iTypeID:
          temp_list[list_len++] = i;
          break;

        default:
          break;
      }

      e = e->next;
    }

    // Scan the temp list in ascending order of tags
    sortInstructions(temp_list, list_len);

    for(int j = 0; j < list_len; ++j) {
      instruction* i = temp_list[j];
      if(listLen(&issue_list) >= SchedulingQueueSize) {
        printInstruction(i, "Dispatch stalled");
        continue;
      }

      // if the scheduling queue is not full, then:
      incrementStage(i); // ID -> IS

      // 3) Rename source operands by looking up state in the register file;
      //    Rename destination by updating state in the register file.

      // Tomasulo Stage 1: issue
      int r = getAvailableReservationStation();
      int rd = i->destReg;
      int rs = i->src1Reg;
      int rt = i->src2Reg;

      if(rs != -1) {
        if(RegisterStat[rs].Qi != 0) {
          RS[r].Qj = RegisterStat[rs].Qi;
        } else {
          RS[r].Qj = 0;
        }
      }

      if(rt != -1) {
        if(RegisterStat[rt].Qi != 0) {
          RS[r].Qk = RegisterStat[rt].Qi;
        } else {
          RS[r].Qk = 0;
        }
      }

      RS[r].Busy = true;
      if(rd != -1) {
        RegisterStat[rd].Qi = r;
      }

      // increment register ref counters
      regRefCount(rd, true, true);
      regRefCount(rs, true, false);
      regRefCount(rt, true, false);

      RS[r].Op = i->opType;
      i->rs = r;

      printInstruction(i, "Dispatch ID -> IS");

      //1) Remove the instruction from the dispatch_list and add it to the issue_list.
      listAdd(&issue_list, i);

      Node* d = dispatch_list.head;
      while(d) {
        Node* next = d->next;

        if((instruction*)d->data == i) listRemove(&dispatch_list, d);

        d = next;
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
void Fetch(FILE* traceFile) {
  printTask("Fetch");
  int fetchCt = 0;

  // 2) the fetch bandwidth is not exceeded, and
  // 3) the dispatch queue is not full.
  while(fetchCt < NPeakFetch &&
        listLen(&dispatch_list) < dispatchQueueMax) {
    char line[1024];

    if(fgets(line, 1024, traceFile)) {
      instruction* i = readTrace(line);
      char* note = formatString("Fetch %d of %d", fetchCt + 1, NPeakFetch);
      printInstruction(i, note);
      free(note);

      fetchCt++;
      numInstructions++;

      // 1) Push the new instruction onto the fake-ROB
      listAdd(&ROB, i);

      // 2) Add the instruction to the dispatch_list
      listAdd(&dispatch_list, i);

      // 1) you have not reached the end-of-file
      listAdd(&instruction_list, i);
    } else {
      traceEOF = true;
      return;
    }
  }
}

//  // Advance_Cycle performs several functions.
//  // It advances the simulator cycle. Also, when it becomes
//  // known that the fake-ROB is empty AND the trace is
//  // depleted, the function returns “false” to terminate
//  // the loop.
int Advance_Cycle() {
  printTask("Advance_Cycle");

  if(traceEOF && listLen(&ROB) == 0) {
    return false;
  }

  numCycles++;

  if(earlyExitCycleLimit > 0 && numCycles >= earlyExitCycleLimit) {
    printf("[EARLY EXIT - SANITY CHECK]\n");
    return false;
  }

  return true;
}

// readTrace() reads a line from the trace file and returns an instruction
// The simulator reads a trace file in the following format:
// <PC> <operation type> <dest reg #> <src1 reg #> <src2 reg #>
// Fatal error handling only, application halts on invalid input
instruction* readTrace(const char* line) {
  char pc[64];
  int opType, destReg, src1Reg, src2Reg;
  int read = sscanf(line, "%s %i %i %i %i", pc, &opType, &destReg, &src1Reg, &src2Reg);
  if(read != 5) {
    fprintf(stderr, "Error parsing trace file err: expected 5 parameters, got %d. line: %s\n",
            read, line);
    freeData();
    exit(1);
  }

  int tag = tagSequence;
  tagSequence++;

  instruction* i = (instruction*)malloc(sizeof(instruction));
  *i = (instruction) {
    .iTypeState = iTypeIF,
    .opType = opType,
    .tag = tag,
    .startCycle = { 0 },
    .endCycle = { 0 },
    .rs = -1,

    .pc = { 0 },
    .destReg = destReg,
    .src1Reg = src1Reg,
    .src2Reg = src2Reg,
  };

  strcpy(i->pc, pc);

  i->startCycle[iTypeIF] = numCycles;
  i->endCycle[iTypeIF] = numCycles;

  return i;
}

// printInstructionTiming - prints functional unit and instruction stage timing values
// Output format:
//
// 0 fu{0} src{29,14} dst{-1} IF{0,1} ID{1,1} IS{2,1} EX{3,1} WB{4,1}
void printInstructionTiming(instruction* i) {
  char* output = formatString("%d fu{%d} src{%d,%d} dst{%d} IF{%d,%d} ID{%d,%d} IS{%d,%d} EX{%d,%d} WB{%d,%d}\n",
                              i->tag, i->opType, i->src1Reg, i->src2Reg, i->destReg, i->startCycle[0],
                              i->endCycle[0]-i->startCycle[0], i->startCycle[1], i->endCycle[1]-i->startCycle[1],
                              i->startCycle[2], i->endCycle[2]-i->startCycle[2], i->startCycle[3], i->endCycle[3]-i->startCycle[3],
                              i->startCycle[4], i->endCycle[4]-i->startCycle[4]);

  printf("%s", output);

  if(outputFile != NULL) {
    fprintf(outputFile, "%s", output);
  }

  free(output);
}

void incrementStage(instruction* i) {
  i->endCycle[i->iTypeState] = numCycles;
  i->iTypeState++;
  i->startCycle[i->iTypeState] = numCycles;
}

void regRefCount(int reg, int inc, int destinationRegister) {
  if(reg != -1) {
    if(experiment == 2 && destinationRegister) {
      if(inc) RegisterStat[reg].Count2++;
      else RegisterStat[reg].Count2--;
    }

    if(inc) RegisterStat[reg].Count++;
    else RegisterStat[reg].Count--;
  }
}

// experiment Type: register reference counter - basic
// - Add up reference counts for all registers and return weighted value
// - Use a fair weight for all registers
int experiment1(instruction* i) {
  int w = 0;

  if(i->destReg != -1) {
    w += RegisterStat[i->destReg].Count;
  }
  if(i->src1Reg != -1) {
    w += RegisterStat[i->src1Reg].Count;
  }
  if(i->src2Reg != -1) {
    w += RegisterStat[i->src2Reg].Count;
  }

  return -w;
}

// experiment Type: register reference counter - prioritize destination registers
// - Add up reference counts for all registers and return weighted value
// - Use a greater weight for destination registers
int experiment2(instruction* i) {
  int w = 0;

  if(i->destReg != -1) {
    w += RegisterStat[i->destReg].Count2 * 3;
  }
  if(i->src1Reg != -1) {
    w += RegisterStat[i->src1Reg].Count;
  }
  if(i->src2Reg != -1) {
    w += RegisterStat[i->src2Reg].Count;
  }

  return -w;
}

// experiment Type: prioritize SLOW instructions
// - Process most expensive instructions first
// - Use tag number as tie breaker
int experiment3(instruction* i) {
  return i->tag - (operandLatency[i->opType] * SchedulingQueueSize);
}

// experiment Type: prioritize FAST instructions
// - Process least expensive instructions first
// - Use tag number as tie breaker
int experiment4(instruction* i) {
  return i->tag + (operandLatency[i->opType] * SchedulingQueueSize);
}

// experiment Type: register reference counter - prioritize source register
// - Return weighted value for number of total source/target references
int experiment5(instruction* i) {
  int w = 0;

  if(i->src1Reg != -1) {
    w += RegisterStat[i->src1Reg].Count;
  }
  if(i->src2Reg != -1) {
    w += RegisterStat[i->src2Reg].Count;
  }

  return w;
}

int expWeight(instruction* i) {
  switch(experiment) {
    case 1:
      return experiment1(i);
    case 2:
      return experiment2(i);
    case 3:
      return experiment3(i);
    case 4:
      return experiment4(i);
    case 5:
      return experiment5(i);
  }

  return i->tag;
}

void profileIssue(instruction* list[], int len) {
  if(!enableIssueProfiling) {
    return;
  }

  printf("!issue-order (%4d): ", numCycles);
  if(len == 0) {
    printf("%s\n", "empty");
  } else {
    for(int i = 0; i < len; ++i) {
      if(i == len - 1) {
        printf("%i", list[i]->tag);
      } else {
        printf("%i,", list[i]->tag);
      }
    }
    
    printf("\n");
  }
}

void prioritySort(instruction* list[], int len) {
  for(int i = 1; i < len; ++i) {
    int j = i - 1;

    instruction* ins = list[i];

    while(j >= 0 && expWeight(list[j]) > expWeight(ins)) {
      list[j + 1] = list[j];
      j--;
    }

    list[j + 1] = ins;
  }
}

void sortInstructions(instruction* list[], int len) {
  for(int i = 1; i < len; ++i) {
    int j = i - 1;

    instruction* ins = list[i];

    while(j >= 0 && list[j]->tag > ins->tag) {
      list[j + 1] = list[j];
      j--;
    }

    list[j + 1] = ins;
  }
}

// Debug instructions follow

void printInstruction(const instruction* i, const char* note) {
  if(devMode) {
    printf("- %-20s - tag: %5d (%s) pc: %s op: %d dst: %2d src1: %2d src2: %2d rs: %d\n",
           note, i->tag, instructionName[i->iTypeState], i->pc,
           i->opType, i->destReg, i->src1Reg, i->src2Reg, i->rs);
  }
}

void printTask(const char* taskName) {
  if(devMode) {
    printf("%-20s - dispatch: %d(%d) issue: %d(%d) execute: %d ROB: %d cycle: %d\n",
           taskName, listLen(&dispatch_list), dispatchQueueMax, listLen(&issue_list), SchedulingQueueSize,
           listLen(&execute_list), listLen(&ROB), numCycles);
  }
}

int getAvailableReservationStation() {
  for(int i = 0; i < RESERVATION_STATIONS; ++i) {
    if(i == 0) {
      continue;  // skip element 0 as a Qj/Qk value of 0 indicates that source operand is available
    }

    if(!RS[i].Busy) {
      RS[i].Op = -1;
      RS[i].Qj = 0;
      RS[i].Qk = 0;
      RS[i].Vj = 0;
      RS[i].Vk = 0;
      RS[i].A = 0;
      return i;
    }
  }

  fprintf(stderr, "no free reservation station available\n");
  freeData();
  exit(1);

  return -1;
}

void writeCSVReport(const char* traceFileName, int SchedulingQueueSize, int NPeakFetch, int numInstructions, int numCycles, double IPC) {
  FILE* reportFile = NULL;
  bool newReport = false;
  char* logFileName = formatString("%s%d.csv", logCSVReport, experiment);

  if (reportFile = fopen(logFileName, "r")) {
    fclose(reportFile);    
  } else {
    newReport = true;
  }

  reportFile = fopen(logFileName, "a+");
  if(reportFile == NULL) {
    fprintf(stderr, "unable to open CSV report file: %s", logFileName);
    return;
  }

  if (newReport) {
		fprintf(reportFile, "traceFileName,SchedulingQueueSize,NPeakFetch,numInstructions,numCycles,IPC\n");
	}

	fprintf(reportFile, "%s,%d,%d,%d,%d,%f\n", traceFileName, SchedulingQueueSize, NPeakFetch, numInstructions, numCycles, IPC);  
	fclose(reportFile);
}
