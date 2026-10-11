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
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "AudioCommon/AudioCommon.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/StringUtil.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/GCPad.h"
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
};

enum class PageId
{
  Main,
  SaveState,
  LoadState,
  ChangeDisc,
  Settings,
};

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

// Paused redraw bookkeeping.
std::atomic<bool> s_redraw_queued{false};
Clock::time_point s_redraw_queued_at;
Clock::time_point s_last_redraw_request;

// Host-thread-only input repeat state.
struct RepeatState
{
  bool held = false;
  Clock::time_point next;
};
std::array<RepeatState, 4> s_dir_repeat;
bool s_prev_accept = false, s_prev_back = false, s_prev_start = false;

// ---------------------------------------------------------------------------------------------
// Helpers usable from any thread
// ---------------------------------------------------------------------------------------------

void PushHostActionLocked(HostAction action, int arg = 0)
{
  s_host_actions.push_back({action, arg});
}

void ResetMenuLocked()
{
  s_pages.clear();
  s_pages.push_back({});
  s_nav_events.clear();
  s_armed_item.clear();
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

// ---------------------------------------------------------------------------------------------
// Controller reading (host thread)
// ---------------------------------------------------------------------------------------------

struct NavButtons
{
  bool up = false, down = false, left = false, right = false;
  bool accept = false, back = false, start = false;
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
  return b;
}

bool AnyHeld(const NavButtons& b)
{
  return b.up || b.down || b.left || b.right || b.accept || b.back || b.start;
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
  Fullscreen,
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
};

struct Palette
{
  ImU32 dim = IM_COL32(0, 0, 0, 150);
  ImU32 panel = IM_COL32(24, 24, 26, 245);
  ImU32 row_hover = IM_COL32(255, 255, 255, 18);
  ImU32 row_selected = IM_COL32(255, 255, 255, 30);
  ImU32 accent = IM_COL32(64, 156, 255, 255);
  ImU32 text = IM_COL32(236, 236, 240, 255);
  ImU32 text_dim = IM_COL32(150, 150, 158, 255);
  ImU32 text_disabled = IM_COL32(100, 100, 106, 255);
  ImU32 warn = IM_COL32(255, 176, 64, 255);
};
constexpr Palette COLORS{};

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
  case Icon::Fullscreen:
  {
    const float k = r * 0.4f;
    for (int sx = -1; sx <= 1; sx += 2)
    {
      for (int sy = -1; sy <= 1; sy += 2)
      {
        const ImVec2 corner{c.x + sx * r * 0.85f, c.y + sy * r * 0.7f};
        dl->AddLine(corner, {corner.x - sx * k, corner.y}, col, t);
        dl->AddLine(corner, {corner.x, corner.y - sy * k}, col, t);
      }
    }
    break;
  }
  }
}

