/**
 * @file tools/sunshinesvc.cpp
 * @brief Handles launching Sunshine.exe into user sessions as SYSTEM
 */
#define WIN32_LEAN_AND_MEAN
// MinGW's ToolHelp header depends on the Windows base types.
// clang-format off
#include <utility>
#include <Windows.h>
#include <TlHelp32.h>
// clang-format on
#include <userenv.h>
#include <wtsapi32.h>
#include <sddl.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "src/crash/crash.h"
#include "src/config_session_binding.h"
#include "sunshinesvc_lifecycle.h"
#include "sunshinesvc_log.h"
#include "sunshinesvc_state.h"

// PROC_THREAD_ATTRIBUTE_JOB_LIST is currently missing from MinGW headers
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
  #define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

constexpr DWORD invalid_session_id = 0xFFFFFFFF;
constexpr ULONGLONG session_reselect_delay_ms = 1000;

struct user_session_selection_t {
  DWORD session_id = invalid_session_id;
  bool query_failed = false;
};

SERVICE_STATUS_HANDLE service_status_handle;
SERVICE_STATUS service_status;
HANDLE stop_event;
HANDLE session_change_event;

#define SERVICE_NAME "SunshineService"

DWORD WINAPI
HandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
  switch (dwControl) {
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
      // Re-evaluate the interactive user session whenever the console or an
      // RDP session changes.  The physical console session is not necessarily
      // the session containing the signed-in user (for example, RDP can be
      // active while the console session is only Conn).
      switch (dwEventType) {
        case WTS_CONSOLE_CONNECT:
        case WTS_CONSOLE_DISCONNECT:
        case WTS_REMOTE_CONNECT:
        case WTS_REMOTE_DISCONNECT:
        case WTS_SESSION_LOGON:
        case WTS_SESSION_LOGOFF:
        case WTS_SESSION_LOCK:
        case WTS_SESSION_UNLOCK:
          SetEvent(session_change_event);
          break;
        default:
          break;
      }
      return NO_ERROR;

    case SERVICE_CONTROL_PRESHUTDOWN:
      // The system is shutting down
    case SERVICE_CONTROL_STOP:
      // Let SCM know we're stopping in up to 30 seconds
      service_status.dwCurrentState = SERVICE_STOP_PENDING;
      service_status.dwControlsAccepted = 0;
      service_status.dwWaitHint = 30 * 1000;
      SetServiceStatus(service_status_handle, &service_status);

      // Trigger ServiceMain() to start cleanup
      SetEvent(stop_event);
      return NO_ERROR;

    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

HANDLE
CreateJobObjectForChildProcess() {
  HANDLE job_handle = CreateJobObjectW(NULL, NULL);
  if (!job_handle) {
    return NULL;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limit_info = {};

  // Kill Sunshine.exe when the final job object handle is closed (which will happen if we terminate unexpectedly).
  // This ensures we don't leave an orphaned Sunshine.exe running with an inherited handle to our log file.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  // Allow Sunshine.exe to use CREATE_BREAKAWAY_FROM_JOB when spawning processes to ensure they can to live beyond
  // the lifetime of SunshineSvc.exe. This avoids unexpected user data loss if we crash or are killed.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;

  if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &job_limit_info, sizeof(job_limit_info))) {
    CloseHandle(job_handle);
    return NULL;
  }

  return job_handle;
}

LPPROC_THREAD_ATTRIBUTE_LIST
AllocateProcThreadAttributeList(DWORD attribute_count) {
  SIZE_T size;
  InitializeProcThreadAttributeList(NULL, attribute_count, 0, &size);

  auto list = (LPPROC_THREAD_ATTRIBUTE_LIST) HeapAlloc(GetProcessHeap(), 0, size);
  if (list == NULL) {
    return NULL;
  }

  if (!InitializeProcThreadAttributeList(list, attribute_count, 0, &size)) {
    HeapFree(GetProcessHeap(), 0, list);
    return NULL;
  }

  return list;
}

HANDLE
DuplicateTokenForSession(DWORD session_id) {
  HANDLE current_token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &current_token)) {
    return NULL;
  }

  // Duplicate our own LocalSystem token
  HANDLE new_token;
  if (!DuplicateTokenEx(current_token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &new_token)) {
    CloseHandle(current_token);
    return NULL;
  }

  CloseHandle(current_token);

  // Change the duplicated token to the console session ID
  if (!SetTokenInformation(new_token, TokenSessionId, &session_id, sizeof(session_id))) {
    CloseHandle(new_token);
    return NULL;
  }

  return new_token;
}

user_session_selection_t
FindActiveUserSessionId() {
  const auto console_session_id = WTSGetActiveConsoleSessionId();

  PWTS_SESSION_INFOW session_info = nullptr;
  DWORD session_count = 0;
  if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &session_info, &session_count)) {
    return { invalid_session_id, true };
  }

  DWORD active_rdp_session_id = invalid_session_id;
  DWORD active_user_session_id = invalid_session_id;
  for (DWORD index = 0; index < session_count; ++index) {
    const auto &session = session_info[index];
    if (session.State != WTSActive) {
      continue;
    }

    LPWSTR username = nullptr;
    DWORD bytes_returned = 0;
    const bool user_query_succeeded = WTSQuerySessionInformationW(
      WTS_CURRENT_SERVER_HANDLE,
      session.SessionId,
      WTSUserName,
      &username,
      &bytes_returned);
    const bool has_user = user_query_succeeded &&
                          username != nullptr &&
                          username[0] != L'\0';
    if (username != nullptr) {
      WTSFreeMemory(username);
    }
    if (!user_query_succeeded) {
      WTSFreeMemory(session_info);
      return { invalid_session_id, true };
    }
    if (!has_user) {
      continue;
    }

    // If the physical console itself has an active user, keep preferring it.
    if (session.SessionId == console_session_id) {
      const auto selected_session_id = session.SessionId;
      WTSFreeMemory(session_info);
      return { selected_session_id, false };
    }

    LPWSTR protocol = nullptr;
    bytes_returned = 0;
    USHORT protocol_type = 0;
    if (WTSQuerySessionInformationW(
          WTS_CURRENT_SERVER_HANDLE,
          session.SessionId,
          WTSClientProtocolType,
          &protocol,
          &bytes_returned)) {
      if (protocol != nullptr && bytes_returned >= sizeof(protocol_type)) {
        protocol_type = *reinterpret_cast<USHORT *>(protocol);
      }
      if (protocol != nullptr) {
        WTSFreeMemory(protocol);
      }
    }

    if (protocol_type == 2 && active_rdp_session_id == invalid_session_id) {
      active_rdp_session_id = session.SessionId;
    }
    if (active_user_session_id == invalid_session_id) {
      active_user_session_id = session.SessionId;
    }
  }

  WTSFreeMemory(session_info);
  if (active_rdp_session_id != invalid_session_id) {
    return { active_rdp_session_id, false };
  }
  return { active_user_session_id, false };
}

// Capture uses the console by default. `session_binding = 0` follows the
// active interactive user instead; GUI selection is independent.
std::wstring
InstallDirectory();

config_session_binding::mode_e
session_binding_mode() {
  const std::filesystem::path config_path = std::filesystem::path(InstallDirectory()) / L"config" / L"sunshine.conf";
  std::ifstream conf_file(config_path);
  if (!conf_file.is_open()) {
    return config_session_binding::default_mode;
  }
  std::string line;
  while (std::getline(conf_file, line)) {
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line.resize(comment);
    }
    const auto separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    const auto trim = [](std::string &text) {
      const auto not_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
      text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), not_space));
      text.erase(std::find_if_not(text.rbegin(), text.rend(), not_space).base(), text.end());
    };
    std::string key = line.substr(0, separator);
    std::string value = line.substr(separator + 1);
    trim(key);
    trim(value);
    if (key == config_session_binding::key) {
      return config_session_binding::parse_or_default(value);
    }
  }
  return config_session_binding::default_mode;
}

struct GuiAgentProcess {
  HANDLE handle = NULL;
  DWORD process_id = 0;
  ULONGLONG acquired_at_ms = 0;
};

struct GuiAgentLookup {
  HANDLE handle = NULL;
  DWORD process_id = 0;
  DWORD error = ERROR_SUCCESS;
};

void
CloseGuiAgentHandle(GuiAgentProcess &process) {
  if (process.handle != NULL) {
    CloseHandle(process.handle);
  }
  process = {};
}

DWORD
ResolveGuiAgentPath(std::wstring &gui_directory, std::wstring &gui_path) {
  std::wstring service_path(32768, L'\0');
  const auto service_path_length =
    GetModuleFileNameW(NULL, service_path.data(), static_cast<DWORD>(service_path.size()));
  if (service_path_length == 0) {
    const auto error = GetLastError();
    return error == ERROR_SUCCESS ? ERROR_BAD_PATHNAME : error;
  }
  if (service_path_length >= service_path.size()) {
    return ERROR_INSUFFICIENT_BUFFER;
  }
  service_path.resize(service_path_length);

  const auto tools_separator = service_path.find_last_of(L"\\/");
  if (tools_separator == std::wstring::npos || tools_separator == 0) {
    return ERROR_BAD_PATHNAME;
  }
  const auto install_separator = service_path.find_last_of(L"\\/", tools_separator - 1);
  if (install_separator == std::wstring::npos) {
    return ERROR_BAD_PATHNAME;
  }

  const auto install_directory = service_path.substr(0, install_separator);
  gui_directory = install_directory + L"\\assets\\gui";
  gui_path = gui_directory + L"\\sunshine-gui.exe";
  const auto gui_attributes = GetFileAttributesW(gui_path.c_str());
  if (gui_attributes == INVALID_FILE_ATTRIBUTES) {
    return GetLastError();
  }
  if (gui_attributes & FILE_ATTRIBUTE_DIRECTORY) {
    return ERROR_FILE_NOT_FOUND;
  }
  return ERROR_SUCCESS;
}

