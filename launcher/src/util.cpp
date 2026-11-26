#include "util.h"

#include <SDL3/SDL.h>

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
