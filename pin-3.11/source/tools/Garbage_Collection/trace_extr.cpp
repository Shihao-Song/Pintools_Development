#include <fstream>
#include <iostream>
#include <string>

#include <unordered_map>

#include "pin.H"

// For creating directory.
#include <sys/types.h>
#include <sys/stat.h>

/* ===================================================================== */
/* Names of malloc and free */
/* ===================================================================== */
#if defined(TARGET_MAC)
#define MALLOC "_malloc"
#define FREE "_free"
#else
#define MALLOC "malloc"
#define FREE "free"
#endif

// Data trace output
using std::ofstream;
ofstream trace_out;
KNOB<std::string> TraceOut(KNOB_MODE_WRITEONCE, "pintool",
    "o", "", "specify output trace file name");

BOOL FollowChild(CHILD_PROCESS childProcess, VOID * userData)
{
    INT appArgc;
    CHAR const * const * appArgv;

    CHILD_PROCESS_GetCommandLine(childProcess, &appArgc, &appArgv);
    std::string childApp(appArgv[0]);

    std::cerr << std::endl;
    std::cerr << "[Pintool] Warning: Child application to execute: " << childApp << std::endl;
    std::cerr << "[Pintool] Warning: We do not run Pin under the child process." << std::endl;
    std::cerr << std::endl;
    return FALSE;
}

static bool entering_roi = false;

PIN_LOCK pinLock;
static const uint64_t LIMIT = 1000000000; // Maximum of instructions (all threads) 
                                          // to be extracted.
static uint64_t insn_count = 0; // Track how many instructions we have already instrumented.
static void increCount(THREADID t_id) 
{
    if (!entering_roi) { return; }

    PIN_GetLock(&pinLock, t_id + 1);

    assert(entering_roi == true);
    
    ++insn_count; // Increment
    // Exit if it exceeds a threshold.
    if (insn_count >= LIMIT)
    {
        // std::cerr << "Done trace extraction." << std::endl;
        std::cerr << "[PINTOOL] End trace extraction." << std::endl;
        std::cerr << "[PINTOOL] Instruction count is reached " << insn_count
                  << std::endl;
        trace_out << std::flush;
	trace_out.close();
        exit(0);
        // PIN_ExitApplication(0);
    }

    PIN_ReleaseLock(&pinLock);
}

// Thread local data
class thread_data_t
{
  public:
    thread_data_t() {}

    unsigned num_exes_before_mem_or_bra = 0;
};

static TLS_KEY tls_key = INVALID_TLS_KEY;

INT32 numThreads = 0;
VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
    numThreads++;
    thread_data_t* tdata = new thread_data_t;
    if (PIN_SetThreadData(tls_key, tdata, threadid) == FALSE)
    {
        std::cerr << "PIN_SetThreadData failed" << std::endl;
        PIN_ExitProcess(1);
    }
}

VOID ThreadFini(THREADID threadIndex, const CONTEXT *ctxt, INT32 code, VOID *v)
{
    thread_data_t* tdata = static_cast<thread_data_t*>(PIN_GetThreadData(tls_key, threadIndex));
    delete tdata;
}

static void nonBranchNorMem(THREADID t_id)
{
    if (entering_roi) { return; }

    thread_data_t* t_data = static_cast<thread_data_t*>(PIN_GetThreadData(tls_key, t_id));

    // Only increment thread data.
    ++(t_data->num_exes_before_mem_or_bra);
}

static void memTrace(THREADID t_id,
                     ADDRINT eip,
                     bool is_store,
                     ADDRINT mem_addr,
                     UINT32 payload_size)
{
    if (entering_roi) { return; }

    thread_data_t* t_data = static_cast<thread_data_t*>(PIN_GetThreadData(tls_key, t_id));
    
    // Lock the print out
    PIN_GetLock(&pinLock, t_id + 1);
/*
    trace_out << t_id << " "
              << t_data->num_exes_before_mem_or_bra << " "
              << eip << " ";
    if (is_store)
    {
        trace_out << "S ";
    }
    else
    {
        trace_out << "L ";
    }
    trace_out << mem_addr << std::endl;
*/
    PIN_ReleaseLock(&pinLock);
    
    t_data->num_exes_before_mem_or_bra = 0;
}

#define ROI_BEGIN    (1025)
#define ROI_END      (1026)
void HandleMagicOp(THREADID t_id, ADDRINT op)
{
    switch (op)
    {
        case ROI_BEGIN:
            PIN_GetLock(&pinLock, t_id + 1);
            entering_roi = true;
            PIN_ReleaseLock(&pinLock);
            // std::cout << "Captured roi_begin() \n";
            return;
        case ROI_END:
            PIN_GetLock(&pinLock, t_id + 1);
            entering_roi = false;
            PIN_ReleaseLock(&pinLock);
            // std::cout << "Captured roi_end() \n";
            return;
    }
}