void DrawChevron(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool right, float thickness)
{
  const float dx = right ? s * 0.25f : -s * 0.25f;
  dl->AddLine({c.x - dx, c.y - s * 0.45f}, {c.x + dx, c.y}, col, thickness);
  dl->AddLine({c.x + dx, c.y}, {c.x - dx, c.y + s * 0.45f}, col, thickness);
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

// ----- Settings helpers -----

template <typename T>
void SetSetting(const Config::Info<T>& info, std::type_identity_t<T> value)
{
  // Config is owned by the UI thread; apply there. The paused CPU thread picks the change up the
  // next time we wake it for a redraw, so graphics settings preview live behind the menu.
  Core::QueueHostJob(
      [&info, value](Core::System&) { Config::SetBaseOrCurrent(info, value); }, false);
  s_config_dirty = true;
}

std::string OnOff(bool v)
{
  return v ? "On" : "Off";
}

int Wrap(int v, int n)
{
  return ((v % n) + n) % n;
}

void BuildMain(std::vector<Item>& items, std::vector<PageState>& pages)
{
  auto push_page = [&pages](PageId id) {
    return [&pages, id] { pages.push_back({id, 0, 0}); };
  };

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
                   .on_accept = push_page(PageId::Settings)});
  items.push_back({.id = "restart",
                   .label = "Restart",
                   .icon = Icon::Restart,
                   .confirm = "Press again to restart",
                   .on_accept = [] {
                     PushHostActionLocked(HostAction::Reset);
                     CloseLocked(true);
                   }});
  items.push_back({.id = "exit",
                   .label = "Exit",
                   .icon = Icon::Exit,
                   .confirm = "Press again to exit",
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
        item.confirm = "Press again to overwrite";
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

void BuildSettings(std::vector<Item>& items)
{
  // Aspect ratio
  {
    static constexpr std::array modes = {AspectMode::Auto, AspectMode::ForceWide,
                                         AspectMode::ForceStandard, AspectMode::Stretch};
    static constexpr std::array names = {"Auto", "16:9", "4:3", "Stretch"};
    const AspectMode cur = Config::Get(Config::GFX_ASPECT_RATIO);
    const auto it = std::ranges::find(modes, cur);
    const int idx = it == modes.end() ? -1 : int(it - modes.begin());
    items.push_back({.id = "aspect",
                     .label = "Aspect Ratio",
                     .value = idx < 0 ? "Custom" : names[idx],
                     .choice = true,
                     .on_adjust = [idx](int dir) {
                       const int next = Wrap((idx < 0 ? 0 : idx) + dir, int(modes.size()));
                       SetSetting(Config::GFX_ASPECT_RATIO, modes[next]);
                     }});
  }
  // Widescreen hack
  {
    const bool v = Config::Get(Config::GFX_WIDESCREEN_HACK);
    items.push_back({.id = "wshack",
                     .label = "Widescreen Hack",
                     .value = OnOff(v),
                     .choice = true,
                     .on_adjust = [v](int) { SetSetting(Config::GFX_WIDESCREEN_HACK, !v); }});
  }
  // Internal resolution
  {
    static constexpr std::array names = {"Auto (Window)", "Native (480p)", "2x (720p)",
                                         "3x (1080p)",    "4x (1440p)",    "5x (1620p)",
                                         "6x (4K)",       "7x (3360p)",    "8x (5K)"};
    const int v = std::clamp(Config::Get(Config::GFX_EFB_SCALE), 0, int(names.size()) - 1);
    items.push_back({.id = "efb",
                     .label = "Internal Resolution",
                     .value = names[v],
                     .choice = true,
                     .on_adjust = [v](int dir) {
                       SetSetting(Config::GFX_EFB_SCALE, Wrap(v + dir, int(names.size())));
                     }});
  }
  // Post-processing shader
  {
    std::vector<std::string> shaders = VideoCommon::PostProcessing::GetShaderList();
    shaders.insert(shaders.begin(), "");
    const std::string cur = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
    const auto it = std::ranges::find(shaders, cur);
    const int idx = it == shaders.end() ? 0 : int(it - shaders.begin());
    items.push_back({.id = "shader",
                     .label = "Shader",
                     .value = cur.empty() ? "None" : cur,
                     .choice = true,
                     .on_adjust = [shaders, idx](int dir) {
                       const int next = Wrap(idx + dir, int(shaders.size()));
                       SetSetting(Config::GFX_ENHANCE_POST_SHADER, shaders[next]);
                     }});
  }
  // V-Sync
  {
    const bool v = Config::Get(Config::GFX_VSYNC);
    items.push_back({.id = "vsync",
                     .label = "V-Sync",
                     .value = OnOff(v),
                     .choice = true,
                     .on_adjust = [v](int) { SetSetting(Config::GFX_VSYNC, !v); }});
  }
  // FPS counter
  {
    const bool v = Config::Get(Config::GFX_SHOW_FPS);
    items.push_back({.id = "fps",
                     .label = "Show FPS",
                     .value = OnOff(v),
                     .choice = true,
                     .on_adjust = [v](int) { SetSetting(Config::GFX_SHOW_FPS, !v); }});
  }
  // Volume
  {
    const int v = Config::Get(Config::MAIN_AUDIO_VOLUME);
    items.push_back({.id = "volume",
                     .label = "Volume",
                     .value = fmt::format("{}%", v),
                     .choice = true,
                     .on_adjust = [v](int dir) {
                       const int next = std::clamp(v + dir * 10, 0, 100);
                       Core::QueueHostJob(
                           [next](Core::System& system) {
                             Config::SetBaseOrCurrent(Config::MAIN_AUDIO_VOLUME, next);
                             AudioCommon::UpdateSoundStream(system);
                           },
                           false);
                       s_config_dirty = true;
                     }});
  }
  // Speed limit
  {
    static constexpr std::array speeds = {1.0f, 0.0f, 0.5f, 1.5f, 2.0f};
    static constexpr std::array names = {"100%", "Unlimited", "50%", "150%", "200%"};
    const float cur = Config::Get(Config::MAIN_EMULATION_SPEED);
    int idx = -1;
    for (size_t i = 0; i < speeds.size(); ++i)
    {
      if (std::abs(speeds[i] - cur) < 0.01f)
        idx = int(i);
    }
    items.push_back({.id = "speed",
                     .label = "Speed Limit",
                     .value = idx < 0 ? fmt::format("{:.0f}%", cur * 100.0f) : names[idx],
                     .choice = true,
                     .on_adjust = [idx](int dir) {
                       const int next = Wrap((idx < 0 ? 0 : idx) + dir, int(speeds.size()));
                       SetSetting(Config::MAIN_EMULATION_SPEED, speeds[next]);
                     }});
  }
  items.push_back({.id = "fullscreen",
                   .label = "Toggle Fullscreen",
                   .icon = Icon::Fullscreen,
                   .on_accept = [] { PushHostActionLocked(HostAction::FullScreen); }});
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
  }
  return "";
}

void GoBackLocked()
{
  s_armed_item.clear();
  if (s_pages.size() > 1)
  {
    if (s_pages.back().id == PageId::Settings)
      SaveConfigIfDirtyLocked();
    s_pages.pop_back();
  }
  else
  {
    CloseLocked(true);
  }
}

void Activate(Item& item)
{
  if (!item.enabled)
    return;
  if (!item.confirm.empty())
  {
    const bool armed = s_armed_item == item.id &&
                       Clock::now() - s_armed_at < std::chrono::seconds(4);
    if (!armed)
    {
      s_armed_item = item.id;
      s_armed_at = Clock::now();
      return;
    }
  }
  s_armed_item.clear();
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
    BuildSettings(items);
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
    ResetMenuLocked();
    s_resume_on_close = state == Core::State::Running || resume_pending;
    s_waiting_for_release = false;
    s_open = true;
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
  s_prev_accept = b.accept;
  s_prev_back = b.back;
  s_prev_start = b.start;

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

  bool refresh_slots = false, refresh_discs = false;
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
  }
  if (refresh_slots)
    RefreshSlotsOnHost();
  if (refresh_discs)
    RefreshDiscCandidatesOnHost(system);

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
      MoveSelection(page, items, -1);
      break;
    case Nav::Down:
      MoveSelection(page, items, +1);
      break;
    case Nav::Left:
    case Nav::Right:
      if (!items.empty())
      {
        Item& it = items[page.selected];
        if (it.choice && it.on_adjust && it.enabled)
          it.on_adjust(nav == Nav::Left ? -1 : +1);
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

  // ---- Layout ----
  ImFont* font = ImGui::GetFont();
  const float row_h = 58.0f * ui;
  const float font_px = 30.0f * ui;
  const float small_px = 20.0f * ui;
  const float icon_px = 26.0f * ui;
  const float pad_x = 22.0f * ui;
  const float header_h = 54.0f * ui;
  const float footer_h = 46.0f * ui;
  const float panel_w =
      std::min(W - 40.0f * ui, (page.id == PageId::Main ? 520.0f : 760.0f) * ui);

  const int max_rows = std::max(3, int((H * 0.78f - header_h - footer_h) / row_h));
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

  ImGui::PushFont(nullptr, PixelFontSize(font_px));
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
      DrawBackArrow(dl, back_c, icon_px, back_hover ? COLORS.accent : COLORS.text, stroke);
      DrawCross(dl, close_c, icon_px, close_hover ? COLORS.accent : COLORS.text, stroke);

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
                    COLORS.text_dim, title.c_str());
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
        const float max_vw = std::max(40.0f * ui, (right - x) - ls.x - 60.0f * ui);
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

      if (clicked && hovered && it.enabled && deferred == Deferred::None)
      {
        deferred = (it.choice && it.on_adjust) ? Deferred::Adjust : Deferred::Activate;
        deferred_index = i;
      }
    }

    // Scroll hints.
    if (page.scroll > 0)
      DrawChevron(dl, {origin.x + panel_w * 0.5f, list_top + 8.0f * ui}, 14.0f * ui,
                  COLORS.text_dim, true, stroke);
    if (page.scroll + visible < n_items)
      DrawChevron(dl, {origin.x + panel_w * 0.5f, list_top + list_h - 8.0f * ui}, 14.0f * ui,
                  COLORS.text_dim, false, stroke);

    // Footer: controller hints.
    {
      const float fy = list_top + list_h;
      const float g = 24.0f * ui;
      float x = origin.x + pad_x;
      const float gy = fy + (footer_h - g) * 0.5f;
      const auto hint = [&](const char* glyph, ImU32 col, const char* text) {
        x += DrawButtonGlyph(dl, font, {x, gy}, g, glyph, col) + 8.0f * ui;
        const ImVec2 ts = font->CalcTextSizeA(small_px, FLT_MAX, 0.0f, text);
        dl->AddText(font, small_px, {x, gy + (g - ts.y) * 0.5f}, COLORS.text_dim, text);
        x += ts.x + 22.0f * ui;
      };
      hint("A", IM_COL32(96, 186, 72, 255), "Select");
      hint("B", IM_COL32(222, 74, 64, 255), s_pages.size() > 1 ? "Back" : "Resume");
      if (n_items > 0 && items[page.selected].choice)
      {
        const char* t = "Change";
        const ImVec2 ts = font->CalcTextSizeA(small_px, FLT_MAX, 0.0f, t);
        const float cx = x + g * 0.5f;
        DrawChevron(dl, {cx - g * 0.25f, gy + g * 0.5f}, g * 0.6f, COLORS.text_dim, false,
                    stroke * 0.8f);
        DrawChevron(dl, {cx + g * 0.25f, gy + g * 0.5f}, g * 0.6f, COLORS.text_dim, true,
                    stroke * 0.8f);
        x += g + 8.0f * ui;
        dl->AddText(font, small_px, {x, gy + (g - ts.y) * 0.5f}, COLORS.text_dim, t);
      }
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
