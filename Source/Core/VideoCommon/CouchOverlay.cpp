// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/CouchOverlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#ifdef _WIN32
#include <Windows.h>
#include <mmsystem.h>
#endif

#include "AudioCommon/AudioCommon.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/GCPad.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

namespace CouchOverlay
{
namespace
{
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------------------------

enum class Nav
{
  Up,
  Down,
  Left,
  Right,
  Accept,
  Back,
  Close,
  Promote,  // X: make the selected setting the system default
  Revert,   // Y: drop this game's override of the selected setting
};

enum class PageId
{
  Main,
  SaveState,
  LoadState,
  ChangeDisc,
  Settings,
  Graphics,
  Enhancements,
  Performance,
  Hacks,
  OnScreenDisplay,
  Emulation,
  Audio,
  Controls,
};

constexpr std::array SETTINGS_CATEGORIES = {
    PageId::Graphics,        PageId::Enhancements, PageId::Performance, PageId::Hacks,
    PageId::OnScreenDisplay, PageId::Emulation,    PageId::Audio,       PageId::Controls};

struct PageState
{
  PageId id = PageId::Main;
  int selected = 0;
  int scroll = 0;
};

constexpr u32 NUM_SLOTS = 10;

std::mutex s_mutex;
std::atomic<bool> s_open{false};

// Guarded by s_mutex.
std::deque<Nav> s_nav_events;
std::vector<PendingHostAction> s_host_actions;
std::vector<PageState> s_pages;
bool s_resume_on_close = false;
bool s_waiting_for_release = false;  // menu closed, holding the resume until buttons are let go
bool s_config_dirty = false;

// Save-state slot labels. Filled on the host thread because reading slot headers takes the
// save-state lock, which must never be waited on from the video thread.
bool s_slot_refresh_requested = false;
std::optional<Clock::time_point> s_slot_refresh_at;
std::array<std::string, NUM_SLOTS> s_slot_labels;
std::array<bool, NUM_SLOTS> s_slot_used{};

// "Press A again" confirmation for destructive rows.
std::string s_armed_item;
Clock::time_point s_armed_at;

// Disc candidates for the Change Disc page, also gathered on the host thread.
bool s_disc_refresh_requested = false;
std::vector<std::string> s_disc_paths;
std::string s_current_disc;
bool s_disc_scanned = false;

// Keys set in this game's LocalGame config layer (its User/GameSettings INI). Refreshed on the UI
// thread, which owns the config, after every change.
std::set<Config::Location> s_overridden;

// Choices for the settings pages that come from disk, gathered on the host thread.
bool s_lists_refresh_requested = false;
std::vector<std::string> s_shader_names;
std::vector<std::string> s_profile_names;

// Game the remembered menu position (s_pages) belongs to.
std::string s_menu_game_id;

// Paused redraw bookkeeping.
std::atomic<bool> s_redraw_queued{false};
Clock::time_point s_redraw_queued_at;
Clock::time_point s_last_redraw_request;

// Video thread only.
ImFont* s_font = nullptr;

// Host-thread-only input repeat state.
struct RepeatState
{
  bool held = false;
  Clock::time_point next;
};
std::array<RepeatState, 4> s_dir_repeat;
bool s_prev_accept = false, s_prev_back = false, s_prev_start = false;
bool s_prev_promote = false, s_prev_revert = false;

// Menu-wide preferences, saved in Dolphin.ini under [CouchOverlay]. They live here so no core
// settings file changes.
const Config::Info<std::string> COUCH_THEME{{Config::System::Main, "CouchOverlay", "Theme"},
                                            "Blue"};
// 0 = off, 1-3 = quiet to loud.
const Config::Info<int> COUCH_SOUND_LEVEL{{Config::System::Main, "CouchOverlay", "Sounds"}, 2};
// Which Couch Potato baseline (see ApplyBaselineValues) this config has received.
const Config::Info<int> COUCH_BASELINE_VERSION{
    {Config::System::Main, "CouchOverlay", "BaselineVersion"}, 0};
constexpr int BASELINE_VERSION = 1;

// ---------------------------------------------------------------------------------------------
// Menu sounds
// ---------------------------------------------------------------------------------------------

// In increasing priority: when several land in one frame, the most important one plays.
enum class Sound
{
  Move,
  Change,
  Deny,
  Select,
  Back,
  Open,
  Close,
};
constexpr size_t NUM_SOUNDS = 7;
constexpr std::array<const char*, NUM_SOUNDS> SOUND_NAMES = {"move", "change", "deny", "select",
                                                             "back", "open",   "close"};

std::optional<Sound> s_pending_sound;  // guarded by s_mutex; played on the host thread

void QueueSoundLocked(Sound sound)
{
  if (!s_pending_sound || *s_pending_sound < sound)
    s_pending_sound = sound;
}

// Host thread only. Buffers are never freed, because PlaySound reads them while playing.
struct SoundBank
{
  std::array<std::vector<u8>, NUM_SOUNDS> wav;
};
std::array<std::optional<SoundBank>, 4> s_generated;  // per level
std::optional<std::array<std::vector<u8>, NUM_SOUNDS>> s_custom;

std::vector<u8> MakeWav(const std::vector<s16>& samples, u32 rate)
{
  std::vector<u8> out;
  const auto put = [&out](u32 v, int bytes) {
    for (int i = 0; i < bytes; ++i)
      out.push_back(u8(v >> (8 * i)));
  };
  const u32 data_size = u32(samples.size() * sizeof(s16));
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  put(36 + data_size, 4);
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put(16, 4);        // fmt chunk size
  put(1, 2);         // PCM
  put(1, 2);         // mono
  put(rate, 4);      // sample rate
  put(rate * 2, 4);  // byte rate
  put(2, 2);         // block align
  put(16, 2);        // bits per sample
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  put(data_size, 4);
  for (s16 v : samples)
    put(u16(v), 2);
  return out;
}

// A short pitched blip with a soft attack and exponential decay, plus a touch of noise for the
// "click" edge. f0 -> f1 is a linear pitch sweep.
std::vector<s16> Blip(float f0, float f1, float ms, float gain, float decay, float noise)
{
  constexpr u32 rate = 44100;
  const int n = int(rate * ms / 1000.0f);
  std::vector<s16> out(n + int(rate * 0.004f), 0);  // short silent tail avoids an end pop
  float phase = 0.0f;
  u32 rng = 0x12345678;
  for (int i = 0; i < n; ++i)
  {
    const float t = float(i) / float(n);
    phase += 2.0f * 3.14159265f * (f0 + (f1 - f0) * t) / float(rate);
    rng = rng * 1664525u + 1013904223u;
    const float white = float(rng >> 8) / float(1u << 24) * 2.0f - 1.0f;
    const float attack = std::min(1.0f, float(i) / (rate * 0.0015f));
    const float env = attack * std::exp(-decay * t) * (1.0f - t * t);
    const float v = (std::sin(phase) * (1.0f - noise) + white * noise * std::exp(-12.0f * t));
    out[i] = s16(std::clamp(v * env * gain, -1.0f, 1.0f) * 32767.0f);
  }
  return out;
}

const SoundBank& GeneratedSounds(int level)
{
  auto& bank = s_generated[level];
  if (!bank)
  {
    static constexpr std::array<float, 4> gains = {0.0f, 0.10f, 0.20f, 0.36f};
    const float g = gains[level];
    bank.emplace();
    const auto set = [&](Sound s, std::vector<s16> samples) {
      bank->wav[size_t(s)] = MakeWav(samples, 44100);
    };
    // Kodi/RetroArch-style: tiny high ticks for movement, rounder blips for actions.
    set(Sound::Move, Blip(1900.0f, 1700.0f, 16.0f, g * 0.8f, 7.0f, 0.25f));
    set(Sound::Change, Blip(2500.0f, 2300.0f, 18.0f, g * 0.8f, 6.0f, 0.2f));
    set(Sound::Deny, Blip(210.0f, 180.0f, 80.0f, g * 0.9f, 3.0f, 0.05f));
    set(Sound::Select, Blip(1050.0f, 1600.0f, 55.0f, g, 4.0f, 0.1f));
    set(Sound::Back, Blip(1300.0f, 800.0f, 55.0f, g, 4.0f, 0.1f));
    set(Sound::Open, Blip(520.0f, 1250.0f, 120.0f, g, 2.5f, 0.05f));
    set(Sound::Close, Blip(1250.0f, 520.0f, 120.0f, g, 2.5f, 0.05f));
  }
  return *bank;
}

// <User>/Load/CouchOverlay/Sounds/<name>.wav replaces the built-in sound of that name.
void LoadCustomSounds()
{
  s_custom.emplace();
  const std::string dir = File::GetUserPath(D_LOAD_IDX) + "CouchOverlay/Sounds/";
  for (size_t i = 0; i < NUM_SOUNDS; ++i)
  {
    std::string data;
    if (File::ReadFileToString(dir + SOUND_NAMES[i] + ".wav", data))
      (*s_custom)[i].assign(data.begin(), data.end());
  }
}

// Host thread.
void PlayPendingSound()
{
  std::optional<Sound> sound;
  {
    std::lock_guard lk(s_mutex);
    sound = std::exchange(s_pending_sound, std::nullopt);
  }
  if (!sound)
    return;
  const int level = std::clamp(Config::Get(COUCH_SOUND_LEVEL), 0, 3);
  if (level == 0)
    return;
  if (!s_custom)
    LoadCustomSounds();
  const size_t i = size_t(*sound);
  const std::vector<u8>& wav =
      !(*s_custom)[i].empty() ? (*s_custom)[i] : GeneratedSounds(level).wav[i];
#ifdef _WIN32
  // Async: a new sound cuts off the previous one, which suits quick menu ticks.
  PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr,
             SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
#else
  (void)wav;
#endif
}

// ---------------------------------------------------------------------------------------------
// Helpers usable from any thread
// ---------------------------------------------------------------------------------------------

void PushHostActionLocked(HostAction action, int arg = 0)
{
  s_host_actions.push_back({action, arg});
}

void RefreshOverrides();

// Reopens the menu where it was last closed (same page and row) while the same game is running.
void PrepareMenuLocked()
{
  const std::string& game_id = SConfig::GetInstance().GetGameID();
  if (s_pages.empty() || game_id != s_menu_game_id)
  {
    s_pages.clear();
    s_pages.push_back({});
    s_menu_game_id = game_id;
  }
  s_nav_events.clear();
  s_armed_item.clear();

  // Pages fill some of their contents when entered; refresh those for the restored pages.
  for (const PageState& page : s_pages)
  {
    switch (page.id)
    {
    case PageId::SaveState:
    case PageId::LoadState:
      s_slot_refresh_requested = true;
      break;
    case PageId::ChangeDisc:
      s_disc_refresh_requested = true;
      break;
    case PageId::Settings:
      s_lists_refresh_requested = true;
      Core::QueueHostJob([](Core::System&) { RefreshOverrides(); }, false);
      break;
    default:
      break;
    }
  }
}

// Writes settings changed from the menu to disk, on the UI thread.
void SaveConfigIfDirtyLocked()
{
  if (!s_config_dirty)
    return;
  s_config_dirty = false;
  Core::QueueHostJob([](Core::System&) { Config::Save(); }, true);
}

// Hides the menu. When resume is true, emulation continues once the buttons that closed the
// menu are released, so the confirming press never reaches the game.
void CloseLocked(bool resume)
{
  QueueSoundLocked(Sound::Close);
  SaveConfigIfDirtyLocked();
  s_open = false;
  s_nav_events.clear();
  s_armed_item.clear();
  s_resume_on_close = s_resume_on_close && resume;
  s_waiting_for_release = true;
}

// Asks the video side to present one more frame while emulation is paused, so the menu can
// animate and react. Dolphin otherwise only presents when the game produces a frame.
void RequestPausedRedraw(Core::System& system)
{
  const auto now = Clock::now();
  // ~60 Hz is plenty for a menu.
  if (now - s_last_redraw_request < std::chrono::milliseconds(16))
    return;
  s_last_redraw_request = now;
  // A request can be lost if the core shuts down between queueing and running; don't let that
  // wedge future redraws.
  if (s_redraw_queued.load() && now - s_redraw_queued_at < std::chrono::milliseconds(250))
    return;

  s_redraw_queued = true;
  s_redraw_queued_at = now;
  const bool queued = system.GetCPU().QueueJobIfPaused([] {
    // CPU thread. In dual-core mode this hands off to the GPU thread; in single-core mode the
    // queue is a passthrough and the redraw happens right here.
    AsyncRequests::GetInstance()->PushEvent([] {
      s_redraw_queued = false;
      if (g_presenter)
        g_presenter->Present();
    });
  });
  if (!queued)
    s_redraw_queued = false;
}

std::string Lower(std::string s)
{
  std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Strips disc markers and dump tags so "Tales of Symphonia (USA) (Disc 2).rvz" and
// "Tales of Symphonia (USA) (Disc 1).rvz" compare equal.
std::string DiscSetKey(const std::filesystem::path& path)
{
  const std::string stem = Lower(PathToString(path.stem()));
  std::string out;
  for (size_t i = 0; i < stem.size(); ++i)
  {
    const char c = stem[i];
    if (c == '(' || c == '[')
    {
      // Drop only groups that mention a disc; keep region tags so different releases stay apart.
      const size_t end = stem.find(c == '(' ? ')' : ']', i);
      const std::string_view group =
          std::string_view(stem).substr(i, end == std::string::npos ? std::string::npos : end - i);
      if (group.find("disc") != std::string_view::npos ||
          group.find("disk") != std::string_view::npos)
      {
        if (end == std::string::npos)
          break;
        i = end;
        continue;
      }
    }
    if (!std::isspace(static_cast<unsigned char>(c)))
      out.push_back(c);
  }
  return out;
}

void RefreshDiscCandidatesOnHost(Core::System& system)
{
  const auto& dvd = system.GetDVDInterface();
  std::vector<std::string> paths = dvd.GetAutoDiscChangePaths();
  const std::string current = dvd.GetCurrentDiscPath();

  if (paths.empty() && !current.empty())
  {
    // No .m3u set; look next to the current disc for other discs of the same game.
    static constexpr std::array exts = {".iso", ".gcm", ".gcz", ".rvz", ".wia", ".ciso", ".wbfs",
                                        ".nfs"};
    const std::filesystem::path cur = StringToPath(current);
    const std::string key = DiscSetKey(cur);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cur.parent_path(), ec))
    {
      if (!entry.is_regular_file(ec))
        continue;
      const std::string ext = Lower(PathToString(entry.path().extension()));
      if (std::ranges::find(exts, ext) == exts.end())
        continue;
      if (DiscSetKey(entry.path()) == key)
        paths.push_back(PathToString(entry.path()));
    }
    std::ranges::sort(paths);
  }