GuiAgentLookup
OpenExistingGuiAgent(DWORD console_session_id, const std::wstring &gui_path, DWORD exclude_process_id = 0) {
  GuiAgentLookup result;
  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    result.error = GetLastError();
    if (result.error == ERROR_SUCCESS) {
      result.error = ERROR_GEN_FAILURE;
    }
    return result;
  }

  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot, &entry)) {
    result.error = GetLastError();
    if (result.error == ERROR_NO_MORE_FILES) {
      result.error = ERROR_SUCCESS;
    }
    else if (result.error == ERROR_SUCCESS) {
      result.error = ERROR_GEN_FAILURE;
    }
    CloseHandle(snapshot);
    return result;
  }

  while (true) {
    if (_wcsicmp(entry.szExeFile, L"sunshine-gui.exe") == 0 && entry.th32ProcessID != exclude_process_id) {
      DWORD session_id = 0;
      if (ProcessIdToSessionId(entry.th32ProcessID, &session_id) && session_id == console_session_id) {
        auto candidate = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (candidate != NULL) {
          std::wstring candidate_path(32768, L'\0');
          DWORD candidate_path_length = static_cast<DWORD>(candidate_path.size());
          if (QueryFullProcessImageNameW(candidate, 0, candidate_path.data(), &candidate_path_length)) {
            candidate_path.resize(candidate_path_length);
            if (_wcsicmp(candidate_path.c_str(), gui_path.c_str()) == 0) {
              if (WaitForSingleObject(candidate, 0) == WAIT_OBJECT_0) {
                CloseHandle(candidate);
                candidate = NULL;
              }
              else {
                result.handle = candidate;
                result.process_id = entry.th32ProcessID;
                break;
              }
            }
          }
          if (candidate != NULL) {
            CloseHandle(candidate);
          }
        }
      }
    }

    if (!Process32NextW(snapshot, &entry)) {
      const auto error = GetLastError();
      if (error != ERROR_NO_MORE_FILES) {
        result.error = error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
      }
      break;
    }
  }

  CloseHandle(snapshot);
  return result;
}

GuiAgentLookup
FindExistingGuiAgent(DWORD console_session_id, std::wstring &gui_directory, std::wstring &gui_path, DWORD exclude_process_id = 0) {
  GuiAgentLookup result;
  result.error = ResolveGuiAgentPath(gui_directory, gui_path);
  if (result.error != ERROR_SUCCESS) {
    return result;
  }
  return OpenExistingGuiAgent(console_session_id, gui_path, exclude_process_id);
}

DWORD
AcquireGuiAgent(DWORD console_session_id, GuiAgentProcess &agent, bool &attached_to_existing) {
  std::wstring gui_directory;
  std::wstring gui_path;
  const auto existing_process = FindExistingGuiAgent(console_session_id, gui_directory, gui_path);
  if (existing_process.error != ERROR_SUCCESS) {
    return existing_process.error;
  }
  if (existing_process.handle != NULL) {
    agent.handle = existing_process.handle;
    agent.process_id = existing_process.process_id;
    agent.acquired_at_ms = GetTickCount64();
    attached_to_existing = true;
    return ERROR_SUCCESS;
  }

  HANDLE user_token = NULL;
  if (!WTSQueryUserToken(console_session_id, &user_token)) {
    return GetLastError();
  }

  LPVOID environment = NULL;
  if (!CreateEnvironmentBlock(&environment, user_token, FALSE)) {
    const auto error = GetLastError();
    CloseHandle(user_token);
    return error;
  }

  auto command = L"\"" + gui_path + L"\" --hidden";
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.lpDesktop = (LPWSTR) L"winsta0\\default";

  PROCESS_INFORMATION process_info = {};
  const auto launched = CreateProcessAsUserW(user_token,
    gui_path.c_str(),
    command_line.data(),
    NULL,
    NULL,
    FALSE,
    CREATE_UNICODE_ENVIRONMENT,
    environment,
    gui_directory.c_str(),
    &startup_info,
    &process_info);
  auto launch_error = launched ? ERROR_SUCCESS : GetLastError();
  if (!launched && launch_error == ERROR_SUCCESS) {
    launch_error = ERROR_GEN_FAILURE;
  }

  if (launched) {
    CloseHandle(process_info.hThread);
    agent.handle = process_info.hProcess;
    agent.process_id = process_info.dwProcessId;
    agent.acquired_at_ms = GetTickCount64();
    attached_to_existing = false;
  }
  DestroyEnvironmentBlock(environment);
  CloseHandle(user_token);
  return launch_error;
}

void
WriteServiceLog(HANDLE log_file, const std::string &message) {
  DWORD bytes_written;
  const auto line = "[sunshinesvc] " + message + "\r\n";
  WriteFile(log_file, line.data(), static_cast<DWORD>(line.size()), &bytes_written, NULL);
}

DWORD
RetryWaitTimeout(ULONGLONG retry_at_ms) {
  const auto now = GetTickCount64();
  if (retry_at_ms <= now) {
    return 0;
  }

  return static_cast<DWORD>(std::min<ULONGLONG>(retry_at_ms - now, MAXDWORD - 1));
}

bool
RunTerminationHelper(HANDLE console_token, DWORD pid) {
  WCHAR module_path[MAX_PATH];
  GetModuleFileNameW(NULL, module_path, _countof(module_path));
  std::wstring command;

  command += L'"';
  command += module_path;
  command += L'"';
  command += L" --terminate " + std::to_wstring(pid);

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.lpDesktop = (LPWSTR) L"winsta0\\default";

  // Execute ourselves as a detached process in the user session with the --terminate argument.
  // This will allow us to attach to Sunshine's console and send it a Ctrl-C event.
  PROCESS_INFORMATION process_info;
  if (!CreateProcessAsUserW(console_token,
        module_path,
        (LPWSTR) command.c_str(),
        NULL,
        NULL,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS,
        NULL,
        NULL,
        &startup_info,
        &process_info)) {
    return false;
  }

  // Bound the helper wait before the Core's 20-second graceful wait.
  const auto wait_result = WaitForSingleObject(process_info.hProcess, 3000);
  DWORD exit_code = ERROR_PROCESS_ABORTED;
  const bool succeeded = wait_result == WAIT_OBJECT_0 &&
                         GetExitCodeProcess(process_info.hProcess, &exit_code) && exit_code == 0;
  if (wait_result != WAIT_OBJECT_0) {
    TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
  }

  // Cleanup handles
  CloseHandle(process_info.hProcess);
  CloseHandle(process_info.hThread);

  return succeeded;
}

struct CoreProcess {
  HANDLE handle = NULL;
  DWORD process_id = 0;
};

// sunshinesvc.exe lives in <install>\\tools. Sunshine.exe is <install>\\Sunshine.exe.
std::wstring
InstallDirectory() {
  std::wstring service_path(32768, L'\0');
  const auto length = GetModuleFileNameW(NULL, service_path.data(), static_cast<DWORD>(service_path.size()));
  if (length == 0 || length >= service_path.size()) {
    return {};
  }
  service_path.resize(length);
  const auto tools = service_path.find_last_of(L"\\/");
  if (tools == std::wstring::npos || tools == 0) {
    return {};
  }
  const auto install = service_path.find_last_of(L"\\/", tools - 1);
  if (install == std::wstring::npos) {
    return {};
  }
  return service_path.substr(0, install);
}

// Every Sunshine.exe whose image is this install, in any session. The service runs in session 0;
// a user-mode copy runs in the console session. Both compete for the web port and the GPU
// [dr-ms-kernel-namespaces-20261109] [dr-ms-queryfullprocessimagenamew-20261109].
std::vector<CoreProcess>
OpenInstallCores(const std::wstring &core_path) {
  std::vector<CoreProcess> found;
  if (core_path.empty()) {
    return found;
  }
  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return found;
  }
  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot, &entry)) {
    CloseHandle(snapshot);
    return found;
  }
  do {
    if (_wcsicmp(entry.szExeFile, L"sunshine.exe") != 0) {
      continue;
    }
    const auto candidate = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
    if (candidate == NULL) {
      continue;
    }
    std::wstring image(32768, L'\0');
    auto image_length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(candidate, 0, image.data(), &image_length) ||
        !sunshinesvc::same_install_core(std::wstring_view { image.data(), image_length }, core_path)) {
      CloseHandle(candidate);
      continue;
    }
    found.push_back(CoreProcess { candidate, entry.th32ProcessID });
  } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return found;
}

// Whether the named per-process event of a core exists and is signaled. A core of an older build, or one that has not
// created it yet, has none.
bool
CoreEventSignaled(sunshinesvc::core_event_e kind, DWORD process_id) {
  const auto event = OpenEventW(SYNCHRONIZE, FALSE, sunshinesvc::core_event_name(kind, process_id).c_str());
  if (event == NULL) {
    return false;
  }
  const auto signaled = WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
  CloseHandle(event);
  return signaled;
}

