#include "util.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

std::string u8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

FILE* open_file(const fs::path& p, const char* mode) {
#ifdef _WIN32
  std::wstring m(mode, mode + std::char_traits<char>::length(mode));
  return _wfopen(p.c_str(), m.c_str());
#else
  return std::fopen(p.c_str(), mode);
#endif
}

bool read_file(const fs::path& p, std::vector<uint8_t>& out) {
  FILE* f = open_file(p, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? static_cast<size_t>(n) : 0);
  const bool ok = n >= 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

bool write_file(const fs::path& p, const void* data, size_t size) {
  FILE* f = open_file(p, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(data, 1, size, f) == size;
  return std::fclose(f) == 0 && ok;
}

fs::path launcher_dir() {
  const char* base = SDL_GetBasePath();
  return base ? from_u8(base) : fs::current_path();
}

static SDL_PropertiesID process_props(const std::vector<const char*>& argv, const fs::path& working_dir,
                                      const std::string& wd) {
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(argv.data()));
  if (!working_dir.empty())
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, wd.c_str());
  return props;
}

#ifdef _WIN32
// Windows: CreateProcess directly, with CREATE_NO_WINDOW. The launcher is a GUI program with no
// console, so every console child (zig, and the clang processes zig starts) would otherwise get
// a console window of its own. With this flag they share one hidden console.
static std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

// One argument quoted the way CommandLineToArgvW / the C runtime split it back.
static void append_quoted(std::wstring& cmd, const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    cmd += arg;
    return;
  }
  cmd += L'"';
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < arg.size() && arg[i] == L'\\') ++i, ++slashes;
    if (i == arg.size()) {
      cmd.append(slashes * 2, L'\\');  // before the closing quote
      break;
    }
    if (arg[i] == L'"') {
      cmd.append(slashes * 2 + 1, L'\\');
      cmd += L'"';
    } else {
      cmd.append(slashes, L'\\');
      cmd += arg[i];
    }
  }
  cmd += L'"';
}

bool run_process(const std::vector<std::string>& args, std::string& output, int& exit_code,
                 const fs::path& working_dir) {
  output.clear();
  exit_code = -1;
  std::wstring cmd;
  for (const auto& a : args) {
    if (!cmd.empty()) cmd += L' ';
    append_quoted(cmd, widen(a));
  }

  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    output = "CreatePipe failed";
    return false;
  }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW si = {};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = wr;
  si.hStdError = wr;
  PROCESS_INFORMATION pi = {};
  const std::wstring wd = working_dir.empty() ? std::wstring() : working_dir.wstring();
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                 nullptr, wd.empty() ? nullptr : wd.c_str(), &si, &pi);
  const DWORD err = GetLastError();
  CloseHandle(wr);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) {
    CloseHandle(rd);
    char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, buf, sizeof buf, nullptr);
    output = "cannot run " + (args.empty() ? std::string() : args[0]) + ": " + buf;
    return false;
  }
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) output.append(buf, n);
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  exit_code = (int)code;
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return true;
}
#else
bool run_process(const std::vector<std::string>& args, std::string& output, int& exit_code,
                 const fs::path& working_dir) {
  std::vector<const char*> argv;
  for (const auto& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  const std::string wd = u8(working_dir);
  SDL_PropertiesID props = process_props(argv, working_dir, wd);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
  SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
  SDL_Process* proc = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  if (!proc) {
    output = SDL_GetError();
    exit_code = -1;
    return false;
  }
  size_t n = 0;
  exit_code = -1;
  void* data = SDL_ReadProcess(proc, &n, &exit_code);
  output.assign(data ? static_cast<const char*>(data) : "", data ? n : 0);
  SDL_free(data);
  SDL_DestroyProcess(proc);
  return true;
}
#endif

bool spawn_detached(const std::vector<std::string>& args, const fs::path& working_dir) {
  std::vector<const char*> argv;
  for (const auto& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  const std::string wd = u8(working_dir);
  SDL_PropertiesID props = process_props(argv, working_dir, wd);
  SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
  SDL_Process* proc = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  if (!proc) return false;
  SDL_DestroyProcess(proc);  // keeps running
  return true;
}