  std::lock_guard lk(s_mutex);
  s_disc_paths = std::move(paths);
  s_current_disc = current;
  s_disc_scanned = true;
}

void RefreshSlotsOnHost()
{
  std::array<std::string, NUM_SLOTS> labels;
  std::array<bool, NUM_SLOTS> used{};
  for (u32 i = 0; i < NUM_SLOTS; ++i)
  {
    used[i] = State::GetUnixTimeOfSlot(i + 1) != 0;
    labels[i] = used[i] ? State::GetInfoStringOfSlot(i + 1, true) : "Empty";
  }
  std::lock_guard lk(s_mutex);
  s_slot_labels = std::move(labels);
  s_slot_used = used;
}

void RefreshSettingsListsOnHost()
{
  std::vector<std::string> shaders = VideoCommon::PostProcessing::GetShaderList();

  // GameCube controller profiles, named the way a game INI's PadProfile1 key refers to them.
  std::vector<std::string> profiles;
  std::error_code ec;
  const std::filesystem::path dir =
      StringToPath(File::GetUserPath(D_CONFIG_IDX) + "Profiles/GCPad");
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
  {
    if (entry.is_regular_file(ec) && Lower(PathToString(entry.path().extension())) == ".ini")
      profiles.push_back(PathToString(entry.path().stem()));
  }
  std::ranges::sort(profiles);

  std::lock_guard lk(s_mutex);
  s_shader_names = std::move(shaders);
  s_profile_names = std::move(profiles);
}

// UI thread.
void RefreshOverrides()
{
  std::set<Config::Location> keys;
  if (const auto layer = Config::GetLayer(Config::LayerType::LocalGame))
  {
    for (const auto& [location, value] : layer->GetLayerMap())
    {
      if (value)
        keys.insert(location);
    }
  }
  std::lock_guard lk(s_mutex);
  s_overridden = std::move(keys);
}

// ---------------------------------------------------------------------------------------------
// Controller reading (host thread)
// ---------------------------------------------------------------------------------------------

struct NavButtons
{
  bool up = false, down = false, left = false, right = false;
  bool accept = false, back = false, start = false, promote = false, revert = false;
};

NavButtons ReadNavButtons()
{
  NavButtons b;

  // Prefer the raw Xbox pad so A/B match the labels on the controller regardless of how the
  // GameCube buttons are mapped.
  ciface::Core::DeviceQualifier xinput;
  xinput.FromString("XInput/0/Gamepad");
  if (const auto dev = g_controller_interface.FindDevice(xinput))
  {
    const auto get = [&dev](std::string_view name) {
      const auto* input = dev->FindInput(name);
      return input ? input->GetState() : 0.0;
    };
    constexpr double stick = 0.55;
    b.up = get("Pad N") > 0.5 || get("Left Y+") > stick;
    b.down = get("Pad S") > 0.5 || get("Left Y-") > stick;
    b.left = get("Pad W") > 0.5 || get("Left X-") > stick;
    b.right = get("Pad E") > 0.5 || get("Left X+") > stick;
    b.accept = get("Button A") > 0.5;
    b.back = get("Button B") > 0.5;
    b.start = get("Start") > 0.5;
    b.promote = get("Button X") > 0.5;
    b.revert = get("Button Y") > 0.5;
    return b;
  }

  // Otherwise use whatever is mapped to GameCube port 1.
  const GCPadStatus pad = Pad::GetStatus(0);
  const int sx = int(pad.stickX) - GCPadStatus::MAIN_STICK_CENTER_X;
  const int sy = int(pad.stickY) - GCPadStatus::MAIN_STICK_CENTER_Y;
  constexpr int stick = GCPadStatus::MAIN_STICK_RADIUS / 2;
  b.up = (pad.button & PAD_BUTTON_UP) || sy > stick;
  b.down = (pad.button & PAD_BUTTON_DOWN) || sy < -stick;
  b.left = (pad.button & PAD_BUTTON_LEFT) || sx < -stick;
  b.right = (pad.button & PAD_BUTTON_RIGHT) || sx > stick;
  b.accept = pad.button & PAD_BUTTON_A;
  b.back = pad.button & PAD_BUTTON_B;
  b.start = pad.button & PAD_BUTTON_START;
  b.promote = pad.button & PAD_BUTTON_X;
  b.revert = pad.button & PAD_BUTTON_Y;
  return b;
}

bool AnyHeld(const NavButtons& b)
{
  return b.up || b.down || b.left || b.right || b.accept || b.back || b.start || b.promote ||
         b.revert;
}

// ---------------------------------------------------------------------------------------------
// Drawing (video thread)
// ---------------------------------------------------------------------------------------------

enum class Icon
{
  None,
  Resume,
  Screenshot,
  SaveState,
  LoadState,
  Disc,
  Settings,
  Restart,
  Exit,
  Undo,
};

struct Item
{
  std::string id;  // stable key for confirmations
  std::string label;
  Icon icon = Icon::None;
  std::string value;  // right-aligned text
  bool submenu = false;
  bool choice = false;  // draws < value > and accepts left/right
  bool enabled = true;
  bool dim_value = false;
  std::string confirm;  // non-empty: first press arms, second press runs
  std::function<void()> on_accept;
  std::function<void(int)> on_adjust;
  std::vector<Config::Location> settings;  // config keys a settings row writes
  bool overridden = false;                 // this game's INI sets one of them
  bool promotable = false;                 // ...and the value can become the system default
  bool next_launch = false;                // only read when a game boots
};

struct Palette
{
  ImU32 dim;            // full-screen veil over the game
  ImU32 panel;          // list background
  ImU32 row_hover;      // mouse hover
  ImU32 row_selected;   // selected row
  ImU32 accent;         // selection bar, choice arrows, hovered header icons
  ImU32 text;
  ImU32 text_dim;
  ImU32 text_disabled;
  ImU32 warn;           // "press A again" confirmations
  ImU32 chrome;         // header/footer band; fully transparent keeps the xemu look
  ImU32 chrome_text;    // header icons
  ImU32 chrome_dim;     // header title and footer hints
};

struct Theme
{
  const char* name;
  Palette palette;
};

// clang-format off
constexpr std::array THEMES = {
  Theme{"Blue", {IM_COL32(0, 0, 0, 150), IM_COL32(24, 24, 26, 245), IM_COL32(255, 255, 255, 18),
                 IM_COL32(255, 255, 255, 30), IM_COL32(64, 156, 255, 255), IM_COL32(236, 236, 240, 255),
                 IM_COL32(150, 150, 158, 255), IM_COL32(100, 100, 106, 255), IM_COL32(255, 176, 64, 255),
                 IM_COL32(0, 0, 0, 0), IM_COL32(236, 236, 240, 255), IM_COL32(170, 170, 178, 255)}},
  Theme{"Dark", {IM_COL32(0, 0, 0, 185), IM_COL32(10, 10, 10, 250), IM_COL32(255, 255, 255, 14),
                 IM_COL32(255, 255, 255, 26), IM_COL32(235, 235, 235, 255), IM_COL32(225, 225, 225, 255),
                 IM_COL32(135, 135, 135, 255), IM_COL32(85, 85, 85, 255), IM_COL32(255, 176, 64, 255),
                 IM_COL32(0, 0, 0, 0), IM_COL32(225, 225, 225, 255), IM_COL32(160, 160, 160, 255)}},
  Theme{"Purple", {IM_COL32(8, 0, 20, 150), IM_COL32(30, 24, 42, 245), IM_COL32(200, 170, 255, 18),
                   IM_COL32(190, 150, 255, 38), IM_COL32(170, 120, 255, 255), IM_COL32(240, 236, 250, 255),
                   IM_COL32(160, 150, 182, 255), IM_COL32(105, 98, 122, 255), IM_COL32(255, 176, 64, 255),
                   IM_COL32(0, 0, 0, 0), IM_COL32(240, 236, 250, 255), IM_COL32(182, 172, 204, 255)}},
  Theme{"Green", {IM_COL32(0, 10, 0, 150), IM_COL32(22, 28, 22, 245), IM_COL32(160, 255, 120, 16),
                  IM_COL32(98, 202, 19, 40), IM_COL32(98, 202, 19, 255), IM_COL32(234, 242, 232, 255),
                  IM_COL32(148, 162, 146, 255), IM_COL32(98, 110, 96, 255), IM_COL32(255, 176, 64, 255),
                  IM_COL32(0, 0, 0, 0), IM_COL32(234, 242, 232, 255), IM_COL32(170, 184, 168, 255)}},
  Theme{"Red", {IM_COL32(16, 0, 0, 150), IM_COL32(34, 22, 24, 245), IM_COL32(255, 160, 160, 16),
                IM_COL32(240, 80, 80, 38), IM_COL32(240, 76, 76, 255), IM_COL32(244, 236, 236, 255),
                IM_COL32(170, 150, 152, 255), IM_COL32(118, 100, 102, 255), IM_COL32(255, 196, 80, 255),
                IM_COL32(0, 0, 0, 0), IM_COL32(244, 236, 236, 255), IM_COL32(190, 172, 174, 255)}},
  Theme{"Gold", {IM_COL32(12, 8, 0, 150), IM_COL32(32, 28, 20, 245), IM_COL32(255, 220, 140, 16),
                 IM_COL32(255, 186, 60, 36), IM_COL32(255, 184, 48, 255), IM_COL32(246, 240, 228, 255),
                 IM_COL32(170, 160, 140, 255), IM_COL32(116, 108, 92, 255), IM_COL32(255, 120, 90, 255),
                 IM_COL32(0, 0, 0, 0), IM_COL32(246, 240, 228, 255), IM_COL32(190, 180, 160, 255)}},
  Theme{"Light", {IM_COL32(0, 0, 0, 110), IM_COL32(244, 244, 247, 250), IM_COL32(0, 0, 0, 12),
                  IM_COL32(0, 90, 200, 30), IM_COL32(0, 110, 215, 255), IM_COL32(24, 24, 28, 255),
                  IM_COL32(96, 96, 106, 255), IM_COL32(165, 165, 172, 255), IM_COL32(200, 100, 0, 255),
                  IM_COL32(228, 228, 234, 250), IM_COL32(24, 24, 28, 255), IM_COL32(96, 96, 106, 255)}},
};
// clang-format on

size_t ThemeIndex(const std::string& name)
{
  for (size_t i = 0; i < THEMES.size(); ++i)
  {
    if (name == THEMES[i].name)
      return i;
  }
  return 0;
}

// Video thread only; refreshed from the config at the start of every Draw().
Palette COLORS = THEMES[0].palette;

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float s, ImU32 col)
{
  const float t = std::max(1.5f, s * 0.09f);  // stroke
  const float r = s * 0.5f;
  switch (icon)
  {
  case Icon::None:
    break;
  case Icon::Resume:
    dl->AddTriangleFilled({c.x - r * 0.55f, c.y - r * 0.8f}, {c.x - r * 0.55f, c.y + r * 0.8f},
                          {c.x + r * 0.85f, c.y}, col);
    break;
  case Icon::Screenshot:
    dl->AddRectFilled({c.x - r, c.y - r * 0.55f}, {c.x + r, c.y + r * 0.75f}, col, r * 0.2f);
    dl->AddRectFilled({c.x - r * 0.35f, c.y - r * 0.8f}, {c.x + r * 0.35f, c.y - r * 0.4f}, col,
                      r * 0.1f);
    dl->AddCircleFilled({c.x, c.y + r * 0.1f}, r * 0.42f, COLORS.panel);
    dl->AddCircleFilled({c.x, c.y + r * 0.1f}, r * 0.24f, col);
    break;
  case Icon::SaveState:
  case Icon::LoadState:
  {
    // Tray with an arrow going in (save) or out (load).
    dl->AddLine({c.x - r, c.y + r * 0.25f}, {c.x - r, c.y + r * 0.85f}, col, t);
    dl->AddLine({c.x - r, c.y + r * 0.85f}, {c.x + r, c.y + r * 0.85f}, col, t);
    dl->AddLine({c.x + r, c.y + r * 0.85f}, {c.x + r, c.y + r * 0.25f}, col, t);
    const bool down = icon == Icon::SaveState;
    const float shaft_top = down ? c.y - r * 0.95f : c.y - r * 0.35f;
    const float shaft_bot = down ? c.y + r * 0.2f : c.y + r * 0.5f;
    dl->AddLine({c.x, shaft_top}, {c.x, shaft_bot}, col, t);
    const float tip = down ? c.y + r * 0.45f : c.y - r * 0.95f;
    const float base = down ? c.y + r * 0.0f : c.y - r * 0.5f;
    dl->AddTriangleFilled({c.x - r * 0.45f, base}, {c.x + r * 0.45f, base}, {c.x, tip}, col);
    break;
  }
  case Icon::Disc:
    dl->AddCircleFilled(c, r, col, 32);
    dl->AddCircleFilled(c, r * 0.3f, COLORS.panel, 24);
    dl->AddCircle(c, r * 0.62f, COLORS.panel, 32, t * 0.6f);
    break;
  case Icon::Settings:
  {
    for (int i = 0; i < 8; ++i)
    {
      const float a = i * 3.14159265f / 4.0f;
      const ImVec2 dir{std::cos(a), std::sin(a)};
      dl->AddLine({c.x + dir.x * r * 0.5f, c.y + dir.y * r * 0.5f},
                  {c.x + dir.x * r * 0.98f, c.y + dir.y * r * 0.98f}, col, t * 1.6f);
    }
    dl->AddCircleFilled(c, r * 0.68f, col, 24);
    dl->AddCircleFilled(c, r * 0.28f, COLORS.panel, 16);
    break;
  }
  case Icon::Restart:
  case Icon::Undo:
  {
    const float a0 = icon == Icon::Restart ? -2.4f : 0.75f;
    const float a1 = icon == Icon::Restart ? 2.0f : 5.15f;
    dl->PathArcTo(c, r * 0.75f, a0, a1, 24);
    dl->PathStroke(col, 0, t);
    const float ae = icon == Icon::Restart ? a1 : a0;
    const ImVec2 end{c.x + std::cos(ae) * r * 0.75f, c.y + std::sin(ae) * r * 0.75f};
    const ImVec2 tan = icon == Icon::Restart ? ImVec2{-std::sin(ae), std::cos(ae)} :
                                               ImVec2{std::sin(ae), -std::cos(ae)};
    const ImVec2 nrm{std::cos(ae), std::sin(ae)};
    dl->AddTriangleFilled(
        {end.x + nrm.x * r * 0.32f, end.y + nrm.y * r * 0.32f},
        {end.x - nrm.x * r * 0.32f, end.y - nrm.y * r * 0.32f},
        {end.x + tan.x * r * 0.42f, end.y + tan.y * r * 0.42f}, col);
    break;
  }
  case Icon::Exit:
    dl->PathArcTo(c, r * 0.78f, -3.14159265f * 0.5f + 0.7f, 3.14159265f * 1.5f - 0.7f, 28);
    dl->PathStroke(col, 0, t);
    dl->AddLine({c.x, c.y - r * 0.95f}, {c.x, c.y - r * 0.1f}, col, t);
    break;
  }
}