std::string
DescribeCore(const CoreProcess &core, const sunshinesvc::core_state_t &state) {
  DWORD session = 0;
  const auto session_text = ProcessIdToSessionId(core.process_id, &session) ? std::to_string(session) : std::string { "?" };
  FILETIME created, exited, kernel, user;
  std::string age = "?";
  if (GetProcessTimes(core.handle, &created, &exited, &kernel, &user)) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const auto to_u64 = [](const FILETIME &t) { return (static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    const auto elapsed = to_u64(now) > to_u64(created) ? to_u64(now) - to_u64(created) : 0;
    age = std::to_string(elapsed / 10000000ull) + " s";
  }
  return "Sunshine.exe PID " + std::to_string(core.process_id) + " (session " + session_text + ", running " + age +
         ", ready=" + (state.ready ? "yes" : "no") + ", stopping=" + (state.stopping ? "yes" : "no") + ")";
}

// Ask one process to close by posting WM_CLOSE to the top-level windows it owns. Unlike a console control event, this reaches that
// process only: a core and a command-line Sunshine.exe may share one console, and the event would stop both.
bool
RequestCoreClose(DWORD pid) {
  struct Search {
    DWORD pid;
    bool posted;
  } search { pid, false };
  EnumWindows(
    [](HWND window, LPARAM parameter) -> BOOL {
      auto *found = reinterpret_cast<Search *>(parameter);
      DWORD owner = 0;
      GetWindowThreadProcessId(window, &owner);
      if (owner == found->pid && PostMessageW(window, WM_CLOSE, 0, 0)) {
        found->posted = true;
      }
      return TRUE;
    },
    reinterpret_cast<LPARAM>(&search));
  return search.posted;
}

// Decide which of the cores found for this install to supervise, and make the others leave. A core is kept only when it
// has said it is ready and has not said it is stopping: one on its way out would exit a moment after it was adopted and
// leave the service without a core (the adopted process that ends after a fixed wait and is then restarted). The others
// are given a short, bounded time and then ended, so a fresh core never competes with them for the ports and the GPU.
// Every handle in `cores` is closed, except the one returned.
CoreProcess
ResolveCores(std::vector<CoreProcess> &cores, DWORD expected_session_id, HANDLE log_file) {
  std::vector<sunshinesvc::core_state_t> states;
  for (const auto &core : cores) {
    auto ready = CoreEventSignaled(sunshinesvc::core_event_e::ready, core.process_id);
    const auto stopping = CoreEventSignaled(sunshinesvc::core_event_e::stopping, core.process_id);
    if (ready) {
      DWORD core_session_id = invalid_session_id;
      if (!ProcessIdToSessionId(core.process_id, &core_session_id) ||
          core_session_id != expected_session_id) {
        ready = false;
      }
    }
    states.push_back({ ready, stopping });
  }
  const auto plan = sunshinesvc::plan_cores(states);

  CoreProcess kept;
  for (std::size_t index = 0; index < cores.size(); ++index) {
    auto &core = cores[index];
    const auto description = DescribeCore(core, states[index]);
    switch (plan.actions[index]) {
      case sunshinesvc::core_action_e::keep:
        kept = core;
        core.handle = NULL;
        continue;
      case sunshinesvc::core_action_e::wait_for_exit: {
        WriteServiceLog(log_file, description + " is shutting down; not adopting it, waiting for it to exit");
        if (WaitForSingleObject(core.handle, sunshinesvc::core_stopping_wait_ms) != WAIT_OBJECT_0) {
          TerminateProcess(core.handle, ERROR_PROCESS_ABORTED);
          const auto ended = WaitForSingleObject(core.handle, sunshinesvc::core_end_wait_ms) == WAIT_OBJECT_0;
          WriteServiceLog(log_file, description + (ended ? " did not finish its shutdown in time and was ended" : " did not exit within the wait after force termination"));
        }
        break;
      }
      case sunshinesvc::core_action_e::end: {
        WriteServiceLog(log_file, description + " is not known to be healthy; not adopting it, ending it before starting a fresh core");
        const auto asked = RequestCoreClose(core.process_id);
        if (!asked || WaitForSingleObject(core.handle, sunshinesvc::core_end_wait_ms) != WAIT_OBJECT_0) {
          TerminateProcess(core.handle, ERROR_PROCESS_ABORTED);
          const auto ended = WaitForSingleObject(core.handle, sunshinesvc::core_end_wait_ms) == WAIT_OBJECT_0;
          WriteServiceLog(log_file, description + (ended ? " was ended" : " did not exit within the wait after force termination"));
        }
        break;
      }
    }
    CloseHandle(core.handle);
    core.handle = NULL;
  }
  cores.clear();

  // The others took time to leave: check the candidate again against what it says now, not against what it said at the start.
  if (kept.handle != NULL) {
    const bool alive = WaitForSingleObject(kept.handle, 0) == WAIT_TIMEOUT;
    const bool ready = alive && CoreEventSignaled(sunshinesvc::core_event_e::ready, kept.process_id);
    const bool stopping = alive && CoreEventSignaled(sunshinesvc::core_event_e::stopping, kept.process_id);
    if (!alive || !ready || stopping) {
      WriteServiceLog(log_file,
        "Sunshine.exe PID " + std::to_string(kept.process_id) +
          (alive ? " is no longer ready and running; not adopting it" : " has exited; not adopting it"));
      if (alive && WaitForSingleObject(kept.handle, sunshinesvc::core_stopping_wait_ms) != WAIT_OBJECT_0) {
        TerminateProcess(kept.handle, ERROR_PROCESS_ABORTED);
        WaitForSingleObject(kept.handle, sunshinesvc::core_end_wait_ms);
      }
      CloseHandle(kept.handle);
      kept = CoreProcess {};
    }
  }
  if (kept.handle != NULL) {
    WriteServiceLog(log_file, "Adopting Sunshine.exe PID " + std::to_string(kept.process_id) + ": it is ready and not shutting down");
  }
  return kept;
}

// The lifecycle facts of the core, for the host to report at its next start (tools/sunshinesvc_lifecycle.h). Writing them never changes what the service does.
std::filesystem::path
LifecycleFile() {
  const auto install = InstallDirectory();
  if (install.empty()) {
    return {};
  }
  return std::filesystem::path(install) / L"config" / L"telemetry" / L"service_lifecycle.jsonl";
}

void
NoteLifecycle(const std::string &line) {
  const auto file = LifecycleFile();
  if (!file.empty()) {
    sunshinesvc::lifecycle::append(file, line);
  }
}

ULONGLONG
FileTimeToMs(const FILETIME &time) {
  ULARGE_INTEGER value;
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  return value.QuadPart / 10000;
}

// How long the process has been running (its creation time to now) and how long it ran (creation to exit, for a process that ended).
long long
ProcessAgeMs(HANDLE process) {
  FILETIME created, exited, kernel, user, now;
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    return 0;
  }
  GetSystemTimeAsFileTime(&now);
  const auto start = FileTimeToMs(created), end = FileTimeToMs(now);
  return end > start ? static_cast<long long>(end - start) : 0;
}

long long
ProcessLifeMs(HANDLE process) {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    return -1;
  }
  const auto start = FileTimeToMs(created), end = FileTimeToMs(exited);
  return end > start ? static_cast<long long>(end - start) : 0;
}

