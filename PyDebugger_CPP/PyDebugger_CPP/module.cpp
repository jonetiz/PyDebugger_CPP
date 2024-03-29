// CPP Debug Handling for Python Binding with pybind11

#include <pybind11/pybind11.h>
#include <iostream>
#include <Windows.h>
#include <Tlhelp32.h>
#include <winternl.h>
#include <thread>

namespace py = pybind11;

DWORD UpdateBreakpointsOnThreads(DWORD dwProcessID, DWORD64 addr, int index) // Walk all threads and set Dr0-Dr3 breakpoints as designated by respective params addr1 - addr4
{
	THREADENTRY32 te = { sizeof(THREADENTRY32) };
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, dwProcessID); // Get a snapshot of all threads in the process

	if (Thread32First(hSnapshot, &te))
		while (Thread32Next(hSnapshot, &te)) // Walk all threads captured in the snapshot
			if (te.th32OwnerProcessID == dwProcessID) // ensure the thread is still owned by our process
			{
				HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID); // Open the thread

				CONTEXT ctx;
				ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (GetThreadContext(hThread, &ctx)) // Get ctx and set Dr0 through Dr3 to the correspondent addresses based on index
				{
					// DWORD64 dFinalDr7[4];
					if (index == 0) {
						ctx.Dr0 = addr;
					}
					if (index == 1) {
						ctx.Dr1 = addr;
					}
					if (index == 2) {
						ctx.Dr2 = addr;
					}
					if (index == 3) {
						ctx.Dr3 = addr;
					}

					ctx.Dr7 = (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6); // Set Dr7 register to enable hardware breakpoints

					SetThreadContext(hThread, &ctx); // Re "save" ctx to the thread
				}

				CloseHandle(hThread); // Close our handle on the thread so it resumes
				hThread = NULL;
			}
	return NULL;
}

BOOL SetDebugPrivilege(BOOL State) // Toggle process debug privileges
{
	HANDLE hToken;
	TOKEN_PRIVILEGES token_privileges;
	DWORD dwSize;

	ZeroMemory(&token_privileges, sizeof(token_privileges));
	token_privileges.PrivilegeCount = 1;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &hToken))
		return FALSE;

	if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &token_privileges.Privileges[0].Luid))
	{
		CloseHandle(hToken);
		return FALSE;
	}

	if (State)
		token_privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	else
		token_privileges.Privileges[0].Attributes = SE_PRIVILEGE_REMOVED;

	if (!AdjustTokenPrivileges(hToken, FALSE, &token_privileges, 0, NULL, &dwSize))
	{
		CloseHandle(hToken);
		return FALSE;
	}

	return CloseHandle(hToken);
}

class PythonContext { // Class used to hold a CONTEXT object and convert to/from python dictionary (dictionaries are faster than classes, which is why it's used)
	public:
		CONTEXT ctxOriginal;
		py::dict ctxPython;
		PythonContext(CONTEXT ctx) {
			ctxOriginal = ctx; // Only worrying about GPRs
			//x86 GPRs
			ctxPython["Rax"] = ctx.Rax;
			ctxPython["Rcx"] = ctx.Rcx;
			ctxPython["Rdx"] = ctx.Rdx;
			ctxPython["Rbx"] = ctx.Rbx;
			ctxPython["Rsp"] = ctx.Rsp;
			ctxPython["Rbp"] = ctx.Rbp;
			ctxPython["Rsi"] = ctx.Rsi;
			ctxPython["Rdi"] = ctx.Rdi;
			//x86_64 GPRs
			ctxPython["R8"] = ctx.R8;
			ctxPython["R9"] = ctx.R9;
			ctxPython["R10"] = ctx.R10;
			ctxPython["R11"] = ctx.R11;
			ctxPython["R12"] = ctx.R12;
			ctxPython["R13"] = ctx.R13;
			ctxPython["R14"] = ctx.R14;
			ctxPython["R15"] = ctx.R15;
		}
		CONTEXT ContextFromDict() {
			CONTEXT ctxOut = ctxOriginal; // Set ctxOut to same values as ctxOriginal

			ctxOut.Rax = ctxPython["Rax"].cast<uint64_t>();
			ctxOut.Rcx = ctxPython["Rcx"].cast<uint64_t>();
			ctxOut.Rdx = ctxPython["Rdx"].cast<uint64_t>();
			ctxOut.Rbx = ctxPython["Rbx"].cast<uint64_t>();
			ctxOut.Rsp = ctxPython["Rsp"].cast<uint64_t>();
			ctxOut.Rbp = ctxPython["Rbp"].cast<uint64_t>();
			ctxOut.Rsi = ctxPython["Rsi"].cast<uint64_t>();
			ctxOut.Rdi = ctxPython["Rdi"].cast<uint64_t>();
			ctxOut.R8 = ctxPython["R8"].cast<uint64_t>();
			ctxOut.R9 = ctxPython["R9"].cast<uint64_t>();
			ctxOut.R10 = ctxPython["R10"].cast<uint64_t>();
			ctxOut.R11 = ctxPython["R11"].cast<uint64_t>();
			ctxOut.R12 = ctxPython["R12"].cast<uint64_t>();
			ctxOut.R13 = ctxPython["R13"].cast<uint64_t>();
			ctxOut.R14 = ctxPython["R14"].cast<uint64_t>();
			ctxOut.R15 = ctxPython["R15"].cast<uint64_t>();

			return ctxOut;
		}
};