void DrawChevron(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool right, float thickness)
{
  const float dx = right ? s * 0.25f : -s * 0.25f;
  dl->AddLine({c.x - dx, c.y - s * 0.45f}, {c.x + dx, c.y}, col, thickness);
  dl->AddLine({c.x + dx, c.y}, {c.x - dx, c.y + s * 0.45f}, col, thickness);
}

void DrawChevronV(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool down, float thickness)
{
  const float dy = down ? s * 0.25f : -s * 0.25f;
  dl->AddLine({c.x - s * 0.45f, c.y - dy}, {c.x, c.y + dy}, col, thickness);
  dl->AddLine({c.x, c.y + dy}, {c.x + s * 0.45f, c.y - dy}, col, thickness);
}

void DrawCross(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float thickness)
{
  const float h = s * 0.4f;
  dl->AddLine({c.x - h, c.y - h}, {c.x + h, c.y + h}, col, thickness);
  dl->AddLine({c.x - h, c.y + h}, {c.x + h, c.y - h}, col, thickness);
}

void DrawBackArrow(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float thickness)
{
  const float h = s * 0.45f;
  dl->AddLine({c.x - h, c.y}, {c.x + h, c.y}, col, thickness);
  dl->AddLine({c.x - h, c.y}, {c.x - h * 0.2f, c.y - h * 0.75f}, col, thickness);
  dl->AddLine({c.x - h, c.y}, {c.x - h * 0.2f, c.y + h * 0.75f}, col, thickness);
}

// Draws a controller face button glyph (filled circle with a letter) and returns its width.
float DrawButtonGlyph(ImDrawList* dl, ImFont* font, ImVec2 pos, float size, const char* letter,
                      ImU32 col)
{
  const ImVec2 c{pos.x + size * 0.5f, pos.y + size * 0.5f};
  dl->AddCircleFilled(c, size * 0.5f, col, 24);
  const float fs = size * 0.72f;
  const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, letter);
  dl->AddText(font, fs, {c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, COLORS.panel, letter);
  return size;
}

std::string OnOff(bool v)
{
  return v ? "On" : "Off";
}

int Wrap(int v, int n)
{
  return ((v % n) + n) % n;
}

// ----- Settings helpers -----
//
// Every change made from the menu is a per-game override. It goes to Dolphin's LocalGame config
// layer, which is this game's User/GameSettings/<GameID>.ini (the file the game's Properties
// dialog edits). "Make This the Default" promotes the overrides to the global config.
//
// Config is owned by the UI thread, so all writes happen there. The paused CPU thread picks a
// change up the next time we wake it for a redraw, so graphics settings preview live behind the
// menu.

using ApplyFn = std::function<void(Config::LayerType layer, Core::System& system)>;

void ChangeGameSettings(ApplyFn apply)
{
  Core::QueueHostJob(
      [apply = std::move(apply)](Core::System& system) {
        // Without a game layer (no game running) the change has nowhere else to go.
        const Config::LayerType layer = Config::GetLayer(Config::LayerType::LocalGame) ?
                                            Config::LayerType::LocalGame :
                                            Config::LayerType::Base;
        apply(layer, system);
        RefreshOverrides();
      },
      false);
  s_config_dirty = true;
}

// UI thread.
template <typename T>
void SetValue(Config::LayerType layer, const Config::Info<T>& info, const T& value)
{
  // A value set for this run only (Dolphin's settings windows do that when a game INI already
  // sets the key) sits above the game layer and would hide the new one.
  if (const auto run = Config::GetLayer(Config::LayerType::CurrentRun);
      run && run->DeleteKey(info.GetLocation()))
  {
    Config::OnConfigChanged();
  }
  Config::Set(layer, info, value);
}

// Removes this game's overrides so the global values apply again.
void ClearGameSettings(std::vector<Config::Location> locations)
{
  ChangeGameSettings(
      [locations = std::move(locations)](Config::LayerType layer, Core::System& system) {
        if (layer != Config::LayerType::LocalGame)
          return;
        const auto local = Config::GetLayer(layer);
        bool changed = false;
        for (const Config::Location& location : locations)
          changed |= local->DeleteKey(location);
        if (!changed)
          return;
        Config::OnConfigChanged();
        AudioCommon::UpdateSoundStream(system);
      });
}

