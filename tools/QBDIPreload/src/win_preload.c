#include "QBDIPreload.h"
#include <Windows.h>
#include <winnt.h>

/* Consts */
static const unsigned long INST_INT1 = 0x01CD;          /* Instruction opcode for "INT 1" */
static const unsigned long INST_INT1_MASK = 0xFFFF;
static const size_t QBDI_RUNTIME_STACK_SIZE = 0x800000; /* QBDI shadow stack size         */
static const long MEM_PAGE_SIZE = 4096;                 /* Default page size on Windows   */

/* Trampoline spec (to be called from assembly stub) */
void qbdipreload_trampoline();

/* Globals */
static struct {
    void* va;
    uint64_t orig_bytes;
} g_EntryPointInfo;                     /* Main module EntryPoint (PE from host process)        */
static PVOID g_hExceptionHandler;       /* VEH for QBDI preload internals (break on EntryPoint) */
static rword g_firstInstructionVA;      /* First instruction that will be executed by QBDI      */
static rword g_lastInstructionVA;       /* Last instruction that will be executed by QBDI       */
PVOID g_shadowStackTop;                 /* QBDI shadow stack top pointer(decreasing address)    */
static GPRState g_EntryPointGPRState;   /* QBDI CPU GPR states when EntryPoint has been reached */
static FPRState g_EntryPointFPRState;   /* QBDI CPU FPR states when EntryPoint has been reached */



/*
 * Conversion from windows CONTEXT ARCH dependent structure 
 * to QBDI GPR state (Global purpose registers)
 */
void qbdipreload_threadCtxToGPRState(const void* gprCtx, GPRState* gprState) {
    PCONTEXT x64cpu = (PCONTEXT) gprCtx;

    gprState->rax = x64cpu->Rax;
    gprState->rbx = x64cpu->Rbx;
    gprState->rcx = x64cpu->Rcx;
    gprState->rdx = x64cpu->Rdx;
    gprState->rsi = x64cpu->Rsi;
    gprState->rdi = x64cpu->Rdi;
    gprState->rbp = x64cpu->Rbp;
    gprState->rsp = x64cpu->Rsp;
    gprState->r8 = x64cpu->R8;
    gprState->r9 = x64cpu->R9;
    gprState->r10 = x64cpu->R10;
    gprState->r11 = x64cpu->R11;
    gprState->r12 = x64cpu->R12;
    gprState->r13 = x64cpu->R13;
    gprState->r14 = x64cpu->R14;
    gprState->r15 = x64cpu->R15;
    gprState->rip = x64cpu->Rip;
    gprState->eflags = x64cpu->EFlags;
}

/*
 * Conversion from windows CONTEXT ARCH dependent structure 
 * to QBDI FPR state (Floating point registers)
 */
void qbdipreload_floatCtxToFPRState(const void* gprCtx, FPRState* fprState) {
     PCONTEXT x64cpu = (PCONTEXT) gprCtx;

    // FPU STmm(X)
    memcpy(&fprState->stmm0, &x64cpu->FltSave.FloatRegisters[0], sizeof(MMSTReg));
    memcpy(&fprState->stmm1, &x64cpu->FltSave.FloatRegisters[1], sizeof(MMSTReg));
    memcpy(&fprState->stmm2, &x64cpu->FltSave.FloatRegisters[2], sizeof(MMSTReg));
    memcpy(&fprState->stmm3, &x64cpu->FltSave.FloatRegisters[3], sizeof(MMSTReg));
    memcpy(&fprState->stmm4, &x64cpu->FltSave.FloatRegisters[4], sizeof(MMSTReg));
    memcpy(&fprState->stmm5, &x64cpu->FltSave.FloatRegisters[5], sizeof(MMSTReg));
    memcpy(&fprState->stmm6, &x64cpu->FltSave.FloatRegisters[6], sizeof(MMSTReg));
    memcpy(&fprState->stmm7, &x64cpu->FltSave.FloatRegisters[7], sizeof(MMSTReg));

    // XMM(X) registers
    memcpy(&fprState->xmm0, &x64cpu->Xmm0, 16);
    memcpy(&fprState->xmm1, &x64cpu->Xmm1, 16);
    memcpy(&fprState->xmm2, &x64cpu->Xmm2, 16);
    memcpy(&fprState->xmm3, &x64cpu->Xmm3, 16);
    memcpy(&fprState->xmm4, &x64cpu->Xmm4, 16);
    memcpy(&fprState->xmm5, &x64cpu->Xmm5, 16);
    memcpy(&fprState->xmm6, &x64cpu->Xmm6, 16);
    memcpy(&fprState->xmm7, &x64cpu->Xmm7, 16);
    memcpy(&fprState->xmm8, &x64cpu->Xmm8, 16);
    memcpy(&fprState->xmm9, &x64cpu->Xmm9, 16);
    memcpy(&fprState->xmm10, &x64cpu->Xmm10, 16);
    memcpy(&fprState->xmm11, &x64cpu->Xmm11, 16);
    memcpy(&fprState->xmm12, &x64cpu->Xmm12, 16);
    memcpy(&fprState->xmm13, &x64cpu->Xmm13, 16);
    memcpy(&fprState->xmm14, &x64cpu->Xmm14, 16);
    memcpy(&fprState->xmm15, &x64cpu->Xmm15, 16);

    // Others FPU registers
    fprState->rfcw = x64cpu->FltSave.ControlWord;
    fprState->rfsw = x64cpu->FltSave.StatusWord;
    fprState->ftw  = x64cpu->FltSave.TagWord;
    fprState->rsrv1 = x64cpu->FltSave.Reserved1;
    fprState->ip =  x64cpu->FltSave.ErrorOffset;
    fprState->cs =  x64cpu->FltSave.ErrorSelector;
    fprState->rsrv2 = x64cpu->FltSave.Reserved2;
    fprState->dp =  x64cpu->FltSave.DataOffset;
    fprState->ds =  x64cpu->FltSave.DataSelector;
    fprState->rsrv3 = x64cpu->FltSave.Reserved3;
    fprState->mxcsr = x64cpu->FltSave.MxCsr;
    fprState->mxcsrmask =  x64cpu->FltSave.MxCsr_Mask;
 }

