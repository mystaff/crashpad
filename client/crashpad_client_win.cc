// Copyright 2015 The Crashpad Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "client/crashpad_client.h"

#include <string.h>
#include <windows.h>

#include "base/atomicops.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string16.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "util/file/file_io.h"
#include "util/win/command_line.h"
#include "util/win/critical_section_with_debug_info.h"
#include "util/win/registration_protocol_win.h"
#include "util/win/scoped_handle.h"

namespace {

// This handle is never closed. This is used to signal to the server that a dump
// should be taken in the event of a crash.
HANDLE g_signal_exception = INVALID_HANDLE_VALUE;

// Where we store the exception information that the crash handler reads.
crashpad::ExceptionInformation g_crash_exception_information;

// These handles are never closed. g_signal_non_crash_dump is used to signal to
// the server to take a dump (not due to an exception), and the server will
// signal g_non_crash_dump_done when the dump is completed.
HANDLE g_signal_non_crash_dump = INVALID_HANDLE_VALUE;
HANDLE g_non_crash_dump_done = INVALID_HANDLE_VALUE;

// Guards multiple simultaneous calls to DumpWithoutCrash(). This is leaked.
base::Lock* g_non_crash_dump_lock;

// Where we store a pointer to the context information when taking a non-crash
// dump.
crashpad::ExceptionInformation g_non_crash_exception_information;

// A CRITICAL_SECTION initialized with
// RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO to force it to be allocated with a
// valid .DebugInfo field. The address of this critical section is given to the
// handler. All critical sections with debug info are linked in a doubly-linked
// list, so this allows the handler to capture all of them.
CRITICAL_SECTION g_critical_section_with_debug_info;

LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exception_pointers) {
  // Tracks whether a thread has already entered UnhandledExceptionHandler.
  static base::subtle::AtomicWord have_crashed;

  // This is a per-process handler. While this handler is being invoked, other
  // threads are still executing as usual, so multiple threads could enter at
  // the same time. Because we're in a crashing state, we shouldn't be doing
  // anything that might cause allocations, call into kernel mode, etc. So, we
  // don't want to take a critical section here to avoid simultaneous access to
  // the global exception pointers in ExceptionInformation. Because the crash
  // handler will record all threads, it's fine to simply have the second and
  // subsequent entrants block here. They will soon be suspended by the crash
  // handler, and then the entire process will be terminated below. This means
  // that we won't save the exception pointers from the second and further
  // crashes, but contention here is very unlikely, and we'll still have a stack
  // that's blocked at this location.
  if (base::subtle::Barrier_AtomicIncrement(&have_crashed, 1) > 1) {
    SleepEx(INFINITE, false);
  }

  // Otherwise, we're the first thread, so record the exception pointer and
  // signal the crash handler.
  g_crash_exception_information.thread_id = GetCurrentThreadId();
  g_crash_exception_information.exception_pointers =
      reinterpret_cast<crashpad::WinVMAddress>(exception_pointers);

  // Now signal the crash server, which will take a dump and then terminate us
  // when it's complete.
  SetEvent(g_signal_exception);

  // Time to wait for the handler to create a dump.
  const DWORD kMillisecondsUntilTerminate = 60 * 1000;

  // Sleep for a while to allow it to process us. Eventually, we terminate
  // ourselves in case the crash server is gone, so that we don't leave zombies
  // around. This would ideally never happen.
  Sleep(kMillisecondsUntilTerminate);

  LOG(ERROR) << "crash server did not respond, self-terminating";

  const UINT kCrashExitCodeNoDump = 0xffff7001;
  TerminateProcess(GetCurrentProcess(), kCrashExitCodeNoDump);

  return EXCEPTION_CONTINUE_SEARCH;
}

std::wstring FormatArgumentString(const std::string& name,
                                  const std::wstring& value) {
  return std::wstring(L"--") + base::UTF8ToUTF16(name) + L"=" + value;
}

}  // namespace