// Copies this game's overrides into the global config and drops them from the game INI.
void MakeGameSettingsDefault(std::vector<Config::Location> locations)
{
  ChangeGameSettings([locations = std::move(locations)](Config::LayerType layer, Core::System&) {
    if (layer != Config::LayerType::LocalGame)
      return;
    const auto local = Config::GetLayer(Config::LayerType::LocalGame);
    const auto base = Config::GetLayer(Config::LayerType::Base);
    const auto builtin = Config::GetLayer(Config::LayerType::GlobalGame);
    for (const Config::Location& location : locations)
    {
      // Controller profiles only exist per game.
      if (location.system == Config::System::GameSettingsOnly)
        continue;
      const std::optional<std::string> value = local->Get<std::string>(location);
      if (!value)
        continue;
      base->Set(location, *value);
      // Dolphin's built-in settings for this game sit between the global config and the game
      // INI. Keep the override when they set the key, so this game still uses the chosen value.
      if (!builtin || !builtin->Exists(location))
        local->DeleteKey(location);
    }
    Config::OnConfigChanged();
  });
}

// The Couch Potato baseline: the global config every game starts from. Dolphin's built-in
// per-game settings (Sys/GameSettings) still apply on top, so games that need slower but more
// accurate options keep them. UI thread.
void ApplyBaselineValues()
{
  Config::ConfigChangeCallbackGuard guard;
  // Graphics
  Config::SetBase(Config::GFX_EFB_SCALE, 3);
  Config::SetBase(Config::GFX_ASPECT_RATIO, AspectMode::Auto);
  Config::SetBase(Config::GFX_VSYNC, true);
  Config::SetBase(Config::GFX_MSAA, 4u);
  Config::SetBase(Config::GFX_SSAA, false);
  Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY, AnisotropicFilteringMode::Force16x);
  Config::SetBase(Config::GFX_ENABLE_PIXEL_LIGHTING, true);
  Config::SetBase(Config::GFX_ENHANCE_FORCE_TRUE_COLOR, true);
  Config::SetBase(Config::GFX_HACK_COPY_EFB_SCALED, true);
  Config::SetBase(Config::GFX_WIDESCREEN_HACK, false);
  // No shader stutter.
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  ShaderCompilationMode::AsynchronousUberShaders);
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);
  Config::SetBase(Config::GFX_HIRES_TEXTURES, true);
  // Emulation
  Config::SetBase(Config::MAIN_CPU_THREAD, true);
  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::DefaultCPUCore());
  Config::SetBase(Config::MAIN_DSP_HLE, true);
  Config::SetBase(Config::MAIN_SKIP_IPL, true);
  Config::SetBase(Config::MAIN_ENABLE_CHEATS, true);
  Config::SetBase(Config::MAIN_EMULATION_SPEED, 1.0f);
  // Couch: no mouse, no desktop questions.
  Config::SetBase(Config::MAIN_FULLSCREEN, true);
  Config::SetBase(Config::MAIN_SHOW_CURSOR, Config::ShowCursor::Never);
  Config::SetBase(Config::MAIN_CONFIRM_ON_STOP, false);
  Config::SetBase(Config::MAIN_PAUSE_ON_FOCUS_LOST, false);

  Config::SetBase(COUCH_BASELINE_VERSION, BASELINE_VERSION);
}

// For menu-wide preferences such as the theme, which are not per game.
template <typename T>
void SetGlobalSetting(const Config::Info<T>& info, std::type_identity_t<T> value)
{
  Core::QueueHostJob([&info, value](Core::System&) { Config::SetBase(info, value); }, false);
  s_config_dirty = true;
}

Item SettingRow(std::string id, std::string label, std::vector<Config::Location> locations,
                bool next_launch)
{
  Item item;
  item.id = std::move(id);
  item.label = std::move(label);
  item.choice = true;
  item.next_launch = next_launch;
  item.overridden = std::ranges::any_of(
      locations, [](const Config::Location& l) { return s_overridden.contains(l); });
  // Controller profiles only exist per game.
  item.promotable = std::ranges::any_of(locations, [](const Config::Location& l) {
    return s_overridden.contains(l) && l.system != Config::System::GameSettingsOnly;
  });
  item.settings = std::move(locations);
  return item;
}

// A row that shows invert ? !value : value as On/Off.
Item BoolRow(std::string id, std::string label, const Config::Info<bool>& info,
             bool next_launch = false, bool invert = false)
{
  const bool on = Config::Get(info) != invert;
  Item item = SettingRow(std::move(id), std::move(label), {info.GetLocation()}, next_launch);
  item.value = OnOff(on);
  item.on_adjust = [&info, value = on == invert](int) {
    ChangeGameSettings(
        [&info, value](Config::LayerType layer, Core::System&) { SetValue(layer, info, value); });
  };
  return item;
}

template <typename T>
using Choices = std::vector<std::pair<T, std::string>>;

template <typename T>
int FindChoice(const Choices<T>& choices, const T& value)
{
  for (size_t i = 0; i < choices.size(); ++i)
  {
    if constexpr (std::is_floating_point_v<T>)
    {
      if (std::abs(choices[i].first - value) < 0.001f)
        return int(i);
    }
    else if (choices[i].first == value)
    {
      return int(i);
    }
  }
  return -1;
}

int NextChoice(int idx, int dir, size_t n)
{
  return idx < 0 ? 0 : Wrap(idx + dir, int(n));
}

template <typename T>
Item ChoiceRow(std::string id, std::string label, const Config::Info<T>& info, Choices<T> choices,
               bool next_launch = false)
{
  const int idx = FindChoice(choices, Config::Get(info));
  Item item = SettingRow(std::move(id), std::move(label), {info.GetLocation()}, next_launch);
  item.value = idx < 0 ? "Custom" : choices[idx].second;
  item.on_adjust = [&info, choices = std::move(choices), idx](int dir) {
    const T value = choices[NextChoice(idx, dir, choices.size())].first;
    ChangeGameSettings(
        [&info, value](Config::LayerType layer, Core::System&) { SetValue(layer, info, value); });
  };
  return item;
}

std::string Percent(float v)
{
  return fmt::format("{:.0f}%", v * 100.0f);
}

// CPU clock and VBI frequency overrides: an enable flag plus a factor, shown as one row.
Item OverrideRow(std::string id, std::string label, const Config::Info<bool>& enable,
                 const Config::Info<float>& factor, std::vector<float> factors)
{
  Choices<float> choices;
  for (float f : factors)
    choices.emplace_back(f, Percent(f));
  const bool on = Config::Get(enable);
  const float cur = Config::Get(factor);
  // Index 0 is Off; factors follow.
  const int found = FindChoice(choices, cur);
  const int idx = on ? (found < 0 ? -1 : found + 1) : 0;
  Item item = SettingRow(std::move(id), std::move(label),
                         {enable.GetLocation(), factor.GetLocation()}, false);
  item.value = on ? Percent(cur) : "Off";
  item.on_adjust = [&enable, &factor, factors = std::move(factors), idx](int dir) {
    const int next = NextChoice(idx, dir, factors.size() + 1);
    ChangeGameSettings([&enable, &factor, next, value = next > 0 ? factors[next - 1] : 1.0f](
                           Config::LayerType layer, Core::System&) {
      SetValue(layer, enable, next > 0);
      if (next > 0)
        SetValue(layer, factor, value);
    });
  };
  return item;
}

std::function<void()> PushPage(std::vector<PageState>& pages, PageId id)
{
  return [&pages, id] { pages.push_back({id, 0, 0}); };
}

void BuildMain(std::vector<Item>& items, std::vector<PageState>& pages)
{
  items.push_back({.id = "resume",
                   .label = "Resume",
                   .icon = Icon::Resume,
                   .on_accept = [] { CloseLocked(true); }});
  items.push_back({.id = "screenshot",
                   .label = "Screenshot",
                   .icon = Icon::Screenshot,
                   .on_accept = [] {
                     // The capture is of the next game frame, so resume to take a clean one.
                     PushHostActionLocked(HostAction::Screenshot);
                     CloseLocked(true);
                   }});
  items.push_back({.id = "save",
                   .label = "Save State",
                   .icon = Icon::SaveState,
                   .submenu = true,
                   .on_accept = [&pages] {
                     s_slot_refresh_requested = true;
                     pages.push_back({PageId::SaveState, 0, 0});
                   }});
  items.push_back({.id = "load",
                   .label = "Load State",
                   .icon = Icon::LoadState,
                   .submenu = true,
                   .on_accept = [&pages] {
                     s_slot_refresh_requested = true;
                     pages.push_back({PageId::LoadState, 0, 0});
                   }});
  items.push_back({.id = "disc",
                   .label = "Change Disc",
                   .icon = Icon::Disc,
                   .submenu = true,
                   .on_accept = [&pages] {
                     s_disc_refresh_requested = true;
                     s_disc_scanned = false;
                     pages.push_back({PageId::ChangeDisc, 0, 0});
                   }});
  items.push_back({.id = "settings",
                   .label = "Settings",
                   .icon = Icon::Settings,
                   .submenu = true,
                   .on_accept = [&pages] {
                     s_lists_refresh_requested = true;
                     Core::QueueHostJob([](Core::System&) { RefreshOverrides(); }, false);
                     pages.push_back({PageId::Settings, 0, 0});
                   }});
  items.push_back({.id = "restart",
                   .label = "Restart",
                   .icon = Icon::Restart,
                   .confirm = "Press A again to restart",
                   .on_accept = [] {
                     PushHostActionLocked(HostAction::Reset);
                     CloseLocked(true);
                   }});
  items.push_back({.id = "exit",
                   .label = "Exit",
                   .icon = Icon::Exit,
                   .confirm = "Press A again to exit",
                   .on_accept = [] {
                     PushHostActionLocked(HostAction::Exit);
                     CloseLocked(false);
                   }});
}

void BuildSlots(std::vector<Item>& items, bool save)
{
  for (u32 i = 0; i < NUM_SLOTS; ++i)
  {
    const int slot = int(i) + 1;
    Item item;
    item.id = fmt::format("{}{}", save ? "save" : "load", slot);
    item.label = fmt::format("Slot {}", slot);
    item.value = s_slot_labels[i].empty() ? "..." : s_slot_labels[i];
    item.dim_value = !s_slot_used[i];
    if (save)
    {
      if (s_slot_used[i])
        item.confirm = fmt::format("Slot {}: press A again to overwrite", slot);
      item.on_accept = [slot] {
        PushHostActionLocked(HostAction::StateSave, slot);
        // Re-read the slot list once the save has had time to land on disk.
        s_slot_refresh_at = Clock::now() + std::chrono::milliseconds(700);
      };
    }
    else
    {
      item.enabled = s_slot_used[i];
      item.on_accept = [slot] {
        PushHostActionLocked(HostAction::StateLoad, slot);
        CloseLocked(true);
      };
    }
    items.push_back(std::move(item));
  }
  if (!save)
  {
    items.push_back({.id = "undoload",
                     .label = "Undo Load State",
                     .icon = Icon::Undo,
                     .on_accept = [] {
                       PushHostActionLocked(HostAction::StateUndoLoad);
                       CloseLocked(true);
                     }});
  }
}