VOID WINAPI
ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv) {
  service_status_handle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, HandlerEx, NULL);
  if (service_status_handle == NULL) {
    // Nothing we can really do here but terminate ourselves
    ExitProcess(GetLastError());
    return;
  }

  // Tell SCM we're starting
  service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  service_status.dwServiceSpecificExitCode = 0;
  service_status.dwWin32ExitCode = NO_ERROR;
  service_status.dwWaitHint = 0;
  service_status.dwControlsAccepted = 0;
  service_status.dwCheckPoint = 0;
  service_status.dwCurrentState = SERVICE_START_PENDING;
  SetServiceStatus(service_status_handle, &service_status);

  // Create a manual-reset stop event
  stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (stop_event == NULL) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // Create an auto-reset session change event
  session_change_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (session_change_event == NULL) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  auto log_file_handle = sunshinesvc::open_primary_log_handle();
  if (log_file_handle == INVALID_HANDLE_VALUE) {
    // A lingering Sunshine.exe holds the inherited log handle open, which can
    // fail the open above with ERROR_SHARING_VIOLATION. The service must
    // never refuse to start over a log file: degrade to a fallback sink so
    // the core can still be launched and recovered.
    log_file_handle = sunshinesvc::open_fallback_log_handle();
  }
  if (log_file_handle == INVALID_HANDLE_VALUE) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // We can use a single STARTUPINFOEXW for all the processes that we launch
  STARTUPINFOEXW startup_info = {};
  startup_info.StartupInfo.cb = sizeof(startup_info);
  startup_info.StartupInfo.lpDesktop = (LPWSTR) L"winsta0\\default";
  startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup_info.StartupInfo.hStdInput = NULL;
  startup_info.StartupInfo.hStdOutput = log_file_handle;
  startup_info.StartupInfo.hStdError = log_file_handle;

  // Allocate an attribute list with space for 2 entries
  startup_info.lpAttributeList = AllocateProcThreadAttributeList(2);
  if (startup_info.lpAttributeList == NULL) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // Only allow Sunshine.exe to inherit the log file handle, not all inheritable handles
  UpdateProcThreadAttribute(startup_info.lpAttributeList,
    0,
    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
    &log_file_handle,
    sizeof(log_file_handle),
    NULL,
    NULL);

  // Tell SCM we're running (and stoppable now)
  service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
  service_status.dwCurrentState = SERVICE_RUNNING;
  SetServiceStatus(service_status_handle, &service_status);
  NoteLifecycle(sunshinesvc::lifecycle::svc_start_line(sunshinesvc::lifecycle::unix_ms()));

  GuiAgentProcess gui_agent;
  DWORD gui_agent_session_id = invalid_session_id;
  DWORD gui_agent_last_error = ERROR_SUCCESS;
  ULONGLONG gui_agent_retry_at_ms = 0;
  sunshinesvc::GuiRestartBackoff gui_agent_backoff;
  sunshinesvc::GuiAgentRestartPolicy gui_agent_restart_policy;
  sunshinesvc::GuiAgentCrashLogLimiter gui_agent_log_limiter;

  const auto acquire_gui_agent = [&]() {
    if (gui_agent_session_id == invalid_session_id) {
      gui_agent_retry_at_ms = GetTickCount64() + sunshinesvc::GUI_REATTACH_POLL_MS;
      return;
    }
    bool attached_to_existing = false;
    const auto error = AcquireGuiAgent(gui_agent_session_id, gui_agent, attached_to_existing);
    if (error == ERROR_SUCCESS) {
      const bool recovered_from_error = gui_agent_last_error != ERROR_SUCCESS;
      const auto recovery = gui_agent_last_error == ERROR_SUCCESS ? "acquired" : "recovered";
      if (gui_agent_log_limiter.should_log_acquisition(recovered_from_error)) {
        WriteServiceLog(log_file_handle,
          "GUI agent " + std::string(recovery) + " for session " + std::to_string(gui_agent_session_id) +
            " (PID " + std::to_string(gui_agent.process_id) +
            (attached_to_existing ? ", existing process)" : ", launched process)"));
      }
      gui_agent_last_error = ERROR_SUCCESS;
      gui_agent_retry_at_ms = 0;
      return;
    }

    const auto retry_delay_ms = gui_agent_backoff.next_delay();
    gui_agent_retry_at_ms = GetTickCount64() + retry_delay_ms;
    if (error != gui_agent_last_error) {
      WriteServiceLog(log_file_handle,
        "GUI agent acquisition failed for session " + std::to_string(gui_agent_session_id) +
          " (Win32 error " + std::to_string(error) + "); retrying in " +
          std::to_string(retry_delay_ms) + " ms");
    }
    gui_agent_last_error = error;
  };

  const auto reattach_gui_agent = [&]() {
    if (gui_agent_session_id == invalid_session_id) {
      gui_agent_retry_at_ms = GetTickCount64() + sunshinesvc::GUI_REATTACH_POLL_MS;
      return;
    }
    std::wstring gui_directory;
    std::wstring gui_path;
    auto existing_process = FindExistingGuiAgent(gui_agent_session_id, gui_directory, gui_path);
    gui_agent_retry_at_ms = GetTickCount64() + sunshinesvc::GUI_REATTACH_POLL_MS;

    if (existing_process.error != ERROR_SUCCESS) {
      if (existing_process.error != gui_agent_last_error) {
        WriteServiceLog(log_file_handle,
          "GUI agent reattach probe failed for session " + std::to_string(gui_agent_session_id) +
            " (Win32 error " + std::to_string(existing_process.error) + "); retrying in " +
            std::to_string(sunshinesvc::GUI_REATTACH_POLL_MS) + " ms");
      }
      gui_agent_last_error = existing_process.error;
      return;
    }

    if (existing_process.handle == NULL) {
      gui_agent_last_error = ERROR_SUCCESS;
      return;
    }

    gui_agent.handle = existing_process.handle;
    gui_agent.process_id = existing_process.process_id;
    gui_agent.acquired_at_ms = GetTickCount64();
    gui_agent_restart_policy.resume_supervision();
    gui_agent_last_error = ERROR_SUCCESS;
    gui_agent_retry_at_ms = 0;
    gui_agent_backoff.reset();
    gui_agent_log_limiter.reset();
    WriteServiceLog(log_file_handle,
      "GUI agent reattached for session " + std::to_string(gui_agent_session_id) +
        " (PID " + std::to_string(gui_agent.process_id) + ", existing process)");
  };

  const auto handle_gui_agent_exit = [&]() {
    DWORD exit_code = ERROR_PROCESS_ABORTED;
    if (!GetExitCodeProcess(gui_agent.handle, &exit_code) || exit_code == STILL_ACTIVE) {
      // A missing status is not a menu quit. TerminateProcess does not run the
      // agent's exit path, and STILL_ACTIVE would mean the wait raced the exit.
      // [ms-getexitcodeprocess-20261109], [ms-terminating-a-process-20261109]
      exit_code = ERROR_PROCESS_ABORTED;
    }
    const auto exit_at_ms = GetTickCount64();
    const auto runtime_ms = exit_at_ms - gui_agent.acquired_at_ms;
    const auto exited_process_id = gui_agent.process_id;
    CloseGuiAgentHandle(gui_agent);

    std::wstring gui_directory;
    std::wstring gui_path;
    auto replacement = FindExistingGuiAgent(gui_agent_session_id, gui_directory, gui_path, exited_process_id);
    const bool replacement_running = replacement.error == ERROR_SUCCESS && replacement.handle != NULL;
    // The GUI performs a native fallback when WebView2 is absent and exits with
    // a dedicated code. Do not turn that expected user-facing path into a
    // service crash loop. [ds5-ms-webview2-distribution-20261109]
    const bool suppress_missing_webview =
      !replacement_running && exit_code == sunshinesvc::GUI_WEBVIEW2_MISSING_EXIT_CODE &&
      gui_agent_restart_policy.note_webview2_missing(runtime_ms);
    if (!replacement_running && exit_code != sunshinesvc::GUI_WEBVIEW2_MISSING_EXIT_CODE) {
      gui_agent_restart_policy.reset_webview2_missing();
    }
    const auto action = suppress_missing_webview ? sunshinesvc::GuiAgentExitAction::suppress_launch : sunshinesvc::decide_gui_agent_exit(exit_code, replacement_running);

    if (action == sunshinesvc::GuiAgentExitAction::adopt_replacement) {
      gui_agent.handle = replacement.handle;
      gui_agent.process_id = replacement.process_id;
      gui_agent.acquired_at_ms = exit_at_ms;
      gui_agent_restart_policy.resume_supervision();
      gui_agent_retry_at_ms = 0;
      gui_agent_last_error = ERROR_SUCCESS;
      gui_agent_backoff.reset();
      gui_agent_log_limiter.reset();
      WriteServiceLog(log_file_handle,
        "GUI agent exit in session " + std::to_string(gui_agent_session_id) +
          " (PID " + std::to_string(exited_process_id) + ", exit code " + std::to_string(exit_code) +
          ") left PID " + std::to_string(gui_agent.process_id) + " running; supervision attached to it");
      return;
    }
    if (replacement.handle != NULL) {
      CloseHandle(replacement.handle);
    }

    if (action == sunshinesvc::GuiAgentExitAction::suppress_launch) {
      gui_agent_restart_policy.suppress_launch();
      gui_agent_retry_at_ms = exit_at_ms + sunshinesvc::GUI_REATTACH_POLL_MS;
      gui_agent_last_error = ERROR_SUCCESS;
      gui_agent_backoff.reset();
      if (suppress_missing_webview) {
        WriteServiceLog(log_file_handle,
          "WebView2 Runtime missing; GUI restart suppressed after " +
            std::to_string(sunshinesvc::GUI_WEBVIEW2_MISSING_MAX_FAILURES) +
            " failures in session " + std::to_string(gui_agent_session_id));
      }
      else {
        WriteServiceLog(log_file_handle,
          "GUI agent quit deliberately in session " + std::to_string(gui_agent_session_id) +
            " (PID " + std::to_string(exited_process_id) +
            "); automatic launch suppressed while existing-process reattach remains active");
      }
      return;
    }

    const auto retry_delay_ms = gui_agent_backoff.next_delay(runtime_ms);
    gui_agent_retry_at_ms = exit_at_ms + retry_delay_ms;
    gui_agent_last_error = ERROR_SUCCESS;
    if (gui_agent_log_limiter.should_log_exit(exit_code, retry_delay_ms, exit_at_ms)) {
      WriteServiceLog(log_file_handle,
        "GUI agent exited in session " + std::to_string(gui_agent_session_id) +
          " (PID " + std::to_string(exited_process_id) + ", exit code " +
          std::to_string(exit_code) + ", runtime " + std::to_string(runtime_ms) +
          " ms); restarting in " + std::to_string(retry_delay_ms) + " ms");
    }
  };

  const auto select_gui_session = [&](DWORD next_session_id) {
    if (gui_agent_session_id == next_session_id) {
      return;
    }
    // Closing the supervision handle does not terminate an agent in the old
    // session. It can be reattached if that user session becomes active again.
    CloseGuiAgentHandle(gui_agent);
    gui_agent_session_id = next_session_id;
    gui_agent_last_error = ERROR_SUCCESS;
    gui_agent_retry_at_ms = 0;
    if (next_session_id == invalid_session_id) {
      gui_agent_retry_at_ms = GetTickCount64() + sunshinesvc::GUI_REATTACH_POLL_MS;
    }
    gui_agent_backoff.reset();
    gui_agent_restart_policy.resume_supervision();
    gui_agent_log_limiter.reset();
  };

  // Loop every 3 seconds until the stop event is set or Sunshine.exe is running. The first pass is at once: the loop used to wait 3 s before it tried at all, and
  // every start of the service (every boot) lost them (service 12:53:00, Sunshine.exe 12:53:04). A user session that is not there yet is picked up at the next pass.
  DWORD loop_wait_ms = 0;
  // why the next core is started: the first one is the boot; later ones follow an end of the core the service did not ask for, a change of the interactive user session, or a start that failed
  const char *next_core_cause = "boot";
  // repeated failures of the core's start are not retried at the pace of the pass interval (sunshinesvc_state.h)
  sunshinesvc::CoreRestartBackoff core_restart_backoff;
  // not before this tick (GetTickCount64): only the start of the core waits; the pass interval, the GUI agent's supervision and the stop event go on as before
  ULONGLONG core_restart_not_before_tick = 0;
  while (WaitForSingleObject(stop_event, std::exchange(loop_wait_ms, 3000)) != WAIT_OBJECT_0) {
    const auto binding_mode = session_binding_mode();
    const auto selection = FindActiveUserSessionId();
    auto interactive_session_id = selection.session_id;
    if (selection.query_failed) {
      interactive_session_id = gui_agent_session_id;
    }
    const auto targets = config_session_binding::select_sessions(
      binding_mode, WTSGetActiveConsoleSessionId(), interactive_session_id);
    if (!selection.query_failed) {
      select_gui_session(targets.gui);
    }

    // GUI availability does not depend on the capture core's session or on a
    // successful core start. A token lookup failure is retried without reviving
    // a GUI that the user deliberately quit.
    if (gui_agent.handle != NULL && WaitForSingleObject(gui_agent.handle, 0) == WAIT_OBJECT_0) {
      handle_gui_agent_exit();
    }
    if (gui_agent.handle == NULL && RetryWaitTimeout(gui_agent_retry_at_ms) == 0) {
      if (gui_agent_restart_policy.launch_allowed()) {
        acquire_gui_agent();
      }
      else {
        reattach_gui_agent();
      }
    }

    if ((selection.query_failed && binding_mode == config_session_binding::mode_e::automatic) ||
        targets.capture == invalid_session_id) {
      // No capture session yet, or automatic selection could not be queried.
      continue;
    }
    const DWORD session_id = targets.capture;

    auto console_token = DuplicateTokenForSession(session_id);
    if (console_token == NULL) {
      continue;
    }

    // One core per install. A leftover from the previous run (the service child that did not
    // exit, or a user-mode Sunshine.exe) is supervised instead of joined by a second process, when it
    // is ready and not shutting down; otherwise it is ended and a fresh core is started.
    const auto core_path = InstallDirectory() + L"\\Sunshine.exe";
    auto cores = OpenInstallCores(core_path);
    HANDLE job_handle = NULL;
    PROCESS_INFORMATION process_info = {};
    bool core_adopted = false;
    ULONGLONG adopted_at_tick = 0;
    const auto found_cores = static_cast<int>(cores.size());
    const auto adopted_core = ResolveCores(cores, session_id, log_file_handle);
    if (sunshinesvc::core_start_decision(adopted_core.handle != NULL) == sunshinesvc::core_start_e::adopt) {
      if (found_cores > 1) {
        NoteLifecycle(sunshinesvc::lifecycle::extra_ended_line(sunshinesvc::lifecycle::unix_ms(), found_cores - 1));
      }
      process_info.hProcess = adopted_core.handle;
      process_info.dwProcessId = adopted_core.process_id;
      process_info.hThread = NULL;
      WriteServiceLog(log_file_handle,
        "Sunshine.exe PID " + std::to_string(process_info.dwProcessId) +
          " is already running; supervising it instead of starting another");
      core_adopted = true;
      adopted_at_tick = GetTickCount64();
      NoteLifecycle(sunshinesvc::lifecycle::core_start_line(sunshinesvc::lifecycle::unix_ms(), true, process_info.dwProcessId, ProcessAgeMs(process_info.hProcess), next_core_cause));
    }
    else {
      if (GetTickCount64() < core_restart_not_before_tick) {
        // the core's start is held back after repeated failures (CoreRestartBackoff): the GUI agent is still looked after in this pass
        CloseHandle(console_token);
        if (gui_agent.handle != NULL && WaitForSingleObject(gui_agent.handle, 0) == WAIT_OBJECT_0) {
          handle_gui_agent_exit();
        }
        continue;
      }
      // Job objects cannot span sessions, so we must create one for each process
      job_handle = CreateJobObjectForChildProcess();
      if (job_handle == NULL) {
        CloseHandle(console_token);
        next_core_cause = "start_failed";
        continue;
      }

      // Start Sunshine.exe inside our job object
      UpdateProcThreadAttribute(startup_info.lpAttributeList,
        0,
        PROC_THREAD_ATTRIBUTE_JOB_LIST,
        &job_handle,
        sizeof(job_handle),
        NULL,
        NULL);

      if (!CreateProcessAsUserW(console_token,
            L"Sunshine.exe",
            NULL,
            NULL,
            NULL,
            TRUE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            NULL,
            NULL,
            (LPSTARTUPINFOW) &startup_info,
            &process_info)) {
        CloseHandle(console_token);
        CloseHandle(job_handle);
        next_core_cause = "start_failed";
        continue;
      }
      NoteLifecycle(sunshinesvc::lifecycle::core_start_line(sunshinesvc::lifecycle::unix_ms(), false, process_info.dwProcessId, 0, next_core_cause));
    }

    // The GUI may have exited after the previous Core stopped, while no inner
    // wait loop was active. Reap that lifecycle's process before a new Core
    // lifecycle restores launch permission or decides whether acquisition is
    // needed.
    if (gui_agent.handle != NULL && WaitForSingleObject(gui_agent.handle, 0) == WAIT_OBJECT_0) {
      handle_gui_agent_exit();
    }

    // Keep the tray agent in the signed-in user's session so HKCU settings and
    // single-instance ownership remain scoped to that user. Preserve the
    // handle across Core restarts in the same session.
    if (gui_agent.handle == NULL &&
        gui_agent_restart_policy.launch_allowed() &&
        RetryWaitTimeout(gui_agent_retry_at_ms) == 0) {
      acquire_gui_agent();
    }

    bool still_running;
    // whether the service itself asked the core to end (service stop, change of the console session), and whether it was the session change
    bool exit_requested = false;
    bool exit_for_session_change = false;
    bool session_reselect_pending = selection.query_failed;
    ULONGLONG session_reselect_at_ms = 0;
    if (selection.query_failed) {
      session_reselect_at_ms = GetTickCount64() + session_reselect_delay_ms;
    }
    do {
      // Wait on the exact GUI process while it is managed. After a clean exit,
      // only a low-frequency lookup runs so externally launched replacements
      // can be reattached without allowing the service to revive the GUI.
      const HANDLE wait_objects[] = { stop_event, process_info.hProcess, session_change_event, gui_agent.handle };
      const DWORD wait_object_count = gui_agent.handle == NULL ? 3 : 4;
      DWORD wait_timeout = INFINITE;
      if (gui_agent.handle == NULL) {
        wait_timeout = RetryWaitTimeout(gui_agent_retry_at_ms);
      }
      if (session_reselect_pending) {
        const auto reselect_timeout = RetryWaitTimeout(session_reselect_at_ms);
        if (wait_timeout == INFINITE || reselect_timeout < wait_timeout) {
          wait_timeout = reselect_timeout;
        }
      }
      const auto wait_result = WaitForMultipleObjects(wait_object_count, wait_objects, FALSE, wait_timeout);
      switch (wait_result) {
        case WAIT_TIMEOUT:
          // The stop/Core handles may have become signaled after the wait timed
          // out but before process acquisition. Let the next wait dispatch the
          // lifecycle event instead of reviving the GUI during shutdown.
          if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0 ||
              WaitForSingleObject(process_info.hProcess, 0) == WAIT_OBJECT_0) {
            still_running = true;
            break;
          }
          if (session_reselect_pending && RetryWaitTimeout(session_reselect_at_ms) == 0) {
            session_reselect_pending = false;
            SetEvent(session_change_event);
            still_running = true;
            break;
          }
          if (gui_agent_session_id == invalid_session_id) {
            const auto gui_selection = FindActiveUserSessionId();
            if (!gui_selection.query_failed && gui_selection.session_id != invalid_session_id) {
              // Re-evaluate both targets: automatic capture may also need to
              // move when a user becomes active without a delivered event.
              SetEvent(session_change_event);
              still_running = true;
              break;
            }
          }
          if (gui_agent_restart_policy.launch_allowed()) {
            acquire_gui_agent();
          }
          else {
            reattach_gui_agent();
          }
          still_running = true;
          break;

        case WAIT_OBJECT_0 + 2: {
          const auto next_user = FindActiveUserSessionId();
          const auto update = config_session_binding::update_sessions(
            binding_mode, session_id, gui_agent_session_id, WTSGetActiveConsoleSessionId(),
            next_user.session_id, next_user.query_failed);
          if (update.update_gui) {
            select_gui_session(update.targets.gui);
          }
          if (update.retry) {
            session_reselect_pending = true;
            session_reselect_at_ms = GetTickCount64() + session_reselect_delay_ms;
          }
          else {
            session_reselect_pending = false;
          }
          if (!update.restart_capture) {
            // GUI-only changes and transient lookup failures must not
            // interrupt capture. A failed lookup is retried above.
            still_running = true;
            continue;
          }
          // Fall-through to terminate Sunshine.exe and start it again.
          exit_for_session_change = true;
          [[fallthrough]];
        }
        case WAIT_OBJECT_0:
          exit_requested = true;
          // The service is shutting down, so try to gracefully terminate Sunshine.exe.
          // If it doesn't terminate in 20 seconds, we will forcefully terminate it.
          if (!RunTerminationHelper(console_token, process_info.dwProcessId) ||
              WaitForSingleObject(process_info.hProcess, 20000) != WAIT_OBJECT_0) {
            // If it won't terminate gracefully, kill it now
            TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
            // Termination may still wait for pending I/O. Record an incomplete
            // exit without assuming the cause of the stalled process.
            if (WaitForSingleObject(process_info.hProcess, 5000) != WAIT_OBJECT_0) {
              WriteServiceLog(log_file_handle,
                "Core PID " + std::to_string(process_info.dwProcessId) +
                  " did not exit within 5 seconds after force termination; ports and files may remain held");
            }
          }
          still_running = false;
          break;

        case WAIT_OBJECT_0 + 1: {
          // Sunshine terminated itself.

          DWORD exit_code;
          if (GetExitCodeProcess(process_info.hProcess, &exit_code) && exit_code == ERROR_SHUTDOWN_IN_PROGRESS) {
            // Sunshine is asking for us to shut down, so gracefully stop ourselves.
            SetEvent(stop_event);
          }
          still_running = false;
          break;
        }

        case WAIT_OBJECT_0 + 3: {
          handle_gui_agent_exit();
          still_running = true;
          break;
        }

        default:
          SetEvent(stop_event);
          still_running = false;
          break;
      }
    } while (still_running);

    // what the end of this core looked like: with the exit code, how long it had run, how long since the service took it over, and whether the service asked for it
    {
      DWORD ended_with = 0;
      if (WaitForSingleObject(process_info.hProcess, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(process_info.hProcess, &ended_with)) {
        const auto exit_ms = sunshinesvc::lifecycle::unix_ms();
        // a restart the core asked for itself (Panel, a saved setting) leaves a marker and exits with code 0; it is not an exit nobody asked for
        bool core_restart = false;
        std::string restart_word;
        if (const auto file = LifecycleFile(); !file.empty()) {
          core_restart = sunshinesvc::lifecycle::take_restart_request(file.parent_path(), exit_ms, process_info.dwProcessId, sunshinesvc::lifecycle::process_start_unix_ms(process_info.hProcess), &restart_word) && !exit_requested && ended_with == 0;
        }
        NoteLifecycle(sunshinesvc::lifecycle::core_exit_line(exit_ms, ended_with, ProcessLifeMs(process_info.hProcess),
          core_adopted ? static_cast<long long>(GetTickCount64() - adopted_at_tick) : -1,
          exit_requested || ended_with == ERROR_SHUTDOWN_IN_PROGRESS, core_restart, core_restart ? restart_word : std::string {}));  // ERROR_SHUTDOWN_IN_PROGRESS: the core asked the service to stop (quit from the tray, the Panel)
        const bool asked_for_exit = exit_requested || exit_for_session_change || ended_with == ERROR_SHUTDOWN_IN_PROGRESS || core_restart;
        const auto restart_wait = sunshinesvc::core_restart_wait(core_restart_backoff, static_cast<std::uint64_t>(ProcessLifeMs(process_info.hProcess)), asked_for_exit, GetTickCount64());
        // the deadline moves for every end of a core, a zero wait included: the deadline of earlier failures never holds back the start that follows an adopted or asked-for end
        core_restart_not_before_tick = restart_wait.not_before_tick;
        if (restart_wait.delay_ms > 0) {
          WriteServiceLog(log_file_handle,
            "Sunshine.exe ended with code " + std::to_string(ended_with) + " shortly after it started; starting it again in " + std::to_string(restart_wait.delay_ms) + " ms");
        }
      }
      next_core_cause = exit_for_session_change ? "session_change" : "core_exit";
    }

    if (process_info.hThread != NULL) {
      CloseHandle(process_info.hThread);
    }
    CloseHandle(process_info.hProcess);
    CloseHandle(console_token);
    if (job_handle != NULL) {
      CloseHandle(job_handle);
    }
  }

  CloseGuiAgentHandle(gui_agent);

  // Let SCM know we've stopped
  service_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(service_status_handle, &service_status);
}