// "Main" function: decode and simulate the instruction
static void instructionSim(INS ins)
{
    // Count number of instructions.
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)increCount, IARG_THREAD_ID, IARG_END);

    if (INS_IsMemoryRead (ins) || INS_IsMemoryWrite (ins))
    {
        for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
        {
            if (INS_MemoryOperandIsRead(ins, i))
            {
                INS_InsertPredicatedCall(
                    ins,
                    IPOINT_BEFORE,
                    (AFUNPTR)memTrace,
		    IARG_THREAD_ID,
                    IARG_ADDRINT, INS_Address(ins),
                    IARG_BOOL, FALSE,
                    IARG_MEMORYOP_EA, i,
                    IARG_UINT32, INS_MemoryOperandSize(ins, i),
                    IARG_END);
            }

            if (INS_MemoryOperandIsWritten(ins, i))
            {
                INS_InsertPredicatedCall(
                    ins,
                    IPOINT_BEFORE,
                    (AFUNPTR)memTrace,
		    IARG_THREAD_ID,
                    IARG_ADDRINT, INS_Address(ins),
                    IARG_BOOL, TRUE,
                    IARG_MEMORYOP_EA, i,
                    IARG_UINT32, INS_MemoryOperandSize(ins, i),
                    IARG_END);
            }
        }
    }
    else if (INS_IsXchg(ins) &&
             INS_OperandReg(ins, 0) == REG_RCX &&
             INS_OperandReg(ins, 1) == REG_RCX)
    {
        INS_InsertCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR) HandleMagicOp,
            IARG_THREAD_ID,
            IARG_REG_VALUE, REG_ECX,
            IARG_END);
    }
    else
    {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)nonBranchNorMem, IARG_THREAD_ID, IARG_END);
    }
}

static void traceCallback(TRACE trace, VOID *v)
{
    BBL bbl_head = TRACE_BblHead(trace);

    for (BBL bbl = bbl_head; BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        for(INS ins = BBL_InsHead(bbl); ; ins = INS_Next(ins))
        {
            instructionSim(ins);
            if (ins == BBL_InsTail(bbl))
            {
                break;
            }
        }
    }
}

// Capture malloc() and free()
std::unordered_map<ADDRINT, size_t> heap; // <pointer, size>
size_t last_malloc_size;

VOID MallocBefore(CHAR *s, ADDRINT size)
{
    if (!entering_roi) { return; }
    last_malloc_size = size;
}
 
VOID FreeBefore(CHAR *s, ADDRINT size)
{
    if (!entering_roi) { return; }
    typeof(heap.begin()) iter;
    if ((iter = heap.find(size)) != heap.end())
    {
        trace_out << "FREE " << size << " " << iter->second << std::endl;
        heap.erase(iter);
    }
}

VOID MallocAfter(ADDRINT ret)
{
    if (!entering_roi) { return; }
    if (ret != 0) 
    {
        heap[ret] = last_malloc_size;
        trace_out << "MALLOC " << ret << " " << last_malloc_size << std::endl;
    }
}

VOID Image(IMG img, VOID *v)
{
    // Instrument the malloc() and free() functions.  Print the input argument
    // of each malloc() or free(), and the return value of malloc().
    //
    //  Find the malloc() function.
    RTN mallocRtn = RTN_FindByName(img, MALLOC);
    if (RTN_Valid(mallocRtn))
    {
        RTN_Open(mallocRtn);

        // Instrument malloc() to print the input argument value and the return value.
        RTN_InsertCall(mallocRtn, IPOINT_BEFORE, (AFUNPTR)MallocBefore,
                       IARG_ADDRINT, MALLOC,
                       IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                       IARG_END);
        RTN_InsertCall(mallocRtn, IPOINT_AFTER, (AFUNPTR)MallocAfter,
                       IARG_FUNCRET_EXITPOINT_VALUE, IARG_END);

        RTN_Close(mallocRtn);
    }

    // Find the free() function.
    RTN freeRtn = RTN_FindByName(img, FREE);
    if (RTN_Valid(freeRtn))
    {
        RTN_Open(freeRtn);
        // Instrument free() to print the input argument value.
        RTN_InsertCall(freeRtn, IPOINT_BEFORE, (AFUNPTR)FreeBefore,
                       IARG_ADDRINT, FREE,
                       IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                       IARG_END);
        RTN_Close(freeRtn);
    }
}

int
main(int argc, char *argv[])
{
    PIN_InitLock(&pinLock);
    tls_key = PIN_CreateThreadDataKey(NULL);
    if (tls_key == INVALID_TLS_KEY)
    {
        std::cerr << "number of already allocated keys reached the MAX_CLIENT_TLS_KEYS limit" 
                  << std::endl;
        PIN_ExitProcess(1);
    }

    PIN_InitSymbols(); // Initialize all the PIN API functions

    // Initialize PIN, e.g., process command line options
    if(PIN_Init(argc,argv))
    {
        return 1;
    }
    assert(!TraceOut.Value().empty());

    trace_out.open(TraceOut.Value().c_str());

    // Register ThreadStart to be called when a thread starts.
    PIN_AddThreadStartFunction(ThreadStart, NULL);

    // Register Fini to be called when thread exits.
    PIN_AddThreadFiniFunction(ThreadFini, NULL);

    PIN_AddFollowChildProcessFunction(FollowChild, 0);

    // RTN_AddInstrumentFunction(routineCallback, 0);
    // Simulate each instruction, to eliminate overhead, we are using Trace-based call back.
    // IMG_AddInstrumentFunction(Image, 0);
    IMG_AddInstrumentFunction(Image, 0);
    TRACE_AddInstrumentFunction(traceCallback, 0);

    /* Never returns */
    PIN_StartProgram();

    return 0;
}