void BuildDiscs(std::vector<Item>& items)
{
  if (s_disc_paths.size() <= 1)
  {
    items.push_back({.id = "nodisc",
                     .label = s_disc_scanned ? "No other discs for this game" :
                                               "Looking for discs...",
                     .enabled = false});
    return;
  }
  int n = 0;
  for (const std::string& path : s_disc_paths)
  {
    const bool current = path == s_current_disc;
    Item item;
    item.id = fmt::format("disc{}", n++);
    item.label = PathToString(StringToPath(path).stem());
    item.icon = Icon::Disc;
    item.value = current ? "Current" : "";
    item.dim_value = true;
    item.enabled = !current;
    item.on_accept = [path] {
      Core::QueueHostJob(
          [path](Core::System& system) {
            system.GetDVDInterface().ChangeDisc(Core::CPUThreadGuard{system}, path);
          },
          false);
      // The new disc is inserted after a second of emulated time, so keep the game running.
      CloseLocked(true);
    };
    items.push_back(std::move(item));
  }
}

// Read by InputConfig when a game boots; a game INI can name a GameCube controller profile.
const Config::Info<std::string> GAME_PAD_PROFILE_1{
    {Config::System::GameSettingsOnly, "Controls", "PadProfile1"}, ""};

void BuildGraphics(std::vector<Item>& items)
{
  items.push_back(ChoiceRow<int>("efb", "Internal Resolution", Config::GFX_EFB_SCALE,
                                 {{0, "Auto (Window)"},
                                  {1, "Native (480p)"},
                                  {2, "2x (720p)"},
                                  {3, "3x (1080p)"},
                                  {4, "4x (1440p)"},
                                  {5, "5x (1620p)"},
                                  {6, "6x (4K)"},
                                  {7, "7x (3360p)"},
                                  {8, "8x (5K)"}}));
  items.push_back(ChoiceRow<AspectMode>("aspect", "Aspect Ratio", Config::GFX_ASPECT_RATIO,
                                        {{AspectMode::Auto, "Auto"},
                                         {AspectMode::ForceWide, "16:9"},
                                         {AspectMode::ForceStandard, "4:3"},
                                         {AspectMode::Stretch, "Stretch"}}));
  items.push_back(BoolRow("wshack", "Widescreen Hack", Config::GFX_WIDESCREEN_HACK));
  items.push_back(BoolRow("crop", "Crop to Aspect Ratio", Config::GFX_CROP_TO_ASPECT_RATIO));
  items.push_back(BoolRow("vsync", "V-Sync", Config::GFX_VSYNC));
  items.push_back(
      BoolRow("borderless", "Borderless Fullscreen", Config::GFX_BORDERLESS_FULLSCREEN));
}

void BuildEnhancements(std::vector<Item>& items)
{
  // Anti-aliasing: MSAA sample count plus whether those samples are SSAA, shown as one row.
  {
    static const std::array<std::tuple<u32, bool, const char*>, 7> modes = {{{1, false, "Off"},
                                                                             {2, false, "2x MSAA"},
                                                                             {4, false, "4x MSAA"},
                                                                             {8, false, "8x MSAA"},
                                                                             {2, true, "2x SSAA"},
                                                                             {4, true, "4x SSAA"},
                                                                             {8, true, "8x SSAA"}}};
    const u32 samples = Config::Get(Config::GFX_MSAA);
    const bool ssaa = Config::Get(Config::GFX_SSAA);
    int idx = -1;
    for (size_t i = 0; i < modes.size(); ++i)
    {
      const u32 mode_samples = std::get<0>(modes[i]);
      if ((samples <= 1 && mode_samples == 1) ||
          (samples == mode_samples && ssaa == std::get<1>(modes[i])))
      {
        idx = int(i);
        break;
      }
    }
    Item item = SettingRow("aa", "Anti-Aliasing",
                           {Config::GFX_MSAA.GetLocation(), Config::GFX_SSAA.GetLocation()}, false);
    item.value = idx < 0 ? "Custom" : std::get<2>(modes[idx]);
    item.on_adjust = [idx](int dir) {
      const auto& mode = modes[NextChoice(idx, dir, modes.size())];
      ChangeGameSettings([samples = std::get<0>(mode),
                          ssaa = std::get<1>(mode)](Config::LayerType layer, Core::System&) {
        SetValue(layer, Config::GFX_MSAA, samples);
        SetValue(layer, Config::GFX_SSAA, ssaa);
      });
    };
    items.push_back(std::move(item));
  }
  items.push_back(ChoiceRow<AnisotropicFilteringMode>(
      "aniso", "Anisotropic Filtering", Config::GFX_ENHANCE_MAX_ANISOTROPY,
      {{AnisotropicFilteringMode::Default, "Default"},
       {AnisotropicFilteringMode::Force1x, "1x"},
       {AnisotropicFilteringMode::Force2x, "2x"},
       {AnisotropicFilteringMode::Force4x, "4x"},
       {AnisotropicFilteringMode::Force8x, "8x"},
       {AnisotropicFilteringMode::Force16x, "16x"}}));
  items.push_back(ChoiceRow<TextureFilteringMode>(
      "texfilter", "Texture Filtering", Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
      {{TextureFilteringMode::Default, "Default"},
       {TextureFilteringMode::Nearest, "Force Nearest"},
       {TextureFilteringMode::Linear, "Force Linear"}}));
  items.push_back(ChoiceRow<OutputResamplingMode>(
      "resampling", "Output Resampling", Config::GFX_ENHANCE_OUTPUT_RESAMPLING,
      {{OutputResamplingMode::Default, "Default"},
       {OutputResamplingMode::Bilinear, "Bilinear"},
       {OutputResamplingMode::BSpline, "Bicubic: B-Spline"},
       {OutputResamplingMode::MitchellNetravali, "Bicubic: Mitchell"},
       {OutputResamplingMode::CatmullRom, "Bicubic: Catmull-Rom"},
       {OutputResamplingMode::SharpBilinear, "Sharp Bilinear"},
       {OutputResamplingMode::AreaSampling, "Area Sampling"}}));
  {
    Choices<std::string> shaders = {{"", "None"}};
    for (const std::string& name : s_shader_names)
      shaders.emplace_back(name, name);
    const std::string cur = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
    if (FindChoice(shaders, cur) < 0)
      shaders.emplace_back(cur, cur);
    items.push_back(ChoiceRow<std::string>("shader", "Post-Processing Shader",
                                           Config::GFX_ENHANCE_POST_SHADER, std::move(shaders)));
  }
  items.push_back(BoolRow("hdr", "HDR Post-Processing", Config::GFX_ENHANCE_HDR_OUTPUT));
  items.push_back(BoolRow("efbscaled", "Scaled EFB Copy", Config::GFX_HACK_COPY_EFB_SCALED));
  items.push_back(BoolRow("pixellight", "Per-Pixel Lighting", Config::GFX_ENABLE_PIXEL_LIGHTING));
  items.push_back(BoolRow("truecolor", "Force 24-Bit Color", Config::GFX_ENHANCE_FORCE_TRUE_COLOR));
  items.push_back(
      BoolRow("copyfilter", "Disable Copy Filter", Config::GFX_ENHANCE_DISABLE_COPY_FILTER));
  items.push_back(BoolRow("mipmap", "Arbitrary Mipmap Detection",
                          Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION));
  items.push_back(BoolRow("fog", "Disable Fog", Config::GFX_DISABLE_FOG));
}

void BuildPerformance(std::vector<Item>& items)
{
  items.push_back(ChoiceRow<ShaderCompilationMode>(
      "shadermode", "Shader Compilation", Config::GFX_SHADER_COMPILATION_MODE,
      {{ShaderCompilationMode::Synchronous, "Specialized"},
       {ShaderCompilationMode::SynchronousUberShaders, "Exclusive Ubershaders"},
       {ShaderCompilationMode::AsynchronousUberShaders, "Hybrid Ubershaders"},
       {ShaderCompilationMode::AsynchronousSkipRendering, "Skip Drawing"}}));
  items.push_back(BoolRow("precompile", "Compile Shaders Before Starting",
                          Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true));
  items.push_back(BoolRow("hires", "Load Custom Textures", Config::GFX_HIRES_TEXTURES));
  items.push_back(
      BoolRow("hirescache", "Prefetch Custom Textures", Config::GFX_CACHE_HIRES_TEXTURES));
  items.push_back(BoolRow("mods", "Graphics Mods", Config::GFX_MODS_ENABLE));
}

void BuildHacks(std::vector<Item>& items)
{
  items.push_back(BoolRow("efbaccess", "Skip EFB Access from CPU",
                          Config::GFX_HACK_EFB_ACCESS_ENABLE, false, true));
  items.push_back(BoolRow("formatchanges", "Ignore Format Changes",
                          Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, false, true));
  items.push_back(
      BoolRow("efbtotex", "EFB Copies to Texture Only", Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM));
  items.push_back(
      BoolRow("defercopies", "Defer EFB Copies to RAM", Config::GFX_HACK_DEFER_EFB_COPIES));
  items.push_back(ChoiceRow<int>("texcache", "Texture Cache Accuracy",
                                 Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES,
                                 {{0, "Safe"}, {512, "Balanced"}, {128, "Fast"}}));
  items.push_back(
      BoolRow("gputex", "GPU Texture Decoding", Config::GFX_ENABLE_GPU_TEXTURE_DECODING));
  items.push_back(
      BoolRow("xfbtotex", "XFB Copies to Texture Only", Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM));
  items.push_back(BoolRow("immxfb", "Immediately Present XFB", Config::GFX_HACK_IMMEDIATE_XFB));
  items.push_back(BoolRow("dupxfb", "Skip Duplicate XFBs", Config::GFX_HACK_SKIP_DUPLICATE_XFBS));
  items.push_back(BoolRow("fastdepth", "Fast Depth Calculation", Config::GFX_FAST_DEPTH_CALC));
  items.push_back(
      BoolRow("bbox", "Disable Bounding Box", Config::GFX_HACK_BBOX_ENABLE, false, true));
  items.push_back(BoolRow("vround", "Vertex Rounding", Config::GFX_HACK_VERTEX_ROUNDING));
  items.push_back(BoolRow("viskip", "VBI Skip", Config::GFX_HACK_VI_SKIP));
}

void BuildOnScreenDisplay(std::vector<Item>& items)
{
  items.push_back(BoolRow("fps", "Show FPS", Config::GFX_SHOW_FPS));
  items.push_back(BoolRow("vps", "Show VPS", Config::GFX_SHOW_VPS));
  items.push_back(BoolRow("speed", "Show Speed", Config::GFX_SHOW_SPEED));
  items.push_back(BoolRow("ftimes", "Show Frame Times", Config::GFX_SHOW_FTIMES));
  items.push_back(BoolRow("graphs", "Show Performance Graphs", Config::GFX_SHOW_GRAPHS));
  items.push_back(
      BoolRow("showres", "Show Internal Resolution", Config::GFX_SHOW_INTERNAL_RESOLUTION));
}