// This will run in a child process in the user session
int
DoGracefulTermination(DWORD pid) {
  // Attach to Sunshine's console
  if (!AttachConsole(pid)) {
    return GetLastError();
  }

  // Disable our own Ctrl-C handling
  SetConsoleCtrlHandler(NULL, TRUE);

  // Send a Ctrl-C event to Sunshine
  if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
    return GetLastError();
  }

  return 0;
}

namespace sunandmon_svc {
  constexpr wchar_t kServiceNameW[] = L"SunAndMonHostService";
  constexpr char kServiceNameA[] = "SunAndMonHostService";
  constexpr wchar_t kDisplayNameW[] = L"SunAndMon Host Service";
  constexpr wchar_t kDescriptionW[] =
    L"Runs the SunAndMon Sunshine host as SYSTEM so remote sessions can view and control Windows UAC prompts.";
  constexpr wchar_t kPipeNameW[] = L"\\\\.\\pipe\\SunAndMonHostService";
  constexpr wchar_t kPipeSddlW[] = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";

  bool
  is_lower_hex(std::string_view value, size_t expected_length) {
    if (value.size() != expected_length) {
      return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
      return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
  }

  std::wstring
  self_module_path() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(NULL, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
      return {};
    }
    path.resize(length);
    return path;
  }