class Breakpoint
{
	public:
		DWORD64 address; // Memory of the breakpoint
		py::function callback; // A python function passed by reference that will be executed when the breakpoint is hit
		BYTE originalByte = 0x00; // The original byte of the instruction (for handling software breakpoints)
		Breakpoint() {
			address = 0x0;
			callback = py::function();
		}
		Breakpoint(DWORD64 addr, py::function cb) {
			address = addr; // Breakpoint address
			callback = cb; // What to do when hit
		}
		Breakpoint(DWORD64 addr, BYTE byOriginal, py::function cb) {
			address = addr; // Breakpoint address
			callback = cb; // What to do when hit
			originalByte = byOriginal; // The original byte of the instruction (for software breakpoints)
		}
		CONTEXT CallbackWithContext(CONTEXT& ctx) { // Run callback() passing context of breakpoint as arg and return the altered context

			// Begin python interpretation
			py::gil_scoped_acquire acquire;

			PythonContext ctxPassed = PythonContext(ctx);

			callback(&ctxPassed.ctxPython);

			CONTEXT ctxRecieved = ctxPassed.ContextFromDict();

			// End python interpretation
			py::gil_scoped_release release;

			return ctxRecieved;
		}
};

// Comparison operator for Breakpoint == Breakpoint; just see if address is the same.
bool operator==(Breakpoint const& lhs, Breakpoint const& rhs)
{
	if (lhs.address == rhs.address)
		return true;
	else
		return false;
}

class DebugHandler
{
	public:
		DWORD pid;
		Breakpoint hwBreakpoints[4]; // 4 Hardware Breakpoints Available per thread on x86 Architecture
		std::vector<Breakpoint> swBreakpoints; // Theoretically unlimited number of software breakpoints

		bool bHandleBreakpoints = false; // false halts loop

		DebugHandler(DWORD p)
		{
			pid = p;
		}

		void CreateHardwareBreakpoint(int i, Breakpoint b) {
			DebugHandler::hwBreakpoints[i] = b; // Add the passed breakpoint to the hwBreakpoints array at the given index
		}

		void CreateSoftwareBreakpoint(Breakpoint b) {
			DebugHandler::swBreakpoints.push_back(b); // Add the passed software breakpoint to the swBreakpoints vector
		}
		void DeleteSoftwareBreakpoint(DWORD64 addr) {
			// TODO: Remove from swBreakpoints
		}

		void SetSoftwareBreakpoint(Breakpoint b) {
			HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, false, DebugHandler::pid);

			BYTE int3 = 0xCC;
			WriteProcessMemory(hProcess, (void*)b.address, &int3, sizeof(unsigned char), NULL); // Write 0xCC to the breakpoint's address