void BuildEmulation(std::vector<Item>& items)
{
  items.push_back(ChoiceRow<float>("speedlimit", "Speed Limit", Config::MAIN_EMULATION_SPEED,
                                   {{0.5f, "50%"},
                                    {0.75f, "75%"},
                                    {1.0f, "100%"},
                                    {1.25f, "125%"},
                                    {1.5f, "150%"},
                                    {2.0f, "200%"},
                                    {3.0f, "300%"},
                                    {0.0f, "Unlimited"}}));
  items.push_back(OverrideRow("cpuclock", "CPU Clock Override", Config::MAIN_OVERCLOCK_ENABLE,
                              Config::MAIN_OVERCLOCK,
                              {0.5f, 0.75f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f}));
  items.push_back(OverrideRow("viclock", "VBI Frequency Override", Config::MAIN_VI_OVERCLOCK_ENABLE,
                              Config::MAIN_VI_OVERCLOCK, {0.5f, 0.75f, 1.25f, 1.5f, 2.0f}));
  items.push_back(BoolRow("syncgpu", "Sync GPU Thread", Config::MAIN_SYNC_GPU));
  items.push_back(BoolRow("dualcore", "Dual Core", Config::MAIN_CPU_THREAD, true));
  {
    Choices<PowerPC::CPUCore> cores;
    for (const PowerPC::CPUCore core : PowerPC::AvailableCPUCores())
    {
      switch (core)
      {
      case PowerPC::CPUCore::Interpreter:
        cores.emplace_back(core, "Interpreter");
        break;
      case PowerPC::CPUCore::CachedInterpreter:
        cores.emplace_back(core, "Cached Interpreter");
        break;
      default:
        cores.emplace_back(core, "JIT Recompiler");
        break;
      }
    }
    items.push_back(ChoiceRow<PowerPC::CPUCore>("cpucore", "CPU Emulation", Config::MAIN_CPU_CORE,
                                                std::move(cores), true));
  }
  items.push_back(BoolRow("mmu", "MMU", Config::MAIN_MMU, true));
  items.push_back(BoolRow("skipipl", "Skip Main Menu", Config::MAIN_SKIP_IPL, true));
  items.push_back(BoolRow("cheats", "Cheats", Config::MAIN_ENABLE_CHEATS, true));
}

void BuildAudio(std::vector<Item>& items)
{
  {
    const int v = Config::Get(Config::MAIN_AUDIO_VOLUME);
    Item item = SettingRow("volume", "Volume", {Config::MAIN_AUDIO_VOLUME.GetLocation()}, false);
    item.value = fmt::format("{}%", v);
    item.on_adjust = [v](int dir) {
      ChangeGameSettings(
          [next = std::clamp(v + dir * 10, 0, 100)](Config::LayerType layer, Core::System& system) {
            SetValue(layer, Config::MAIN_AUDIO_VOLUME, next);
            AudioCommon::UpdateSoundStream(system);
          });
    };
    items.push_back(std::move(item));
  }
  {
    const bool muted = Config::Get(Config::MAIN_AUDIO_MUTED);
    Item item = SettingRow("mute", "Mute", {Config::MAIN_AUDIO_MUTED.GetLocation()}, false);
    item.value = OnOff(muted);
    item.on_adjust = [muted](int) {
      ChangeGameSettings([muted](Config::LayerType layer, Core::System& system) {
        SetValue(layer, Config::MAIN_AUDIO_MUTED, !muted);
        AudioCommon::UpdateSoundStream(system);
      });
    };
    items.push_back(std::move(item));
  }
  items.push_back(ChoiceRow<bool>("dsp", "DSP Emulation", Config::MAIN_DSP_HLE,
                                  {{true, "HLE (Fast)"}, {false, "LLE (Accurate)"}}, true));
}

void BuildControls(std::vector<Item>& items)
{
  const std::string cur = Config::Get(GAME_PAD_PROFILE_1);
  std::vector<std::string> names = {""};
  names.insert(names.end(), s_profile_names.begin(), s_profile_names.end());
  if (std::ranges::find(names, cur) == names.end())
    names.push_back(cur);
  const int idx = int(std::ranges::find(names, cur) - names.begin());

  Item item = SettingRow("padprofile", "Controller Profile (Port 1)",
                         {GAME_PAD_PROFILE_1.GetLocation()}, true);
  item.value = cur.empty() ? "Default" : cur;
  item.on_adjust = [names = std::move(names), idx](int dir) {
    const std::string next = names[NextChoice(idx, dir, names.size())];
    if (next.empty())
    {
      // An empty value would make Dolphin load every profile in the folder; remove the key.
      ClearGameSettings({GAME_PAD_PROFILE_1.GetLocation()});
      return;
    }
    ChangeGameSettings([next](Config::LayerType layer, Core::System&) {
      if (layer == Config::LayerType::LocalGame)
        SetValue(layer, GAME_PAD_PROFILE_1, next);
    });
  };
  items.push_back(std::move(item));
}

void BuildCategory(std::vector<Item>& items, PageId id)
{
  switch (id)
  {
  case PageId::Graphics:
    BuildGraphics(items);
    break;
  case PageId::Enhancements:
    BuildEnhancements(items);
    break;
  case PageId::Performance:
    BuildPerformance(items);
    break;
  case PageId::Hacks:
    BuildHacks(items);
    break;
  case PageId::OnScreenDisplay:
    BuildOnScreenDisplay(items);
    break;
  case PageId::Emulation:
    BuildEmulation(items);
    break;
  case PageId::Audio:
    BuildAudio(items);
    break;
  case PageId::Controls:
    BuildControls(items);
    break;
  default:
    break;
  }
}

// The keys this game overrides among the settings a page manages.
std::vector<Config::Location> OverriddenSettings(PageId id)
{
  std::vector<Config::Location> out;
  std::vector<Item> items;
  BuildCategory(items, id);
  for (const Item& item : items)
  {
    for (const Config::Location& location : item.settings)
    {
      if (s_overridden.contains(location))
        out.push_back(location);
    }
  }
  return out;
}

const char* PageTitle(PageId id)
{
  switch (id)
  {
  case PageId::Main:
    return "";
  case PageId::SaveState:
    return "Save State";
  case PageId::LoadState:
    return "Load State";
  case PageId::ChangeDisc:
    return "Change Disc";
  case PageId::Settings:
    return "Settings";
  case PageId::Graphics:
    return "Graphics";
  case PageId::Enhancements:
    return "Enhancements";
  case PageId::Performance:
    return "Performance";
  case PageId::Hacks:
    return "Hacks";
  case PageId::OnScreenDisplay:
    return "On-Screen Display";
  case PageId::Emulation:
    return "Emulation";
  case PageId::Audio:
    return "Audio";
  case PageId::Controls:
    return "Controls";
  }
  return "";
}

void BuildSettings(std::vector<Item>& items, std::vector<PageState>& pages)
{
  items.push_back({.id = "scope", .label = "Changes apply to this game only", .enabled = false});
  std::vector<Config::Location> overridden;
  for (const PageId id : SETTINGS_CATEGORIES)
  {
    const std::vector<Config::Location> page_overridden = OverriddenSettings(id);
    const size_t changed = page_overridden.size();
    overridden.insert(overridden.end(), page_overridden.begin(), page_overridden.end());
    items.push_back({.id = fmt::format("page{}", int(id)),
                     .label = PageTitle(id),
                     .value = changed ? fmt::format("{} changed", changed) : "",
                     .submenu = true,
                     .dim_value = true,
                     .on_accept = PushPage(pages, id)});
  }
  items.push_back({.id = "fullscreen", .label = "Toggle Fullscreen", .on_accept = [] {
                     PushHostActionLocked(HostAction::FullScreen);
                   }});
  {
    const size_t idx = ThemeIndex(Config::Get(COUCH_THEME));
    items.push_back({.id = "theme",
                     .label = "Menu Theme",
                     .value = THEMES[idx].name,
                     .choice = true,
                     .on_adjust = [idx](int dir) {
                       const int next = Wrap(int(idx) + dir, int(THEMES.size()));
                       SetGlobalSetting(COUCH_THEME, std::string(THEMES[next].name));
                     }});
  }
  {
    static constexpr std::array names = {"Off", "Quiet", "Normal", "Loud"};
    const int level = std::clamp(Config::Get(COUCH_SOUND_LEVEL), 0, 3);
    items.push_back({.id = "sounds",
                     .label = "Menu Sounds",
                     .value = names[level],
                     .choice = true,
                     .on_adjust = [level](int dir) {
                       SetGlobalSetting(COUCH_SOUND_LEVEL, Wrap(level + dir, int(names.size())));
                     }});
  }
  const bool promotable = std::ranges::any_of(overridden, [](const Config::Location& l) {
    return l.system != Config::System::GameSettingsOnly;
  });
  items.push_back({.id = "makedefault",
                   .label = "Make This the Default",
                   .enabled = promotable,
                   .confirm = "Use for all games? Press A again",
                   .on_accept = [overridden] { MakeGameSettingsDefault(overridden); }});
  items.push_back({.id = "resetgame",
                   .label = "Reset This Game",
                   .enabled = !overridden.empty(),
                   .confirm = "Clear this game's changes? Press A again",
                   .on_accept = [overridden] { ClearGameSettings(overridden); }});
  items.push_back({.id = "baseline",
                   .label = "Restore Couch Potato Defaults",
                   .confirm = "Reset system defaults? Press A again",
                   .on_accept = [] {
                     // Per-game changes stay; only the system defaults are reset.
                     Core::QueueHostJob([](Core::System&) { ApplyBaselineValues(); }, false);
                     s_config_dirty = true;
                   }});
}

void GoBackLocked()
{
  s_armed_item.clear();
  if (s_pages.size() > 1)
  {
    SaveConfigIfDirtyLocked();
    s_pages.pop_back();
    QueueSoundLocked(Sound::Back);
  }
  else
  {
    CloseLocked(true);
  }
}

void Activate(Item& item)
{
  if (!item.enabled)
  {
    QueueSoundLocked(Sound::Deny);
    return;
  }
  if (!item.confirm.empty())
  {
    const bool armed = s_armed_item == item.id &&
                       Clock::now() - s_armed_at < std::chrono::seconds(4);
    if (!armed)
    {
      s_armed_item = item.id;
      s_armed_at = Clock::now();
      QueueSoundLocked(Sound::Change);
      return;
    }
  }
  s_armed_item.clear();
  QueueSoundLocked(item.on_accept ? Sound::Select : Sound::Change);
  if (item.on_accept)
    item.on_accept();
  else if (item.on_adjust)
    item.on_adjust(+1);
}

void MoveSelection(PageState& page, const std::vector<Item>& items, int dir, bool disarm = true)
{
  if (items.empty())
    return;
  const int n = int(items.size());
  int sel = page.selected;
  for (int tries = 0; tries < n; ++tries)
  {
    sel = Wrap(sel + dir, n);
    if (items[sel].enabled)
      break;
  }
  page.selected = sel;
  if (disarm)
    s_armed_item.clear();
}

void BuildPage(std::vector<Item>& items, PageId id)
{
  switch (id)
  {
  case PageId::Main:
    BuildMain(items, s_pages);
    break;
  case PageId::SaveState:
    BuildSlots(items, true);
    break;
  case PageId::LoadState:
    BuildSlots(items, false);
    break;
  case PageId::ChangeDisc:
    BuildDiscs(items);
    break;
  case PageId::Settings:
    BuildSettings(items, s_pages);
    break;
  default:
    BuildCategory(items, id);
    break;
  }
}