namespace crashpad {

CrashpadClient::CrashpadClient()
    : ipc_pipe_() {
}

CrashpadClient::~CrashpadClient() {
}

bool CrashpadClient::StartHandler(
    const base::FilePath& handler,
    const base::FilePath& database,
    const std::string& url,
    const std::map<std::string, std::string>& annotations,
    const std::vector<std::string>& arguments) {
  DCHECK(ipc_pipe_.empty());

  std::string ipc_pipe =
      base::StringPrintf("\\\\.\\pipe\\crashpad_%d_", GetCurrentProcessId());
  for (int index = 0; index < 16; ++index) {
    ipc_pipe.append(1, static_cast<char>(base::RandInt('A', 'Z')));
  }
  ipc_pipe_ = base::UTF8ToUTF16(ipc_pipe);

  std::wstring command_line;
  AppendCommandLineArgument(handler.value(), &command_line);
  for (const std::string& argument : arguments) {
    AppendCommandLineArgument(base::UTF8ToUTF16(argument), &command_line);
  }
  if (!database.value().empty()) {
    AppendCommandLineArgument(FormatArgumentString("database",
                                                   database.value()),
                              &command_line);
  }
  if (!url.empty()) {
    AppendCommandLineArgument(FormatArgumentString("url",
                                                   base::UTF8ToUTF16(url)),
                              &command_line);
  }
  for (const auto& kv : annotations) {
    AppendCommandLineArgument(
        FormatArgumentString("annotation",
                             base::UTF8ToUTF16(kv.first + '=' + kv.second)),
        &command_line);
  }
  AppendCommandLineArgument(FormatArgumentString("pipe-name", ipc_pipe_),
                            &command_line);

  STARTUPINFO startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup_info.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process_info;
  BOOL rv = CreateProcess(handler.value().c_str(),
                          &command_line[0],
                          nullptr,
                          nullptr,
                          true,
                          0,
                          nullptr,
                          nullptr,
                          &startup_info,
                          &process_info);
  if (!rv) {
    PLOG(ERROR) << "CreateProcess";
    return false;
  }

  rv = CloseHandle(process_info.hThread);
  PLOG_IF(WARNING, !rv) << "CloseHandle thread";

  rv = CloseHandle(process_info.hProcess);
  PLOG_IF(WARNING, !rv) << "CloseHandle process";

  return true;
}

bool CrashpadClient::SetHandlerIPCPipe(const std::wstring& ipc_pipe) {
  DCHECK(ipc_pipe_.empty());
  DCHECK(!ipc_pipe.empty());

  ipc_pipe_ = ipc_pipe;

  return true;
}

std::wstring CrashpadClient::GetHandlerIPCPipe() const {
  DCHECK(!ipc_pipe_.empty());
  return ipc_pipe_;
}

bool CrashpadClient::UseHandler() {
  DCHECK(!ipc_pipe_.empty());
  DCHECK_EQ(g_signal_exception, INVALID_HANDLE_VALUE);
  DCHECK_EQ(g_signal_non_crash_dump, INVALID_HANDLE_VALUE);
  DCHECK_EQ(g_non_crash_dump_done, INVALID_HANDLE_VALUE);
  DCHECK(!g_critical_section_with_debug_info.DebugInfo);

  ClientToServerMessage message;
  memset(&message, 0, sizeof(message));
  message.type = ClientToServerMessage::kRegister;
  message.registration.version = RegistrationRequest::kMessageVersion;
  message.registration.client_process_id = GetCurrentProcessId();
  message.registration.crash_exception_information =
      reinterpret_cast<WinVMAddress>(&g_crash_exception_information);
  message.registration.non_crash_exception_information =
      reinterpret_cast<WinVMAddress>(&g_non_crash_exception_information);

  // We create this dummy CRITICAL_SECTION with the
  // RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO flag set to have an entry point
  // into the doubly-linked list of RTL_CRITICAL_SECTION_DEBUG objects. This
  // allows us to walk the list at crash time to gather data for !locks. A
  // debugger would instead inspect ntdll!RtlCriticalSectionList to get the head
  // of the list. But that is not an exported symbol, so on an arbitrary client
  // machine, we don't have a way of getting that pointer.
  if (InitializeCriticalSectionWithDebugInfoIfPossible(
          &g_critical_section_with_debug_info)) {
    message.registration.critical_section_address =
        reinterpret_cast<WinVMAddress>(&g_critical_section_with_debug_info);
  }

  ServerToClientMessage response = {0};

  if (!SendToCrashHandlerServer(ipc_pipe_, message, &response)) {
    return false;
  }

  // The server returns these already duplicated to be valid in this process.
  g_signal_exception = reinterpret_cast<HANDLE>(
      static_cast<uintptr_t>(response.registration.request_crash_dump_event));
  g_signal_non_crash_dump = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(
      response.registration.request_non_crash_dump_event));
  g_non_crash_dump_done = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(
      response.registration.non_crash_dump_completed_event));

  g_non_crash_dump_lock = new base::Lock();

  // In theory we could store the previous handler but it is not clear what
  // use we have for it.
  SetUnhandledExceptionFilter(&UnhandledExceptionHandler);
  return true;
}

// static
void CrashpadClient::DumpWithoutCrash(const CONTEXT& context) {
  if (g_signal_non_crash_dump == INVALID_HANDLE_VALUE ||
      g_non_crash_dump_done == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << "haven't called UseHandler()";
    return;
  }

  // In the non-crashing case, we aren't concerned about avoiding calls into
  // Win32 APIs, so just use regular locking here in case of multiple threads
  // calling this function. If a crash occurs while we're in here, the worst
  // that can happen is that the server captures a partial dump for this path
  // because on the other thread gathering a crash dump, it TerminateProcess()d,
  // causing this one to abort.
  base::AutoLock lock(*g_non_crash_dump_lock);

  // Create a fake EXCEPTION_POINTERS to give the handler something to work
  // with.
  EXCEPTION_POINTERS exception_pointers = {0};

  // This is logically const, but EXCEPTION_POINTERS does not declare it as
  // const, so we have to cast that away from the argument.
  exception_pointers.ContextRecord = const_cast<CONTEXT*>(&context);

  // We include a fake exception and use a code of '0x517a7ed' (something like
  // "simulated") so that it's relatively obvious in windbg that it's not
  // actually an exception. Most values in
  // https://msdn.microsoft.com/en-us/library/windows/desktop/aa363082.aspx have
  // some of the top nibble set, so we make sure to pick a value that doesn't,
  // so as to be unlikely to conflict.
  const uint32_t kSimulatedExceptionCode = 0x517a7ed;
  EXCEPTION_RECORD record = {0};
  record.ExceptionCode = kSimulatedExceptionCode;
#if defined(ARCH_CPU_64_BITS)
  record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
#else
  record.ExceptionAddress = reinterpret_cast<void*>(context.Eip);
#endif  // ARCH_CPU_64_BITS

  exception_pointers.ExceptionRecord = &record;

  g_non_crash_exception_information.thread_id = GetCurrentThreadId();
  g_non_crash_exception_information.exception_pointers =
      reinterpret_cast<crashpad::WinVMAddress>(&exception_pointers);

  bool set_event_result = !!SetEvent(g_signal_non_crash_dump);
  PLOG_IF(ERROR, !set_event_result) << "SetEvent";

  DWORD wfso_result = WaitForSingleObject(g_non_crash_dump_done, INFINITE);
  PLOG_IF(ERROR, wfso_result != WAIT_OBJECT_0) << "WaitForSingleObject";
}

}  // namespace crashpad