			CloseHandle(hProcess);
		}

		void RemoveSoftwareBreakpoint(Breakpoint b) {
			for (Breakpoint& breakpoint : DebugHandler::swBreakpoints) {
				if (breakpoint == b) {
					HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, false, DebugHandler::pid);
					WriteProcessMemory(hProcess, (void*)breakpoint.address, &breakpoint.originalByte, sizeof(unsigned char), NULL); // Write the original memory to the breakpoint's address (remove 0xCC)
					CloseHandle(hProcess);

				}
			}
		}

		void StartHandlingBreakpoints() {
			py::gil_scoped_release release; // Release GIL to allow this thread to execute python
			bHandleBreakpoints = true;
			UpdateBreakpoints(); // Update breakpoints on target process
			HandleBreakpoints(); // Begin handling breakpoints
			py::gil_scoped_acquire acquire; // Reacquire GIL to maintain integrity
		}

		void UpdateBreakpoints() { // "set" the hardware breakpoints
			SetDebugPrivilege(true); // Give this module debug privileges
			HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, false, DebugHandler::pid);
			if (DebugActiveProcess(DebugHandler::pid)) // Attempt to attach to the process (only one process may be attached to another at a time)
			{
				if (DebugSetProcessKillOnExit != NULL) // Make it so the attached process does not close when this module exits/detaches
					DebugSetProcessKillOnExit(false);

				int counter = 0; // Keep track of array index and debug register
				for (Breakpoint& breakpoint : DebugHandler::hwBreakpoints) { // Set hardware breakpoints on debug registers
					if (breakpoint.address != 0x0) {
						UpdateBreakpointsOnThreads(DebugHandler::pid, breakpoint.address, counter); // Set hardware breakpoints on all threads
					}
					counter += 1;
				}

				DebugActiveProcessStop(DebugHandler::pid); // Detach from process
			}

			CloseHandle(&hProcess);
		}

		void HandleBreakpoints() { // Handle breakpoints (debug process, wait for breakpoint, run python callback, continue)
			SetDebugPrivilege(true); // Obtain debug privileges
			HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, false, DebugHandler::pid);
			if (DebugActiveProcess(DebugHandler::pid)) // Attempt to debug the process
			{
				if (DebugSetProcessKillOnExit != NULL) // Prevent the debugee from closing when the debugger exits/detaches
					DebugSetProcessKillOnExit(false);

				DEBUG_EVENT dbgEvent;
				HANDLE hThread = NULL;
				BOOL bContinueDebugging = false;

				while (DebugHandler::bHandleBreakpoints) // bHandleBreakpoints = false will break this loop
				{
					for (Breakpoint& breakpoint : DebugHandler::swBreakpoints) { // Set software breakpoints on each iteration (we have to rewrite original memory to allow execution)
						DebugHandler::SetSoftwareBreakpoint(breakpoint);
					}

					WaitForDebugEvent(&dbgEvent, INFINITE); // Listen indefinitely for debug event

					switch (dbgEvent.dwDebugEventCode)
					{
					case EXCEPTION_DEBUG_EVENT: // When we get a dbgEvent
						if (dbgEvent.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP) // Hardware breakpoint is triggered
						{
							for (Breakpoint& breakpoint : DebugHandler::hwBreakpoints) { // Foreach breakpoint in array check if this is the one
								if (dbgEvent.u.Exception.ExceptionRecord.ExceptionAddress == (void*)breakpoint.address)
								{
									if (hThread = OpenThread(THREAD_ALL_ACCESS, false, dbgEvent.dwThreadId))
									{
										dbgEvent.u.Exception.ExceptionRecord.ExceptionFlags = 0;

										CONTEXT ctx;
										ctx.ContextFlags = CONTEXT_FULL;

										// stop the thread for continuing to run while we check the breakpoints
										//DWORD dwSuspended = SuspendThread(&hThread);

										// get the context of the thread
										BOOL bGetThreadContext = GetThreadContext(hThread, &ctx);

										// run the python callback, passing the current CONTEXT object
										CONTEXT callback = breakpoint.CallbackWithContext(ctx);

										ctx = callback;

										ctx.EFlags |= 0x10000;

										// set the context so our changes are made
										BOOL bSetThreadContext = SetThreadContext(hThread, &ctx);

										// resume the thread so program continues to run
										//DWORD dwResumed = ResumeThread(&hThread);

										BOOL bCloseHandle = CloseHandle(&hThread);

										bContinueDebugging = true;
									}
								}
							}
						}
						else if (dbgEvent.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) // Software breakpoint is triggered
						{
							for (Breakpoint& breakpoint : DebugHandler::swBreakpoints) { // Foreach software breakpoint in array check if this is the one
								if (dbgEvent.u.Exception.ExceptionRecord.ExceptionAddress == (void*)breakpoint.address) {
									DebugHandler::RemoveSoftwareBreakpoint(breakpoint);
									if (hThread = OpenThread(THREAD_ALL_ACCESS, false, dbgEvent.dwThreadId))
									{
										dbgEvent.u.Exception.ExceptionRecord.ExceptionFlags = 0;

										CONTEXT ctx;
										ctx.ContextFlags = CONTEXT_FULL;

										// get the context of the thread
										GetThreadContext(hThread, &ctx);

										// run the python callback, passing the current CONTEXT object
										CONTEXT callback = breakpoint.CallbackWithContext(ctx);

										ctx = callback;
										// Go back an instruction to run the proper code
										ctx.Rip--;
										// Set the breakpoint back and continue
										ctx.EFlags |= 0x10000;

										// set the context so our changes are made
										SetThreadContext(hThread, &ctx);

										BOOL bCloseHandle = CloseHandle(&hThread);
									}
									bContinueDebugging = true;
								}
							}
						}

						if (bContinueDebugging)
						{
							BOOL bContinue = ContinueDebugEvent(dbgEvent.dwProcessId, dbgEvent.dwThreadId, DBG_CONTINUE);
							bContinueDebugging = false;
						}
						else
							ContinueDebugEvent(dbgEvent.dwProcessId, dbgEvent.dwThreadId, DBG_EXCEPTION_NOT_HANDLED);

						break;
					default:
						ContinueDebugEvent(dbgEvent.dwProcessId, dbgEvent.dwThreadId, DBG_EXCEPTION_NOT_HANDLED);
						break;
					}
				}
			}
			DebugActiveProcessStop(DebugHandler::pid);
			CloseHandle(hProcess);
		}

		void Stop() {
			//Set HandleBreakpoints to false so while loop terminates
			DebugHandler::bHandleBreakpoints = false;

			// Remove breakpoints
			DebugHandler::hwBreakpoints[0] = Breakpoint();
			DebugHandler::hwBreakpoints[1] = Breakpoint();
			DebugHandler::hwBreakpoints[2] = Breakpoint();
			DebugHandler::hwBreakpoints[3] = Breakpoint();
			UpdateBreakpoints();
		}
};