/*
 * Write an "int 1" instruction at given address
 * Save previous byte to internal buffer
 * return 0 in case of failure
 */
int setInt1Exception(void* fn_va) {
     DWORD oldmemprot;

    if (!fn_va){
        return 0;
    }

    uintptr_t base = (uintptr_t) fn_va - ((uintptr_t) fn_va % MEM_PAGE_SIZE);
    
    g_EntryPointInfo.va = fn_va;
    if(!VirtualProtect((void*) base, MEM_PAGE_SIZE, PAGE_READWRITE, &oldmemprot))
        return 0;
    g_EntryPointInfo.orig_bytes = *(uint64_t*) fn_va;
    *(uint64_t*) fn_va = INST_INT1 | (g_EntryPointInfo.orig_bytes & (~(uint64_t)INST_INT1_MASK));
    
    return VirtualProtect((void*) base, MEM_PAGE_SIZE, oldmemprot, &oldmemprot) != 0;
}

/*
 * Restore original bytes on a previously installed "int 1" instruction
 * return 0 in case of failure
 */
int unsetInt1Exception() {
	DWORD oldmemprot;
    uintptr_t base = (uintptr_t) g_EntryPointInfo.va - ((uintptr_t) g_EntryPointInfo.va % MEM_PAGE_SIZE);

    if(!VirtualProtect((void*) base, MEM_PAGE_SIZE, PAGE_READWRITE, &oldmemprot))
        return 0;
    
    *(uint64_t*) g_EntryPointInfo.va = g_EntryPointInfo.orig_bytes;
    
    return VirtualProtect((void*) base, MEM_PAGE_SIZE, oldmemprot, &oldmemprot) != 0;
}

/*
 * Remove a previously installed vectored exception handler
 * Return 0 in case of failure
 */
int unsetExceptionHandler(LONG (*exception_filter_fn)(PEXCEPTION_POINTERS)) {
    return RemoveVectoredExceptionHandler(exception_filter_fn);
}

/*
 * Install a vectored exception handler
 * Return 0 in case of failure
 */
int setExceptionHandler(LONG (*exception_filter_fn)(PEXCEPTION_POINTERS)) {
    
    g_hExceptionHandler = AddVectoredExceptionHandler(1, exception_filter_fn);
    return g_hExceptionHandler != NULL;
}

/*
 * Trampoline implementation
 * It removes exception handler, restore entry point bytes and 
 * setup QBDI runtime for host target
 * Its is called from separate qbdipreload_trampoline() assembly stub that
 * makes this function load in a arbitraty allocated stack, then QBDI can
 * safely initialize & instrument main target thread
 */
