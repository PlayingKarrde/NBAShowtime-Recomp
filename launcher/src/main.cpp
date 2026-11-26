// nbalauncher: builds a playable NBA Showtime from the user's own ROM set. It is only needed
// once: the result is a standalone folder with the game executable and its data.
//
// usage: nbalauncher                                   (window)
//        nbalauncher --rom PATH --out DIR [--keep-build] [--jobs N]   (no window)
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "builder.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

namespace {

fs::path default_install_dir() {
  const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME);
  return (home ? from_u8(home) : launcher_dir()) / "Games" / "NBA Showtime";
}

void print_romset(const RomSet& rs) {
  std::printf("BIOS:      %s\n", rs.bios.found ? rs.bios.where.c_str() : "MISSING (showtime_mar15_1999.u27)");
  std::printf("Sound ROM: %s\n", rs.sound.found ? rs.sound.where.c_str() : "MISSING (vegassio.bin)");
  std::printf("Disk:      %s\n", rs.disk.found ? rs.disk.where.c_str() : "MISSING (nbashowt.chd)");
  for (const auto& n : rs.notes) std::printf("note: %s\n", n.c_str());
}

int run_cli(int argc, char** argv) {
  fs::path rom, out = default_install_dir();
  BuildOptions opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
    if (a == "--rom" && next) rom = from_u8(argv[++i]);
    else if (a == "--out" && next) out = from_u8(argv[++i]);
    else if (a == "--jobs" && next) opt.jobs = std::atoi(argv[++i]);
    else if (a == "--keep-build") opt.keep_build_files = true;
    else {
      std::fprintf(stderr, "usage: nbalauncher [--rom PATH --out DIR [--keep-build] [--jobs N]]\n");
      return 2;
    }
  }
  if (rom.empty()) {
    std::fprintf(stderr, "--rom is required\n");
    return 2;
  }
  const Toolchain tc = find_toolchain();
  for (const auto& p : tc.problems) std::fprintf(stderr, "toolchain: %s\n", p.c_str());
  if (!tc.ok()) return 1;
  opt.roms = scan_romset(rom);
  print_romset(opt.roms);
  if (!opt.roms.complete()) return 1;
  opt.install_dir = out;
  BuildState st;
  st.echo_stdout = true;
  return run_build(opt, tc, st) ? 0 : 1;
}

/* ---- window ---- */
struct App {
  SDL_Window* window = nullptr;
  std::mutex m;  // guards the fields below that dialogs and threads write
  std::string rom_input, install_dir;
  bool rom_dirty = false, install_dirty = false;

  std::unique_ptr<RomSet> roms;
  bool scanning = false;
  std::thread scan_thread;

  std::unique_ptr<Toolchain> tc;
  bool tc_loading = true;
  std::thread tc_thread;

  std::unique_ptr<BuildState> build;
  std::thread build_thread;
  bool keep_build = false;
  std::string status;

  char rom_buf[1024] = {}, dir_buf[1024] = {};
};

void SDLCALL on_rom_picked(void* ud, const char* const* files, int) {
  App* app = static_cast<App*>(ud);
  if (files && files[0]) {
    std::lock_guard<std::mutex> lk(app->m);
    app->rom_input = files[0];
    app->rom_dirty = true;
  }
}

void SDLCALL on_dir_picked(void* ud, const char* const* files, int) {
  App* app = static_cast<App*>(ud);
  if (files && files[0]) {
    std::lock_guard<std::mutex> lk(app->m);
    app->install_dir = files[0];
    app->install_dirty = true;
  }
}

void start_scan(App& app, const std::string& path) {
  if (app.scan_thread.joinable()) app.scan_thread.join();
  app.scanning = true;
  app.roms.reset();
  app.scan_thread = std::thread([&app, path] {
    auto rs = std::make_unique<RomSet>(scan_romset(from_u8(path)));
    std::lock_guard<std::mutex> lk(app.m);
    app.roms = std::move(rs);
    app.scanning = false;
  });
}