DebugHandler CurrentDebugger = DebugHandler(NULL); // The currently instantiated randomizer object (for access from python)

void CreateDebugger(DWORD pid) { // Called from python to give python handle on the DebugHandler
	CurrentDebugger = DebugHandler(pid);
}

DebugHandler* AccessDebugger() { // Returns the DebugHandler being used
	return &CurrentDebugger;
}

PYBIND11_MODULE(PyDebugger_CPP, m)
{
	/*m.def("fn_name_py", &fn_name_cpp, R"pbdoc(
		Docstring
	)pbdoc");*/

	m.def("create_debugger", &CreateDebugger, R"pbdoc(
        Instantiates a DebugHandler object and places into CurrentDebugger.
    )pbdoc");
	m.def("access_debugger", &AccessDebugger, R"pbdoc(
        Accesses the DebugHandler object currently in CurrentDebugger.
    )pbdoc");

	py::class_<Breakpoint>(m, "Breakpoint")
		.def(py::init<DWORD64&, py::function&>(), R"pbdoc(
			Hardware Breakpoint
		)pbdoc")
		.def(py::init<DWORD64&, BYTE&, py::function&>(), R"pbdoc(
			Software Breakpoint
		)pbdoc");
	py::class_<DebugHandler>(m, "debugger_obj_cpp")
		.def(py::init<DWORD&>())
		.def("create_hardware_breakpoint", &DebugHandler::CreateHardwareBreakpoint)
		.def("create_software_breakpoint", &DebugHandler::CreateSoftwareBreakpoint, R"pbdoc(
			Set a software breakpoint on the passed address.
		)pbdoc")
		.def("remove_software_breakpoint", &DebugHandler::RemoveSoftwareBreakpoint, R"pbdoc(
			Remove the software breakpoint on the passed address.
		)pbdoc")
		.def("start_handling_breakpoints", &DebugHandler::StartHandlingBreakpoints, R"pbdoc(
			Begin handling breakpoints; sets them on all threads of the process.
		)pbdoc")
		.def("stop", &DebugHandler::Stop, R"pbdoc(
			Stop handling breakpoints, should kill CPP thread.
		)pbdoc")
		.def_readonly("pid", &DebugHandler::pid);

#ifdef VERSION_INFO
	m.attr("__version__") = VERSION_INFO;
#else
	m.attr("__version__") = "DEV";
#endif
}