float PixelFontSize(float px)
{
  // ImGui 1.92 multiplies the pushed size by the global font scales; undo that so sizes here are
  // true pixels relative to the backbuffer.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float k = std::max(0.01f, style.FontScaleMain * style.FontScaleDpi);
  return px / k;
}
}  // namespace

// =============================================================================================
// Public API
// =============================================================================================

bool IsOpen()
{
  return s_open.load();
}

void UseFont(ImFont* font)
{
  s_font = font;
}

void ApplyBaselineIfNeeded()
{
  if (Config::Get(COUCH_BASELINE_VERSION) >= BASELINE_VERSION)
    return;
  ApplyBaselineValues();
  Config::Save();
}

void LoadFonts()
{
  s_font = nullptr;
  std::vector<std::string> candidates = {File::GetUserPath(D_LOAD_IDX) + "CouchOverlay_Font.ttf",
                                         File::GetUserPath(D_LOAD_IDX) + "CouchOverlay_Font.otf"};
#ifdef _WIN32
  if (const char* windir = std::getenv("WINDIR"))
  {
    candidates.push_back(std::string(windir) + "\\Fonts\\seguisb.ttf");  // Segoe UI Semibold
    candidates.push_back(std::string(windir) + "\\Fonts\\segoeui.ttf");
  }
#endif
  ImGuiIO& io = ImGui::GetIO();
  for (const std::string& path : candidates)
  {
    if (!File::Exists(path))
      continue;
    // The first font in the atlas is ImGui's default for everything else; keep it the OSD font.
    if (io.Fonts->Fonts.empty())
      io.Fonts->AddFontDefault();
    if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str()))
    {
      s_font = font;
      return;
    }
  }
}

void Toggle(Core::System& system)
{
  if (s_open)
  {
    std::lock_guard lk(s_mutex);
    CloseLocked(true);
    return;
  }

  const Core::State state = Core::GetState(system);
  if (state != Core::State::Running && state != Core::State::Paused)
    return;

  {
    std::lock_guard lk(s_mutex);
    // Reopened before the previous close finished resuming: the game is only paused because of
    // us, so it should still resume when this menu closes.
    const bool resume_pending = s_waiting_for_release && s_resume_on_close;
    PrepareMenuLocked();
    s_resume_on_close = state == Core::State::Running || resume_pending;
    s_waiting_for_release = false;
    s_open = true;
    QueueSoundLocked(Sound::Open);
  }
  s_redraw_queued = false;

  if (state == Core::State::Running)
  {
    Core::QueueHostJob(
        [](Core::System& sys) {
          if (s_open && Core::GetState(sys) == Core::State::Running)
            Core::SetState(sys, Core::State::Paused);
        },
        false);
  }
}

void PollHost(Core::System& system)
{
  const Core::State state = Core::GetState(system);
  const bool game_active = state == Core::State::Running || state == Core::State::Paused;
  if (!game_active)
  {
    if (s_open || s_waiting_for_release)
    {
      std::lock_guard lk(s_mutex);
      s_open = false;
      s_waiting_for_release = false;
      s_resume_on_close = false;
      s_host_actions.clear();
      s_pending_sound.reset();
    }
    return;
  }

  const bool open = s_open.load();
  bool waiting;
  {
    std::lock_guard lk(s_mutex);
    waiting = s_waiting_for_release;
  }
  if (!open && !waiting)
    return;

  PlayPendingSound();

  const NavButtons b = ReadNavButtons();
  const auto now = Clock::now();

  if (waiting)
  {
    if (!AnyHeld(b))
    {
      bool resume;
      {
        std::lock_guard lk(s_mutex);
        s_waiting_for_release = false;
        resume = s_resume_on_close;
        s_resume_on_close = false;
      }
      if (resume)
      {
        Core::QueueHostJob(
            [](Core::System& sys) {
              if (!s_open && Core::GetState(sys) == Core::State::Paused)
                Core::SetState(sys, Core::State::Running);
            },
            false);
      }
    }
    // Clear the menu off a still-paused screen.
    if (state == Core::State::Paused)
      RequestPausedRedraw(system);
    s_prev_accept = b.accept;
    s_prev_back = b.back;
    s_prev_start = b.start;
    s_prev_promote = b.promote;
    s_prev_revert = b.revert;
    return;
  }

  // Edge-triggered buttons and auto-repeating directions.
  std::vector<Nav> events;
  if (b.accept && !s_prev_accept)
    events.push_back(Nav::Accept);
  if (b.back && !s_prev_back)
    events.push_back(Nav::Back);
  if (b.start && !s_prev_start)
    events.push_back(Nav::Close);
  if (b.promote && !s_prev_promote)
    events.push_back(Nav::Promote);
  if (b.revert && !s_prev_revert)
    events.push_back(Nav::Revert);
  s_prev_accept = b.accept;
  s_prev_back = b.back;
  s_prev_start = b.start;
  s_prev_promote = b.promote;
  s_prev_revert = b.revert;

  const std::array<bool, 4> dirs = {b.up, b.down, b.left, b.right};
  constexpr std::array<Nav, 4> dir_nav = {Nav::Up, Nav::Down, Nav::Left, Nav::Right};
  for (size_t i = 0; i < dirs.size(); ++i)
  {
    RepeatState& rs = s_dir_repeat[i];
    if (!dirs[i])
    {
      rs.held = false;
      continue;
    }
    if (!rs.held)
    {
      rs.held = true;
      rs.next = now + std::chrono::milliseconds(380);
      events.push_back(dir_nav[i]);
    }
    else if (now >= rs.next)
    {
      rs.next = now + std::chrono::milliseconds(85);
      events.push_back(dir_nav[i]);
    }
  }

  bool refresh_slots = false, refresh_discs = false, refresh_lists = false;
  {
    std::lock_guard lk(s_mutex);
    for (Nav n : events)
      s_nav_events.push_back(n);
    refresh_slots = std::exchange(s_slot_refresh_requested, false);
    if (s_slot_refresh_at && now >= *s_slot_refresh_at)
    {
      s_slot_refresh_at.reset();
      refresh_slots = true;
    }
    refresh_discs = std::exchange(s_disc_refresh_requested, false);
    refresh_lists = std::exchange(s_lists_refresh_requested, false);
  }
  if (refresh_slots)
    RefreshSlotsOnHost();
  if (refresh_discs)
    RefreshDiscCandidatesOnHost(system);
  if (refresh_lists)
    RefreshSettingsListsOnHost();

  // Keep the menu alive while the game is paused behind it.
  if (state == Core::State::Paused)
    RequestPausedRedraw(system);
}

unsigned PollDialogNav()
{
  // Separate edge state from the menu so the two never swallow each other's presses.
  static NavButtons prev;
  const NavButtons b = ReadNavButtons();
  unsigned mask = 0;
  if (b.accept && !prev.accept)
    mask |= DialogNav_Accept;
  if (b.back && !prev.back)
    mask |= DialogNav_Back;
  if ((b.up && !prev.up) || (b.left && !prev.left))
    mask |= DialogNav_Prev;
  if ((b.down && !prev.down) || (b.right && !prev.right))
    mask |= DialogNav_Next;
  prev = b;
  return mask;
}

bool TakeHostAction(PendingHostAction* out)
{
  std::lock_guard lk(s_mutex);
  if (s_host_actions.empty())
    return false;
  *out = s_host_actions.front();
  s_host_actions.erase(s_host_actions.begin());
  return true;
}

