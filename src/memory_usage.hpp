#pragma once

#include <cstddef>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <fstream>
#include <limits.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach-o/dyld.h>
#endif
#endif

namespace sentrix {

inline std::size_t current_rss_bytes() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))
               ? static_cast<std::size_t>(pmc.WorkingSetSize) : 0;
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::size_t total = 0, resident = 0;
    const long page = sysconf(_SC_PAGESIZE);
    return (statm >> total >> resident) && page > 0
               ? resident * static_cast<std::size_t>(page) : 0;
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                     reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS
               ? static_cast<std::size_t>(info.resident_size) : 0;
#else
    return 0;
#endif
}

inline std::size_t peak_rss_bytes() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))
               ? static_cast<std::size_t>(pmc.PeakWorkingSetSize) : 0;
#elif defined(__APPLE__)
    struct rusage u {};
    return (getrusage(RUSAGE_SELF, &u) == 0 && u.ru_maxrss > 0) ? static_cast<std::size_t>(u.ru_maxrss) : 0;
#elif defined(__linux__)
    struct rusage u {};
    return (getrusage(RUSAGE_SELF, &u) == 0 && u.ru_maxrss > 0)
               ? static_cast<std::size_t>(u.ru_maxrss) * 1024ULL : 0;
#else
    return 0;
#endif
}

inline std::string executable_path(const char* argv0) {
#if defined(_WIN32)
    char buf[MAX_PATH];
    const DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return std::string(buf, static_cast<std::size_t>(len));
#elif defined(__linux__)
    char buf[PATH_MAX];
    const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) return std::string(buf, static_cast<std::size_t>(len));
#elif defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return std::string(buf);
#endif
    return argv0 ? std::string(argv0) : std::string{};
}

inline bool run_subprocess_capture(const std::string& exe_path,
                                   const std::vector<std::string>& args,
                                   std::string& stdout_output) {
    stdout_output.clear();
    if (exe_path.empty()) return false;

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "\"" + exe_path + "\"";
    for (const auto& a : args) cmd += " \"" + a + "\"";
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(exe_path.c_str(), mutable_cmd.data(), nullptr, nullptr,
                                   TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }

    char chunk[512];
    DWORD n = 0;
    while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
        stdout_output.append(chunk, static_cast<std::size_t>(n));
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
#elif defined(__linux__) || defined(__APPLE__)
    int fd[2];
    if (pipe(fd) != 0) return false;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return false;
    }
    if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        close(fd[1]);
        std::string prog = exe_path;
        std::vector<std::string> copy = args;
        std::vector<char*> argv_ptrs{prog.data()};
        for (auto& a : copy) argv_ptrs.push_back(a.data());
        argv_ptrs.push_back(nullptr);
        execv(exe_path.c_str(), argv_ptrs.data());
        _exit(127);
    }
    close(fd[1]);
    char chunk[512];
    ssize_t n = 0;
    while ((n = read(fd[0], chunk, sizeof(chunk))) > 0) {
        stdout_output.append(chunk, static_cast<std::size_t>(n));
    }
    close(fd[0]);
    int status = 0;
    return waitpid(pid, &status, 0) >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
    (void)args;
    return false;
#endif
}

} // namespace sentrix