void status_line(bool ok, const char* label, const std::string& detail) {
  ImGui::TextColored(ok ? ImVec4(0.4f, 0.85f, 0.4f, 1) : ImVec4(0.95f, 0.45f, 0.4f, 1), ok ? "  OK  " : "  --  ");
  ImGui::SameLine();
  ImGui::Text("%s", label);
  if (!detail.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", detail.c_str());
  }
}

// Width of a row of buttons with these labels, including spacing before each.
float buttons_width(std::initializer_list<const char*> labels) {
  const ImGuiStyle& st = ImGui::GetStyle();
  float w = 0;
  for (const char* l : labels) w += ImGui::CalcTextSize(l).x + st.FramePadding.x * 2 + st.ItemSpacing.x;
  return w;
}

void draw(App& app) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("##main", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

  {
    std::lock_guard<std::mutex> lk(app.m);
    if (app.rom_dirty) {
      std::snprintf(app.rom_buf, sizeof app.rom_buf, "%s", app.rom_input.c_str());
      app.rom_dirty = false;
      start_scan(app, app.rom_input);
    }
    if (app.install_dirty) {
      std::snprintf(app.dir_buf, sizeof app.dir_buf, "%s", app.install_dir.c_str());
      app.install_dirty = false;
    }
  }
  const bool building = app.build && app.build->running;

  ImGui::SeparatorText("NBA Showtime: build from your ROM");
  ImGui::TextWrapped(
      "This builds a playable copy of NBA Showtime (NBA on NBC, ver 2.0) from your own ROM set. "
      "You only need to do this once: afterwards run the game straight from the install folder.");
  ImGui::Spacing();

  ImGui::SeparatorText("1. ROM set");
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("The NBA Showtime zip, a MAME rom folder (nbashowt.zip + nbashowt/nbashowt.chd), or any folder "
                     "with those files. You can also drop it on this window.");
  ImGui::PopStyleColor();
  ImGui::BeginDisabled(building);
  ImGui::SetNextItemWidth(-buttons_width({"Choose file...", "Choose folder..."}));
  if (ImGui::InputText("##rom", app.rom_buf, sizeof app.rom_buf, ImGuiInputTextFlags_EnterReturnsTrue) ||
      ImGui::IsItemDeactivatedAfterEdit())
    start_scan(app, app.rom_buf);
  ImGui::SameLine();
  if (ImGui::Button("Choose file...")) {
    static const SDL_DialogFileFilter filters[] = {{"ROM set (zip, chd)", "zip;chd"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(on_rom_picked, &app, app.window, filters, 2, nullptr, false);
  }
  ImGui::SameLine();
  if (ImGui::Button("Choose folder...")) SDL_ShowOpenFolderDialog(on_rom_picked, &app, app.window, nullptr, false);
  ImGui::EndDisabled();
  {
    std::lock_guard<std::mutex> lk(app.m);
    if (app.scanning) ImGui::TextDisabled("  Looking for the ROM files...");
    else if (app.roms) {
      const RomSet& rs = *app.roms;
      status_line(rs.bios.found, "BIOS (showtime_mar15_1999.u27)", rs.bios.found ? rs.bios.where : "");
      status_line(rs.sound.found, "Sound ROM (vegassio.bin)", rs.sound.found ? rs.sound.where : "");
      status_line(rs.disk.found, "Hard disk (nbashowt.chd)", rs.disk.found ? rs.disk.where : "");
      for (const auto& n : rs.notes) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "  %s", n.c_str());
    }
  }

  ImGui::SeparatorText("2. Install folder");
  ImGui::BeginDisabled(building);
  ImGui::SetNextItemWidth(-buttons_width({"Choose..."}));
  ImGui::InputText("##dir", app.dir_buf, sizeof app.dir_buf);
  ImGui::SameLine();
  if (ImGui::Button("Choose...")) SDL_ShowOpenFolderDialog(on_dir_picked, &app, app.window, app.dir_buf, false);
  ImGui::Checkbox("Keep build files (generated C code and objects, for debugging)", &app.keep_build);
  ImGui::EndDisabled();

  ImGui::SeparatorText("3. Build");
  bool tc_ok = false;
  {
    std::lock_guard<std::mutex> lk(app.m);
    if (app.tc_loading) ImGui::TextDisabled("  Checking the compiler...");
    else if (app.tc) {
      tc_ok = app.tc->ok();
      if (tc_ok) status_line(true, "Compiler", "zig " + app.tc->zig_version);
      for (const auto& p : app.tc->problems) status_line(false, p.c_str(), "");
    }
  }
  bool ready;
  {
    std::lock_guard<std::mutex> lk(app.m);
    ready = tc_ok && app.roms && app.roms->complete() && app.dir_buf[0] && !building;
  }
  ImGui::BeginDisabled(!ready);
  if (ImGui::Button(building ? "Building..." : "Build", ImVec2(ImGui::GetFontSize() * 8, 0))) {
    if (app.build_thread.joinable()) app.build_thread.join();
    app.build = std::make_unique<BuildState>();
    app.build->running = true;
    BuildOptions opt;
    {
      std::lock_guard<std::mutex> lk(app.m);
      opt.roms = *app.roms;
    }
    opt.install_dir = from_u8(app.dir_buf);
    opt.keep_build_files = app.keep_build;
    const Toolchain tc = *app.tc;
    BuildState* st = app.build.get();
    app.build_thread = std::thread([st, opt, tc] {
      run_build(opt, tc, *st);
      st->finished = true;
      st->running = false;
    });
  }
  ImGui::EndDisabled();
  if (building) {
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) app.build->cancel = true;
  }

  if (app.build) {
    BuildState& st = *app.build;
    std::string step;
    float progress;
    {
      std::lock_guard<std::mutex> lk(st.m);
      step = st.step;
      progress = st.progress;
    }
    ImGui::SameLine();
    ImGui::ProgressBar(progress, ImVec2(-1, 0), step.c_str());
    if (st.finished && !st.failed) {
      ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1), "NBA Showtime is ready in %s", app.dir_buf);
      ImGui::TextDisabled("You can start it from there from now on; the launcher is no longer needed.");
      if (ImGui::Button("Play", ImVec2(ImGui::GetFontSize() * 8, 0))) {
        const fs::path dir = from_u8(app.dir_buf);
        if (!spawn_detached({u8(game_executable(dir))}, dir)) app.status = SDL_GetError();
      }
      ImGui::SameLine();
      if (ImGui::Button("Open folder")) SDL_OpenURL(("file://" + std::string(app.dir_buf)).c_str());
      if (!app.status.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", app.status.c_str());
    } else if (st.finished) {
      ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "The build failed: see the log below.");
    }
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    {
      std::lock_guard<std::mutex> lk(st.m);
      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(st.log.size()));
      while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) ImGui::TextUnformatted(st.log[i].c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
  }
  ImGui::End();
}