void Draw()
{
  if (!s_open)
    return;

  std::lock_guard lk(s_mutex);
  if (!s_open || s_pages.empty())
    return;

  COLORS = THEMES[ThemeIndex(Config::Get(COUCH_THEME))].palette;

  ImGuiIO& io = ImGui::GetIO();
  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  const float ui = std::max(0.55f, std::min(W / 1920.0f, H / 1080.0f) * 1.25f);

  // Keyboard fallback (arrows / Enter / Backspace), handy for testing at a desk.
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
    s_nav_events.push_back(Nav::Up);
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
    s_nav_events.push_back(Nav::Down);
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
    s_nav_events.push_back(Nav::Left);
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
    s_nav_events.push_back(Nav::Right);
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
    s_nav_events.push_back(Nav::Accept);
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
    s_nav_events.push_back(Nav::Back);
  if (ImGui::IsKeyPressed(ImGuiKey_Insert, false))
    s_nav_events.push_back(Nav::Promote);
  if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    s_nav_events.push_back(Nav::Revert);

  // ---- Apply queued navigation, rebuilding the item list whenever the page changes ----
  std::vector<Item> items;
  const auto rebuild = [&items] {
    items.clear();
    PageState& page = s_pages.back();
    BuildPage(items, page.id);
    if (items.empty())
      return;
    page.selected = std::clamp(page.selected, 0, int(items.size()) - 1);
    if (!items[page.selected].enabled)
      MoveSelection(page, items, +1, false);
  };
  rebuild();

  while (!s_nav_events.empty() && s_open)
  {
    const Nav nav = s_nav_events.front();
    s_nav_events.pop_front();
    PageState& page = s_pages.back();
    const size_t depth = s_pages.size();
    const PageId id = page.id;
    switch (nav)
    {
    case Nav::Up:
    case Nav::Down:
    {
      const int before = page.selected;
      MoveSelection(page, items, nav == Nav::Up ? -1 : +1);
      if (page.selected != before)
        QueueSoundLocked(Sound::Move);
      break;
    }
    case Nav::Left:
    case Nav::Right:
      if (!items.empty())
      {
        Item& it = items[page.selected];
        if (it.choice && it.on_adjust && it.enabled)
        {
          it.on_adjust(nav == Nav::Left ? -1 : +1);
          QueueSoundLocked(Sound::Change);
        }
      }
      break;
    case Nav::Accept:
      if (!items.empty())
        Activate(items[page.selected]);
      break;
    case Nav::Back:
      GoBackLocked();
      break;
    case Nav::Close:
      CloseLocked(true);
      break;
    case Nav::Promote:
      if (!items.empty() && items[page.selected].promotable)
      {
        MakeGameSettingsDefault(items[page.selected].settings);
        QueueSoundLocked(Sound::Select);
      }
      else
      {
        QueueSoundLocked(Sound::Deny);
      }
      break;
    case Nav::Revert:
      if (!items.empty() && items[page.selected].overridden)
      {
        ClearGameSettings(items[page.selected].settings);
        QueueSoundLocked(Sound::Back);
      }
      else
      {
        QueueSoundLocked(Sound::Deny);
      }
      break;
    }
    if (!s_open)
      break;
    if (s_pages.size() != depth || s_pages.back().id != id)
      rebuild();
  }
  if (!s_open)
  {
    s_nav_events.clear();
    return;
  }

  // From here until the deferred action at the end, the page stack is not modified, so this
  // reference stays valid.
  PageState& page = s_pages.back();

  // ---- Layout ---- (whole pixels, so edges stay crisp at any scale)
  const auto px = [ui](float v) { return std::round(v * ui); };
  const float row_h = px(58.0f);
  const float font_px = px(31.0f);
  const float small_px = px(20.0f);
  const float icon_px = px(26.0f);
  const float pad_x = px(22.0f);
  const float header_h = px(54.0f);
  const float footer_h = px(46.0f);
  const float panel_w = std::min(W - px(40.0f), px(page.id == PageId::Main ? 520.0f : 760.0f));

  const int max_rows = std::max(3, int((H * 0.9f - header_h - footer_h) / row_h));
  const int n_items = int(items.size());
  const int visible = std::min(n_items, max_rows);
  if (n_items > 0)
  {
    if (page.selected < page.scroll)
      page.scroll = page.selected;
    if (page.selected >= page.scroll + visible)
      page.scroll = page.selected - visible + 1;
    page.scroll = std::clamp(page.scroll, 0, std::max(0, n_items - visible));
  }

  const float list_h = visible * row_h;
  const float total_h = header_h + list_h + footer_h;
  const ImVec2 origin{std::floor((W - panel_w) * 0.5f), std::floor((H - total_h) * 0.5f)};

  // Mouse clicks are collected here and applied after drawing, so nothing below sees a page
  // stack or item list that changed mid-frame.
  enum class Deferred
  {
    None,
    Back,
    Close,
    Activate,
    Adjust,
  };
  Deferred deferred = Deferred::None;
  int deferred_index = 0;

  // Dim the game behind the menu.
  ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, {W, H}, COLORS.dim);

  ImGui::PushFont(s_font, PixelFontSize(font_px));
  ImFont* const font = ImGui::GetFont();
  ImGui::SetNextWindowPos(origin);
  ImGui::SetNextWindowSize({panel_w, total_h});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoBackground;
  ImGui::SetNextWindowFocus();
  if (ImGui::Begin("##couch_overlay", nullptr, flags))
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float stroke = std::max(2.0f, 2.6f * ui);
    const bool mouse_moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    // Header: back arrow, page title (or game title on the main page), close cross.
    {
      const float cy = origin.y + header_h * 0.5f;
      const ImVec2 back_c{origin.x + pad_x + icon_px * 0.5f, cy};
      const ImVec2 close_c{origin.x + panel_w - pad_x - icon_px * 0.5f, cy};
      const float hit = header_h * 0.45f;
      const bool back_hover =
          ImGui::IsMouseHoveringRect({back_c.x - hit, cy - hit}, {back_c.x + hit, cy + hit});
      const bool close_hover =
          ImGui::IsMouseHoveringRect({close_c.x - hit, cy - hit}, {close_c.x + hit, cy + hit});
      if ((COLORS.chrome >> IM_COL32_A_SHIFT) != 0)
      {
        dl->AddRectFilled(origin, {origin.x + panel_w, origin.y + header_h}, COLORS.chrome);
        dl->AddRectFilled({origin.x, origin.y + total_h - footer_h},
                          {origin.x + panel_w, origin.y + total_h}, COLORS.chrome);
      }
      DrawBackArrow(dl, back_c, icon_px, back_hover ? COLORS.accent : COLORS.chrome_text, stroke);
      DrawCross(dl, close_c, icon_px, close_hover ? COLORS.accent : COLORS.chrome_text, stroke);

      std::string title = PageTitle(page.id);
      if (title.empty())
        title = SConfig::GetInstance().GetTitleName();
      if (!title.empty())
      {
        const float max_w = panel_w - 2.0f * (pad_x + icon_px + 16.0f * ui);
        ImVec2 ts = font->CalcTextSizeA(small_px, FLT_MAX, 0.0f, title.c_str());
        while (ts.x > max_w && title.size() > 4)
        {
          title.resize(title.size() - 4);
          title += "...";
          ts = font->CalcTextSizeA(small_px, FLT_MAX, 0.0f, title.c_str());
        }
        dl->AddText(font, small_px, {origin.x + (panel_w - ts.x) * 0.5f, cy - ts.y * 0.5f},
                    COLORS.chrome_dim, title.c_str());
      }

      if (clicked && back_hover)
        deferred = Deferred::Back;
      else if (clicked && close_hover)
        deferred = Deferred::Close;
    }

    // List.
    const float list_top = origin.y + header_h;
    dl->AddRectFilled({origin.x, list_top}, {origin.x + panel_w, list_top + list_h},
                      COLORS.panel);
    if (!s_armed_item.empty() && Clock::now() - s_armed_at >= std::chrono::seconds(4))
      s_armed_item.clear();

    for (int row = 0; row < visible; ++row)
    {
      const int i = page.scroll + row;
      const Item& it = items[i];
      const ImVec2 rmin{origin.x, list_top + row * row_h};
      const ImVec2 rmax{origin.x + panel_w, rmin.y + row_h};
      const float cy = rmin.y + row_h * 0.5f;
      const bool hovered = ImGui::IsMouseHoveringRect(rmin, rmax);
      if (hovered && mouse_moved && it.enabled && page.selected != i)
      {
        page.selected = i;
        s_armed_item.clear();
      }
      const bool selected = i == page.selected;

      if (selected)
      {
        dl->AddRectFilled(rmin, rmax, COLORS.row_selected);
        dl->AddRectFilled(rmin, {rmin.x + 4.0f * ui, rmax.y}, COLORS.accent);
      }
      else if (hovered && it.enabled)
      {
        dl->AddRectFilled(rmin, rmax, COLORS.row_hover);
      }

      const ImU32 text_col = it.enabled ? COLORS.text : COLORS.text_disabled;
      float x = rmin.x + pad_x;
      if (it.icon != Icon::None)
      {
        DrawIcon(dl, it.icon, {x + icon_px * 0.5f, cy}, icon_px, text_col);
        x += icon_px + 16.0f * ui;
      }

      const bool armed = it.id == s_armed_item;
      const std::string& label = armed ? it.confirm : it.label;
      const ImVec2 ls = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, label.c_str());
      static constexpr const char* NEXT_LAUNCH = "Next launch";
      const bool show_note = it.next_launch && !armed;
      const float note_gap = 12.0f * ui;
      const ImVec2 ns = font->CalcTextSizeA(small_px, FLT_MAX, 0.0f, NEXT_LAUNCH);
      const float label_w = ls.x + (show_note ? note_gap + ns.x : 0.0f);

      // This game's INI overrides the setting.
      if (it.overridden)
        dl->AddCircleFilled({rmin.x + pad_x * 0.5f, cy}, std::max(3.0f, 4.5f * ui), COLORS.accent);

      // Right side: chevron for submenus, < value > for choices, plain value otherwise.
      float right = rmax.x - pad_x;
      if (it.submenu)
      {
        DrawChevron(dl, {right - icon_px * 0.3f, cy}, icon_px * 0.8f, text_col, true, stroke);
        right -= icon_px;
      }
      if (!it.value.empty() && !armed)
      {
        const float vpx = it.choice ? font_px * 0.85f : small_px;
        std::string value = it.value;
        const float max_vw = std::max(40.0f * ui, (right - x) - label_w - 60.0f * ui);
        ImVec2 vs = font->CalcTextSizeA(vpx, FLT_MAX, 0.0f, value.c_str());
        while (vs.x > max_vw && value.size() > 4)
        {
          value = "..." + value.substr(4);
          vs = font->CalcTextSizeA(vpx, FLT_MAX, 0.0f, value.c_str());
        }
        const ImU32 vcol =
            (it.dim_value || !it.enabled || !selected) ? COLORS.text_dim : COLORS.text;
        if (it.choice)
        {
          const float ch = font_px * 0.45f;
          const ImU32 ccol = selected ? COLORS.accent : COLORS.text_dim;
          DrawChevron(dl, {right - ch * 0.5f, cy}, ch, ccol, true, stroke * 0.85f);
          right -= ch + 10.0f * ui;
          dl->AddText(font, vpx, {right - vs.x, cy - vs.y * 0.5f}, vcol, value.c_str());
          right -= vs.x + 10.0f * ui;
          DrawChevron(dl, {right - ch * 0.5f, cy}, ch, ccol, false, stroke * 0.85f);
        }
        else
        {
          dl->AddText(font, vpx, {right - vs.x, cy - vs.y * 0.5f}, vcol, value.c_str());
        }
      }

      dl->AddText(font, font_px, {x, cy - ls.y * 0.5f}, armed ? COLORS.warn : text_col,
                  label.c_str());
      if (show_note)
      {
        dl->AddText(font, small_px, {x + ls.x + note_gap, cy - ns.y * 0.5f}, COLORS.text_dim,
                    NEXT_LAUNCH);
      }

      if (clicked && hovered && it.enabled && deferred == Deferred::None)
      {
        deferred = (it.choice && it.on_adjust) ? Deferred::Adjust : Deferred::Activate;
        deferred_index = i;
      }
    }

    // Scroll hints.
    if (page.scroll > 0)
      DrawChevronV(dl, {origin.x + panel_w * 0.5f, list_top + px(9.0f)}, px(16.0f),
                   COLORS.text_dim, false, stroke);
    if (page.scroll + visible < n_items)
      DrawChevronV(dl, {origin.x + panel_w * 0.5f, list_top + list_h - px(9.0f)}, px(16.0f),
                   COLORS.text_dim, true, stroke);

    // Footer: controller hints.
    {
      const float fy = list_top + list_h;
      const float g = 24.0f * ui;
      float x = origin.x + pad_x;
      const float gy = fy + (footer_h - g) * 0.5f;
      const auto hint = [&](const char* glyph, ImU32 col, const char* text) {
        x += DrawButtonGlyph(dl, font, {x, gy}, g, glyph, col) + 8.0f * ui;
        const ImVec2 ts = font->CalcTextSizeA(small_px, FLT_MAX, 0.0f, text);
        dl->AddText(font, small_px, {x, gy + (g - ts.y) * 0.5f}, COLORS.chrome_dim, text);
        x += ts.x + 22.0f * ui;
      };
      const Item* sel = n_items > 0 ? &items[page.selected] : nullptr;
      // Overridden rows have more hints than fit; A on a choice row only cycles the value anyway.
      if (!(sel && sel->choice && sel->overridden))
        hint("A", IM_COL32(96, 186, 72, 255), "Select");
      hint("B", IM_COL32(222, 74, 64, 255), s_pages.size() > 1 ? "Back" : "Resume");
      if (sel && sel->choice)
      {
        const char* t = "Change";
        const ImVec2 ts = font->CalcTextSizeA(small_px, FLT_MAX, 0.0f, t);
        const float cx = x + g * 0.5f;
        DrawChevron(dl, {cx - g * 0.25f, gy + g * 0.5f}, g * 0.6f, COLORS.chrome_dim, false,
                    stroke * 0.8f);
        DrawChevron(dl, {cx + g * 0.25f, gy + g * 0.5f}, g * 0.6f, COLORS.chrome_dim, true,
                    stroke * 0.8f);
        x += g + 8.0f * ui;
        dl->AddText(font, small_px, {x, gy + (g - ts.y) * 0.5f}, COLORS.chrome_dim, t);
        x += ts.x + 22.0f * ui;
      }
      if (sel && sel->promotable)
        hint("X", IM_COL32(64, 140, 230, 255), "Set as System Default");
      if (sel && sel->overridden)
        hint("Y", IM_COL32(240, 190, 40, 255), "Use System Default");
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopFont();

  // ---- Deferred mouse action ----
  switch (deferred)
  {
  case Deferred::None:
    break;
  case Deferred::Back:
    GoBackLocked();
    break;
  case Deferred::Close:
    CloseLocked(true);
    break;
  case Deferred::Activate:
    page.selected = deferred_index;
    Activate(items[deferred_index]);
    break;
  case Deferred::Adjust:
    page.selected = deferred_index;
    items[deferred_index].on_adjust(+1);
    break;
  }
}
}  // namespace CouchOverlay