  std::wstring
  expected_controller_path() {
    const auto host_dir = InstallDirectory();
    if (host_dir.empty()) {
      return {};
    }
    const auto sep = host_dir.find_last_of(L"\\/");
    if (sep == std::wstring::npos) {
      return {};
    }
    return host_dir.substr(0, sep) + L"\\SunAndMon.exe";
  }

  bool
  verify_pipe_client(HANDLE pipe, DWORD &client_pid, DWORD &client_session_id) {
    client_pid = 0;
    client_session_id = invalid_session_id;
    if (!GetNamedPipeClientProcessId(pipe, &client_pid) || client_pid == 0) {
      return false;
    }
    if (!ProcessIdToSessionId(client_pid, &client_session_id)) {
      return false;
    }
    const auto expected = expected_controller_path();
    if (expected.empty()) {
      return false;
    }
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, client_pid);
    if (process == NULL) {
      return false;
    }
    std::wstring image(32768, L'\0');
    auto image_length = static_cast<DWORD>(image.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, image.data(), &image_length) &&
                    sunshinesvc::same_install_core(std::wstring_view { image.data(), image_length }, expected);
    CloseHandle(process);
    return ok;
  }

  std::wstring
  ascii_to_wide(std::string_view text) {
    return std::wstring(text.begin(), text.end());
  }

  std::wstring
  decode_utf8_hex_path(std::string_view hex) {
    if (hex.empty() || (hex.size() % 2) != 0 || hex.size() > 1600) {
      return {};
    }
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    std::string utf8;
    utf8.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
      const int hi = nibble(hex[i]);
      const int lo = nibble(hex[i + 1]);
      if (hi < 0 || lo < 0) {
        return {};
      }
      const char ch = static_cast<char>((hi << 4) | lo);
      if (ch == '\0' || ch == '\r' || ch == '\n') {
        return {};
      }
      utf8.push_back(ch);
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
      return {};
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed) != needed) {
      return {};
    }
    const std::filesystem::path candidate(wide);
    if (!candidate.is_absolute()) {
      return {};
    }
    for (const auto &part : candidate) {
      if (part == L"..") {
        return {};
      }
    }
    return wide;
  }

  std::vector<wchar_t>
  build_environment_block(HANDLE token, const std::string &managed_token, const std::string &host_key, bool allow_alt_ports, const std::wstring &config_dir) {
    std::vector<wchar_t> block;
    LPVOID raw_env = nullptr;
    if (CreateEnvironmentBlock(&raw_env, token, FALSE) && raw_env != nullptr) {
      const auto *cursor = static_cast<const wchar_t *>(raw_env);
      while (*cursor != L'\0') {
        const std::wstring_view entry(cursor);
        const bool skip = entry.size() >= 10 && _wcsnicmp(entry.data(), L"SUNANDMON_", 10) == 0;
        if (!skip) {
          block.insert(block.end(), entry.begin(), entry.end());
          block.push_back(L'\0');
        }
        cursor += entry.size() + 1;
      }
      DestroyEnvironmentBlock(raw_env);
    }

    auto append = [&](std::wstring_view name, std::wstring_view value) {
      block.insert(block.end(), name.begin(), name.end());
      block.push_back(L'=');
      block.insert(block.end(), value.begin(), value.end());
      block.push_back(L'\0');
    };

    append(L"SUNANDMON_ALLOW_ALTERNATE_PORTS", allow_alt_ports ? L"1" : L"0");
    append(L"SUNANDMON_MANAGED_TOKEN", ascii_to_wide(managed_token));
    if (!host_key.empty()) {
      append(L"SUNANDMON_HOST_KEY", ascii_to_wide(host_key));
    }
    if (!config_dir.empty()) {
      append(L"SUNANDMON_CONFIG_DIR", config_dir);
    }
    block.push_back(L'\0');
    return block;
  }

  bool
  write_pipe_bytes(HANDLE pipe, std::mutex &write_mutex, std::string_view data) {
    std::lock_guard lock(write_mutex);
    const auto write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (write_event == NULL) {
      return false;
    }
    OVERLAPPED overlapped = {};
    overlapped.hEvent = write_event;
    DWORD written = 0;
    bool ok = WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), &written, &overlapped) != FALSE;
    if (!ok) {
      if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(write_event, 3000) == WAIT_OBJECT_0) {
          ok = GetOverlappedResult(pipe, &overlapped, &written, FALSE) != FALSE;
        }
        else {
          CancelIoEx(pipe, &overlapped);
        }
      }
    }
    CloseHandle(write_event);
    return ok && written == data.size();
  }

  bool
  read_pipe_line(HANDLE pipe, HANDLE cancel_event, std::string &line, DWORD timeout_ms) {
    line.clear();
    const auto read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (read_event == NULL) {
      return false;
    }
    const auto deadline = GetTickCount64() + timeout_ms;
    char ch = '\0';
    bool ok = false;
    while (line.size() < 2048) {
      ResetEvent(read_event);
      OVERLAPPED overlapped = {};
      overlapped.hEvent = read_event;
      DWORD read_bytes = 0;
      BOOL started = ReadFile(pipe, &ch, 1, &read_bytes, &overlapped);
      if (!started) {
        const auto error = GetLastError();
        if (error != ERROR_IO_PENDING) {
          break;
        }
        const auto now = GetTickCount64();
        if (now >= deadline) {
          CancelIoEx(pipe, &overlapped);
          break;
        }
        const HANDLE waits[] = { cancel_event, read_event };
        const auto waited = WaitForMultipleObjects(2, waits, FALSE, static_cast<DWORD>(deadline - now));
        if (waited != WAIT_OBJECT_0 + 1) {
          CancelIoEx(pipe, &overlapped);
          break;
        }
        if (!GetOverlappedResult(pipe, &overlapped, &read_bytes, FALSE)) {
          break;
        }
      }
      if (read_bytes == 0) {
        break;
      }
      if (ch == '\n') {
        ok = true;
        break;
      }
      if (ch != '\r') {
        line.push_back(ch);
      }
    }
    CloseHandle(read_event);
    return ok;
  }

  void
  run_session(HANDLE pipe, HANDLE svc_stop_event, HANDLE log_file_handle) {
    std::mutex write_mutex;
    DWORD client_pid = 0;
    DWORD client_session_id = invalid_session_id;
    if (!verify_pipe_client(pipe, client_pid, client_session_id)) {
      WriteServiceLog(log_file_handle, "Rejected SunAndMonHostService pipe client PID " + std::to_string(client_pid));
      write_pipe_bytes(pipe, write_mutex, "ERROR unauthorized\n");
      return;
    }

    std::string request;
    if (!read_pipe_line(pipe, svc_stop_event, request, 5000)) {
      return;
    }

    std::istringstream parser(request);
    std::string verb;
    std::string managed_token;
    std::string host_key_field;
    std::string allow_alt_field;
    std::string config_dir_hex;
    if (!(parser >> verb >> managed_token >> host_key_field >> allow_alt_field) ||
        verb != "START" ||
        !is_lower_hex(managed_token, 64) ||
        (host_key_field != "-" && !is_lower_hex(host_key_field, 64)) ||
        (allow_alt_field != "0" && allow_alt_field != "1")) {
      write_pipe_bytes(pipe, write_mutex, "ERROR bad_request\n");
      return;
    }
    std::wstring config_dir;
    if (parser >> config_dir_hex) {
      config_dir = decode_utf8_hex_path(config_dir_hex);
      if (config_dir.empty()) {
        write_pipe_bytes(pipe, write_mutex, "ERROR bad_config_dir\n");
        return;
      }
    }
    const std::string host_key = host_key_field == "-" ? std::string {} : host_key_field;
    const bool allow_alt_ports = allow_alt_field == "1";

    const auto host_dir = InstallDirectory();
    const auto core_path = host_dir + L"\\sunshine.exe";
    const auto console_token = DuplicateTokenForSession(client_session_id);
    if (console_token == NULL) {
      WriteServiceLog(log_file_handle, "DuplicateTokenForSession failed: " + std::to_string(GetLastError()));
      write_pipe_bytes(pipe, write_mutex, "ERROR token\n");
      return;
    }

    auto env_block = build_environment_block(console_token, managed_token, host_key, allow_alt_ports, config_dir);
    const auto job_handle = CreateJobObjectForChildProcess();
    if (job_handle == NULL) {
      CloseHandle(console_token);
      write_pipe_bytes(pipe, write_mutex, "ERROR job\n");
      return;
    }

    SECURITY_ATTRIBUTES inherit_sa = {};
    inherit_sa.nLength = sizeof(inherit_sa);
    inherit_sa.bInheritHandle = TRUE;

    HANDLE child_stdin_r = NULL;
    HANDLE child_stdin_w = NULL;
    HANDLE child_stdout_r = NULL;
    HANDLE child_stdout_w = NULL;
    HANDLE nul_stderr = INVALID_HANDLE_VALUE;
    auto close_pipes = [&]() {
      if (child_stdin_r) CloseHandle(child_stdin_r);
      if (child_stdin_w) CloseHandle(child_stdin_w);
      if (child_stdout_r) CloseHandle(child_stdout_r);
      if (child_stdout_w) CloseHandle(child_stdout_w);
      if (nul_stderr != INVALID_HANDLE_VALUE) CloseHandle(nul_stderr);
      child_stdin_r = child_stdin_w = child_stdout_r = child_stdout_w = NULL;
      nul_stderr = INVALID_HANDLE_VALUE;
    };

    if (!CreatePipe(&child_stdin_r, &child_stdin_w, &inherit_sa, 0) ||
        !SetHandleInformation(child_stdin_w, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&child_stdout_r, &child_stdout_w, &inherit_sa, 0) ||
        !SetHandleInformation(child_stdout_r, HANDLE_FLAG_INHERIT, 0)) {
      close_pipes();
      CloseHandle(job_handle);
      CloseHandle(console_token);
      write_pipe_bytes(pipe, write_mutex, "ERROR pipe\n");
      return;
    }

    nul_stderr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit_sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (nul_stderr == INVALID_HANDLE_VALUE) {
      close_pipes();
      CloseHandle(job_handle);
      CloseHandle(console_token);
      write_pipe_bytes(pipe, write_mutex, "ERROR nul\n");
      return;
    }

    STARTUPINFOEXW startup_info = {};
    startup_info.StartupInfo.cb = sizeof(startup_info);
    startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup_info.StartupInfo.hStdInput = child_stdin_r;
    startup_info.StartupInfo.hStdOutput = child_stdout_w;
    startup_info.StartupInfo.hStdError = nul_stderr;
    startup_info.lpAttributeList = AllocateProcThreadAttributeList(2);
    if (startup_info.lpAttributeList == NULL) {
      close_pipes();
      CloseHandle(job_handle);
      CloseHandle(console_token);
      write_pipe_bytes(pipe, write_mutex, "ERROR attr\n");
      return;
    }

    HANDLE inherited_handles[3] = { child_stdin_r, child_stdout_w, nul_stderr };
    HANDLE job_copy = job_handle;
    UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_copy, sizeof(job_copy), NULL, NULL);
    UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles, sizeof(inherited_handles), NULL, NULL);

    PROCESS_INFORMATION process_info = {};
    const BOOL launched = CreateProcessAsUserW(
      console_token,
      core_path.c_str(),
      NULL,
      NULL,
      NULL,
      TRUE,
      CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
      env_block.data(),
      host_dir.c_str(),
      reinterpret_cast<LPSTARTUPINFOW>(&startup_info),
      &process_info);
    const DWORD launch_error = launched ? ERROR_SUCCESS : GetLastError();

    DeleteProcThreadAttributeList(startup_info.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, startup_info.lpAttributeList);
    CloseHandle(console_token);
    CloseHandle(child_stdin_r);
    CloseHandle(child_stdout_w);
    CloseHandle(nul_stderr);
    child_stdin_r = NULL;
    child_stdout_w = NULL;
    nul_stderr = INVALID_HANDLE_VALUE;

    if (!launched) {
      WriteServiceLog(log_file_handle, "SunAndMonHostService failed to launch sunshine.exe: " + std::to_string(launch_error));
      CloseHandle(child_stdin_w);
      CloseHandle(child_stdout_r);
      CloseHandle(job_handle);
      write_pipe_bytes(pipe, write_mutex, "ERROR launch\n");
      return;
    }

    WriteServiceLog(log_file_handle,
      "SunAndMonHostService launched sunshine.exe PID " + std::to_string(process_info.dwProcessId) +
        " in session " + std::to_string(client_session_id) + " for client PID " + std::to_string(client_pid));

    std::thread stdout_thread([child_stdout_r, pipe, &write_mutex]() {
      std::string pending;
      char buffer[1024];
      DWORD bytes_read = 0;
      while (ReadFile(child_stdout_r, buffer, sizeof(buffer), &bytes_read, NULL) && bytes_read > 0) {
        pending.append(buffer, bytes_read);
        size_t newline = std::string::npos;
        while ((newline = pending.find('\n')) != std::string::npos) {
          std::string line = pending.substr(0, newline);
          pending.erase(0, newline + 1);
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }
          if (line.rfind("SUNANDMON_PORTS ", 0) == 0 || line.rfind("SUNANDMON_PORT_CONFLICT ", 0) == 0) {
            write_pipe_bytes(pipe, write_mutex, line + "\n");
          }
        }
        if (pending.size() > 8192) {
          pending.clear();
        }
      }
    });

    const auto pipe_read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    bool child_exited_naturally = false;
    if (pipe_read_event != NULL) {
      char control_buf[128];
      std::string control_pending;
      while (true) {
        ResetEvent(pipe_read_event);
        OVERLAPPED read_ov = {};
        read_ov.hEvent = pipe_read_event;
        DWORD control_read = 0;
        BOOL immediate = ReadFile(pipe, control_buf, sizeof(control_buf), &control_read, &read_ov);
        if (!immediate) {
          const auto err = GetLastError();
          if (err != ERROR_IO_PENDING) {
            break;
          }
          const HANDLE waits[] = { svc_stop_event, process_info.hProcess, pipe_read_event };
          const auto waited = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
          if (waited == WAIT_OBJECT_0) {
            CancelIoEx(pipe, &read_ov);
            break;
          }
          if (waited == WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe, &read_ov);
            child_exited_naturally = true;
            break;
          }
          if (waited != WAIT_OBJECT_0 + 2 || !GetOverlappedResult(pipe, &read_ov, &control_read, FALSE)) {
            break;
          }
        }
        if (control_read == 0) {
          break;
        }
        control_pending.append(control_buf, control_read);
        if (control_pending.find("STOP") != std::string::npos) {
          break;
        }
        if (control_pending.size() > 1024) {
          control_pending.erase(0, control_pending.size() - 64);
        }
      }
      CloseHandle(pipe_read_event);
    }

    if (child_stdin_w != NULL) {
      CloseHandle(child_stdin_w);
      child_stdin_w = NULL;
    }

    if (!child_exited_naturally) {
      const HANDLE stop_waits[] = { process_info.hProcess, svc_stop_event };
      WaitForMultipleObjects(2, stop_waits, FALSE, 10000);
    }

    CloseHandle(job_handle);
    if (stdout_thread.joinable()) {
      stdout_thread.join();
    }

    DWORD exit_code = 0;
    if (GetExitCodeProcess(process_info.hProcess, &exit_code)) {
      if (child_exited_naturally) {
        write_pipe_bytes(pipe, write_mutex, "EXIT " + std::to_string(exit_code) + "\n");
        FlushFileBuffers(pipe);
      }
    }

    CloseHandle(child_stdout_r);
    if (process_info.hThread != NULL) {
      CloseHandle(process_info.hThread);
    }
    CloseHandle(process_info.hProcess);
  }

  VOID WINAPI
  ServiceMain(DWORD, LPTSTR *) {
    service_status_handle = RegisterServiceCtrlHandlerEx(kServiceNameA, HandlerEx, NULL);
    if (service_status_handle == NULL) {
      return;
    }

    service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service_status.dwServiceSpecificExitCode = 0;
    service_status.dwWin32ExitCode = NO_ERROR;
    service_status.dwWaitHint = 0;
    service_status.dwControlsAccepted = 0;
    service_status.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(service_status_handle, &service_status);

    stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    session_change_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (stop_event == NULL || session_change_event == NULL) {
      service_status.dwCurrentState = SERVICE_STOPPED;
      service_status.dwWin32ExitCode = GetLastError();
      SetServiceStatus(service_status_handle, &service_status);
      return;
    }

    auto log_file_handle = sunshinesvc::open_primary_log_handle();
    if (log_file_handle == INVALID_HANDLE_VALUE) {
      log_file_handle = sunshinesvc::open_fallback_log_handle();
    }
    PSECURITY_DESCRIPTOR pipe_sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddlW, SDDL_REVISION_1, &pipe_sd, nullptr)) {
      WriteServiceLog(log_file_handle, "ConvertStringSecurityDescriptorToSecurityDescriptorW failed: " + std::to_string(GetLastError()));
      service_status.dwCurrentState = SERVICE_STOPPED;
      service_status.dwWin32ExitCode = GetLastError();
      SetServiceStatus(service_status_handle, &service_status);
      return;
    }

    SECURITY_ATTRIBUTES pipe_sa = {};
    pipe_sa.nLength = sizeof(pipe_sa);
    pipe_sa.lpSecurityDescriptor = pipe_sd;
    pipe_sa.bInheritHandle = FALSE;

    service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN;
    service_status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(service_status_handle, &service_status);
    WriteServiceLog(log_file_handle, "SunAndMonHostService started");

    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
      const auto pipe = CreateNamedPipeW(
        kPipeNameW,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        4096,
        4096,
        0,
        &pipe_sa);
      if (pipe == INVALID_HANDLE_VALUE) {
        WaitForSingleObject(stop_event, 500);
        continue;
      }

      const auto connect_event = CreateEventW(NULL, TRUE, FALSE, NULL);
      if (connect_event == NULL) {
        CloseHandle(pipe);
        WaitForSingleObject(stop_event, 500);
        continue;
      }

      OVERLAPPED connect_ov = {};
      connect_ov.hEvent = connect_event;
      bool connected = ConnectNamedPipe(pipe, &connect_ov) != FALSE;
      if (!connected) {
        const auto err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
          connected = true;
        }
        else if (err == ERROR_IO_PENDING) {
          const HANDLE waits[] = { stop_event, connect_event };
          const auto waited = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
          if (waited == WAIT_OBJECT_0 + 1) {
            DWORD unused = 0;
            connected = GetOverlappedResult(pipe, &connect_ov, &unused, FALSE) != FALSE;
          }
          else {
            CancelIoEx(pipe, &connect_ov);
          }
        }
      }
      CloseHandle(connect_event);

      if (connected && WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        run_session(pipe, stop_event, log_file_handle);
        DisconnectNamedPipe(pipe);
      }
      CloseHandle(pipe);
    }

    LocalFree(pipe_sd);
    if (log_file_handle != INVALID_HANDLE_VALUE) {
      WriteServiceLog(log_file_handle, "SunAndMonHostService stopped");
      CloseHandle(log_file_handle);
    }
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
  }

  bool
  wait_for_service_state(SC_HANDLE service, DWORD desired_state, DWORD timeout_ms) {
    const auto deadline = GetTickCount64() + timeout_ms;
    while (true) {
      SERVICE_STATUS_PROCESS status = {};
      DWORD needed = 0;
      if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed)) {
        return false;
      }
      if (status.dwCurrentState == desired_state) {
        return true;
      }
      if (GetTickCount64() >= deadline) {
        return false;
      }
      Sleep(150);
    }
  }

  int
  InstallService() {
    const auto self = self_module_path();
    if (self.empty()) {
      return 1;
    }
    const std::wstring bin_path = L"\"" + self + L"\" --sunandmon-service";
    const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (manager == NULL) {
      return static_cast<int>(GetLastError());
    }

    auto service = OpenServiceW(manager, kServiceNameW, SERVICE_ALL_ACCESS);
    bool needs_restart = false;
    if (service != NULL) {
      DWORD bytes_needed = 0;
      QueryServiceConfigW(service, nullptr, 0, &bytes_needed);
      if (bytes_needed > 0) {
        std::vector<BYTE> buf(bytes_needed);
        auto *cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buf.data());
        if (QueryServiceConfigW(service, cfg, bytes_needed, &bytes_needed) && cfg->lpBinaryPathName) {
          needs_restart = _wcsicmp(cfg->lpBinaryPathName, bin_path.c_str()) != 0;
        }
      }
      if (!ChangeServiceConfigW(
            service,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            bin_path.c_str(),
            nullptr,
            nullptr,
            nullptr,
            L"LocalSystem",
            nullptr,
            kDisplayNameW)) {
        const auto err = static_cast<int>(GetLastError());
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return err;
      }
    }
    else {
      service = CreateServiceW(
        manager,
        kServiceNameW,
        kDisplayNameW,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        bin_path.c_str(),
        nullptr,
        nullptr,
        nullptr,
        L"LocalSystem",
        nullptr);
      if (service == NULL) {
        const auto err = static_cast<int>(GetLastError());
        CloseServiceHandle(manager);
        return err;
      }
    }

    SERVICE_DESCRIPTIONW description = {};
    description.lpDescription = const_cast<LPWSTR>(kDescriptionW);
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

    if (needs_restart) {
      SERVICE_STATUS unused = {};
      ControlService(service, SERVICE_CONTROL_STOP, &unused);
      wait_for_service_state(service, SERVICE_STOPPED, 15000);
    }

    if (!StartServiceW(service, 0, nullptr)) {
      const auto err = GetLastError();
      if (err != ERROR_SERVICE_ALREADY_RUNNING) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return static_cast<int>(err);
      }
    }

    const bool running = wait_for_service_state(service, SERVICE_RUNNING, 15000);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return running ? 0 : ERROR_TIMEOUT;
  }

  int
  UninstallService() {
    const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (manager == NULL) {
      return static_cast<int>(GetLastError());
    }
    const auto service = OpenServiceW(manager, kServiceNameW, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (service == NULL) {
      const auto err = GetLastError();
      CloseServiceHandle(manager);
      return err == ERROR_SERVICE_DOES_NOT_EXIST ? 0 : static_cast<int>(err);
    }

    SERVICE_STATUS unused = {};
    if (ControlService(service, SERVICE_CONTROL_STOP, &unused)) {
      wait_for_service_state(service, SERVICE_STOPPED, 15000);
    }
    const BOOL deleted = DeleteService(service);
    const auto err = deleted ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return (err == ERROR_SUCCESS || err == ERROR_SERVICE_MARKED_FOR_DELETE) ? 0 : static_cast<int>(err);
  }
}  // namespace sunandmon_svc