int run_window() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  App app;
  app.window = SDL_CreateWindow("NBA Showtime Launcher", int(860 * scale), int(640 * scale),
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!app.window) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Renderer* renderer = SDL_CreateRenderer(app.window, nullptr);
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;
  style.FontSizeBase = 15.0f;
  style.FrameRounding = 3.0f;
  ImGui_ImplSDL3_InitForSDLRenderer(app.window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  std::snprintf(app.dir_buf, sizeof app.dir_buf, "%s", u8(default_install_dir()).c_str());
  app.tc_thread = std::thread([&app] {
    auto tc = std::make_unique<Toolchain>(find_toolchain());
    std::lock_guard<std::mutex> lk(app.m);
    app.tc = std::move(tc);
    app.tc_loading = false;
  });

  bool quit = false;
  while (!quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      if (e.type == SDL_EVENT_QUIT) quit = true;
      if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(app.window)) quit = true;
      if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) on_rom_picked(&app, &e.drop.data, 0);
    }
    if (SDL_GetWindowFlags(app.window) & SDL_WINDOW_MINIMIZED) {
      SDL_Delay(20);
      continue;
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw(app);
    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(renderer, 0.08f, 0.08f, 0.1f, 1.0f);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }
  if (app.build) app.build->cancel = true;
  if (app.build_thread.joinable()) app.build_thread.join();
  if (app.scan_thread.joinable()) app.scan_thread.join();
  if (app.tc_thread.joinable()) app.tc_thread.join();
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(app.window);
  SDL_Quit();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) return run_cli(argc, argv);
  return run_window();
}