void qbdipreload_trampoline_impl() {
    unsetInt1Exception();
    unsetExceptionHandler(g_hExceptionHandler);

    int status = qbdipreload_on_main(0, NULL);
    
    if(status == QBDIPRELOAD_NOT_HANDLED) {
        VMInstanceRef vm;
        qbdi_initVM(&vm, NULL, NULL);
    
        // Filter some modules to avoid conflicts
        qbdi_removeAllInstrumentedRanges(vm);

        // Set original states
        qbdi_setGPRState(vm, &g_EntryPointGPRState);
        qbdi_setFPRState(vm, &g_EntryPointFPRState);

        status = qbdipreload_on_run(vm, g_firstInstructionVA, g_lastInstructionVA);
    }

    // Exiting early must be done as qbdipreload_trampoline_impl
    // is executed on fake stack without any caller
    // It will trigger DLL_PROCESS_DETACH event in DLLMain()
    ExitProcess(status);
}

/*
 * Windows QBDI preload specific excetion handler
 * It must be uninstalled once used one time
 * The handler catches the first INT1 exception
 */
LONG WINAPI QbdiPreloadExceptionFilter(PEXCEPTION_POINTERS exc_info) {
    PCONTEXT x64cpu = exc_info->ContextRecord;

    // Sanity check on exception
    if((exc_info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) &&
        (x64cpu->Rip == (DWORD64) g_EntryPointInfo.va)) {
        // Call user provided callback with x64 cpu state (specific to windows)
        int status = qbdipreload_on_premain((void*) x64cpu, (void*) x64cpu);

        // Convert windows CPU context to QBDIGPR/FPR states
        qbdipreload_threadCtxToGPRState(x64cpu, &g_EntryPointGPRState);
        qbdipreload_floatCtxToFPRState(x64cpu, &g_EntryPointFPRState);

        // First instruction to execute is main module entry point)
        g_firstInstructionVA = QBDI_GPR_GET(&g_EntryPointGPRState, REG_PC);

        // Last instruction to execute is inside windows PE loader
        // (inside BaseThreadInitThunk() who called PE entry point & set RIP on stack) 
        g_lastInstructionVA =  *((rword*) QBDI_GPR_GET(&g_EntryPointGPRState, REG_SP));

        if (status == QBDIPRELOAD_NOT_HANDLED) {
            // Allocate shadow stack & keep some space at the end for QBDI runtime
            g_shadowStackTop = VirtualAlloc(NULL, QBDI_RUNTIME_STACK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            g_shadowStackTop = (PVOID) ((uint64_t) g_shadowStackTop + QBDI_RUNTIME_STACK_SIZE - 0x1008);
        }

        // Continue execution on trampoline to make QBDI runtime
        // execute using a separate stack and not instrumented target one
        x64cpu->Rip = (uint64_t) qbdipreload_trampoline;
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

/*
 * Get main module entry point
 */
void* getMainModuleEntryPoint() {
    PIMAGE_DOS_HEADER mainmod_imgbase = (PIMAGE_DOS_HEADER) GetModuleHandle(NULL);
    PIMAGE_NT_HEADERS  mainmod_nt = (PIMAGE_NT_HEADERS) ((uint8_t*)mainmod_imgbase + mainmod_imgbase->e_lfanew);
    
    return (void*) ((uint8_t*) mainmod_imgbase + mainmod_nt->OptionalHeader.AddressOfEntryPoint);
    
}

/*
 * Unused on windows as DllMain() already provides
 * code execution at start
 */
int qbdipreload_hook_init() {
    return QBDIPRELOAD_NO_ERROR;
}

/*
 * Hooking based on int1 instruction + exception handler
 * Return 0 in case of failure
 */
int qbdipreload_hook(void* va) {
    if(!setInt1Exception(va)) {
        return 0;
    }

    return !setExceptionHandler(QbdiPreloadExceptionFilter);
}

/*
 * QBDI windows preload installation is done automatically
 * through DLLMain() once the QBDI instrumentation module
 * is loaded inside target (e.g. with LoadLibrary)
 */
BOOLEAN WINAPI DllMain(IN HINSTANCE hDllHandle, DWORD nReason, LPVOID Reserved)
{
    if(nReason == DLL_PROCESS_ATTACH) {
        void* mainmod_entry_point = getMainModuleEntryPoint();
        // Call user provided callback
        int status = qbdipreload_on_start(mainmod_entry_point);
        if (status == QBDIPRELOAD_NOT_HANDLED) {
            // QBDI preload installation
            qbdipreload_hook(mainmod_entry_point);
        }
    }
    else if(nReason == DLL_PROCESS_DETACH) {
        DWORD dwExitCode;

        GetExitCodeProcess(GetCurrentProcess(), &dwExitCode);
        qbdipreload_on_exit(dwExitCode);
    }

    return TRUE;
}