int
main(int argc, char *argv[]) {
  static const SERVICE_TABLE_ENTRY service_table[] = {
    { (LPSTR) SERVICE_NAME, ServiceMain },
    { NULL, NULL }
  };
  static const SERVICE_TABLE_ENTRY sunandmon_service_table[] = {
    { (LPSTR) sunandmon_svc::kServiceNameA, sunandmon_svc::ServiceMain },
    { NULL, NULL }
  };

  if (argc == 2 && strcmp(argv[1], "--install-sunandmon-service") == 0) {
    return sunandmon_svc::InstallService();
  }
  if (argc == 2 && strcmp(argv[1], "--uninstall-sunandmon-service") == 0) {
    return sunandmon_svc::UninstallService();
  }

  // Check if this is a reinvocation of ourselves to send Ctrl-C to Sunshine.exe
  if (argc == 3 && strcmp(argv[1], "--terminate") == 0) {
    return DoGracefulTermination(atol(argv[2]));
  }

  const bool sunandmon_mode = argc == 2 && strcmp(argv[1], "--sunandmon-service") == 0;

  // By default, services have their current directory set to %SYSTEMROOT%\System32.
  // We want to use the directory where Sunshine.exe is located instead of system32.
  // This requires stripping off 2 path components: the file name and the last folder
  WCHAR module_path[MAX_PATH];
  GetModuleFileNameW(NULL, module_path, _countof(module_path));
  for (auto i = 0; i < 2; i++) {
    auto last_sep = wcsrchr(module_path, '\\');
    if (last_sep) {
      *last_sep = 0;
    }
  }
  SetCurrentDirectoryW(module_path);

  // A crash of the service wrapper itself leaves a report next to the host's own (the host uploads it; this process has no uploader)
  {
    crash::options_t crash_options;
#ifdef SUNSHINESVC_VERSION
    crash_options.version = SUNSHINESVC_VERSION;
#endif
    crash_options.os_version = "Windows";
    crash_options.config_dir = std::filesystem::path(module_path) / L"config";
    crash::install(crash_options);
  }

  // Trigger our ServiceMain()
  const auto dispatcher_result = StartServiceCtrlDispatcher(sunandmon_mode ? sunandmon_service_table : service_table);

  // The crash watchdog is a static std::thread: left running, its destructor finds it joinable while the CRT tears the
  // process down and calls std::terminate, so every normal stop of the service would end as an abort report.
  crash::stop_watchdog();
  return dispatcher_result;
}
