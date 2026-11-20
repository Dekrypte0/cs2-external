#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <dwmapi.h>
#include "imgui.h"
#include "overlay.hpp"
#include "renderer.hpp"
#include "game.hpp"
#include "logger.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

struct ESPSettings {
    bool enabled = true;
    bool showBox = true;
    bool showHealthBar = true;
    bool showHealthText = false;
    bool showDistance = true;
    bool showTeamName = true;
    bool showNames = true;
    bool showSnaplines = false;
    bool showOutline = true;
    bool filledBox = false;
    bool teamCheck = false;   // show everyone by default; the menu can re-enable enemies-only
    bool showSelf = false;    // camera pawn is detected and names resolve, but its box is off by default

    float boxThickness = 1.5f;
    float snaplineThickness = 1.0f;
    float maxDistance = 0.0f; // 0 = unlimited

    ImVec4 enemyColor = ImVec4(1.00f, 0.45f, 0.25f, 1.00f);
    ImVec4 teamColor  = ImVec4(0.35f, 0.70f, 1.00f, 1.00f);
    ImVec4 textColor  = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    ImVec4 outlineColor = ImVec4(0.00f, 0.00f, 0.00f, 0.80f);
    ImVec4 snaplineColor = ImVec4(1.00f, 1.00f, 1.00f, 0.60f);
};

/* UI palette — accent-driven dark theme */
static const ImVec4 kAccent    = ImVec4(0.35f, 0.62f, 1.00f, 1.00f);
static const ImVec4 kAccentDim = ImVec4(0.14f, 0.22f, 0.38f, 1.00f);
static const ImVec4 kPanelBg   = ImVec4(0.07f, 0.075f, 0.095f, 0.97f);
static const ImVec4 kGoodGreen = ImVec4(0.30f, 0.85f, 0.40f, 1.00f);
static const ImVec4 kBadRed    = ImVec4(0.95f, 0.35f, 0.30f, 1.00f);

/* health color: red -> yellow -> green across the range */
static ImU32 HpColor(float hp) {
    float r, g, b;
    if (hp > 0.5f) {
        const float k = (hp - 0.5f) * 2.0f;
        r = 0.95f + (0.20f - 0.95f) * k;
        g = 0.80f + (0.90f - 0.80f) * k;
        b = 0.20f + (0.30f - 0.20f) * k;
    } else {
        const float k = hp * 2.0f;
        r = 0.95f;
        g = 0.25f + (0.80f - 0.25f) * k;
        b = 0.20f;
    }
    return IM_COL32((int)(r * 255.0f), (int)(g * 255.0f), (int)(b * 255.0f), 255);
}

static ImU32 DimCol(ImU32 c, float f) {
    const int r = (int)(((c >> 0) & 0xFF) * f);
    const int g = (int)(((c >> 8) & 0xFF) * f);
    const int b = (int)(((c >> 16) & 0xFF) * f);
    const int a = (int)((c >> 24) & 0xFF);
    return IM_COL32(r, g, b, a);
}

class CS2ESP {
private:
    Overlay overlay;
    Renderer renderer;
    Game game;

    ESPSettings settings;

    bool showMenu = false;
    bool showDebugWindow = false;
    bool showPlayerList = false;
    bool menuClickThrough = true;

    int entityCount = 0;
    int localTeam = 0;
    int lastDrawn = -1;
    bool gameWindowSynced = false;
    std::string lastError;

    /* render-side accounting: "detected" and "on screen" are different facts,
     * and a missing box should always be explainable by a counter here. */
    struct DrawStats {
        int total = 0;
        int local = 0;
        int team = 0;
        int dist = 0;
        int offscreen = 0;
        int tiny = 0;
        int drawn = 0;
    };
    DrawStats lastDrawStats;

    static ImU32 ToU32(const ImVec4& c, float alphaMul = 1.0f) {
        return IM_COL32(
            (int)(c.x * 255.0f),
            (int)(c.y * 255.0f),
            (int)(c.z * 255.0f),
            (int)(c.w * alphaMul * 255.0f));
    }

    void PushMenuTheme() {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelBg);
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.08f, 0.085f, 0.105f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.24f, 0.36f, 0.65f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.11f, 0.12f, 0.155f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.15f, 0.17f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.17f, 0.23f, 0.33f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, kAccent);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kAccent);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.13f, 0.19f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.17f, 0.25f, 0.42f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.21f, 0.31f, 0.52f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.20f, 0.26f, 0.38f, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.56f, 0.60f, 0.68f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.10f, 0.11f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.19f, 0.28f, 0.46f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabActive, kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_TabUnfocused, ImVec4(0.10f, 0.11f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive, ImVec4(0.13f, 0.16f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 7));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    }

    void PopMenuTheme() {
        ImGui::PopStyleColor(21);
        ImGui::PopStyleVar(7);
    }

public:
    bool Initialize(bool verbose = true) {
        if (!game.Initialize(verbose)) {
            lastError = "Failed to init memory";
            if (verbose) Log("[!] " + lastError);
            return false;
        }

        if (!overlay.Initialize()) {
            lastError = "Failed to create overlay";
            Log("[!] " + lastError);
            return false;
        }

        if (!renderer.Initialize(overlay.GetHWND())) {
            lastError = "Failed to init D3D11";
            Log("[!] " + lastError);
            return false;
        }

        ApplyMenuState();
        Log("[+] Ready");
        return true;
    }

    void ApplyMenuState() {
        HWND hwnd = overlay.GetHWND();
        LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        if (showMenu) {
            ex &= ~WS_EX_TRANSPARENT;
            menuClickThrough = false;
        } else {
            ex |= WS_EX_TRANSPARENT;
            menuClickThrough = true;
        }
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
    }

    void ToggleMenu() {
        showMenu = !showMenu;
        ApplyMenuState();
    }

    void RenderMenu() {
        if (!showMenu) return;

        PushMenuTheme();
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(540, 520), ImGuiCond_FirstUseEver);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
        if (ImGui::Begin("cs2-external", &showMenu, flags)) {
            /* header */
            ImGui::TextColored(kAccent, "CS2 EXTERNAL");
            ImGui::SameLine();
            ImGui::TextDisabled("read-only memory overlay");
            ImGui::TextDisabled("INSERT menu | F2 ESP | F1 debug | END exit");

            if (ImGui::BeginTabBar("##tabs")) {
                if (ImGui::BeginTabItem("Visuals")) {
                    ImGui::Spacing();
                    ImGui::Checkbox("Enable ESP", &settings.enabled);
                    ImGui::SameLine();
                    ImGui::Checkbox("Enemies only", &settings.teamCheck);
                    ImGui::SameLine();
                    ImGui::Checkbox("Show self", &settings.showSelf);

                    ImGui::Spacing();
                    ImGui::SeparatorText("Elements");
                    ImGui::Checkbox("Box", &settings.showBox);
                    ImGui::SameLine();
                    ImGui::Checkbox("Filled box", &settings.filledBox);
                    ImGui::SameLine();
                    ImGui::Checkbox("Outline", &settings.showOutline);

                    ImGui::Checkbox("Health bar", &settings.showHealthBar);
                    ImGui::SameLine();
                    ImGui::Checkbox("Health text", &settings.showHealthText);

                    ImGui::Checkbox("Player names", &settings.showNames);
                    ImGui::SameLine();
                    ImGui::Checkbox("Team label", &settings.showTeamName);
                    ImGui::SameLine();
                    ImGui::Checkbox("Distance", &settings.showDistance);
                    ImGui::Checkbox("Snaplines", &settings.showSnaplines);

                    ImGui::Spacing();
                    ImGui::SeparatorText("Style");
                    ImGui::SliderFloat("Box thickness", &settings.boxThickness, 0.5f, 4.0f, "%.1f");
                    ImGui::SliderFloat("Snapline thickness", &settings.snaplineThickness, 0.5f, 3.0f, "%.1f");
                    ImGui::SliderFloat("Max distance (m, 0=off)", &settings.maxDistance, 0.0f, 300.0f, "%.0f");

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Colors")) {
                    ImGui::Spacing();
                    ImGui::SeparatorText("ESP colors");
                    ImGui::ColorEdit4("Enemy", &settings.enemyColor.x, ImGuiColorEditFlags_AlphaBar);
                    ImGui::ColorEdit4("Team", &settings.teamColor.x, ImGuiColorEditFlags_AlphaBar);
                    ImGui::ColorEdit4("Text", &settings.textColor.x, ImGuiColorEditFlags_AlphaBar);
                    ImGui::ColorEdit4("Outline", &settings.outlineColor.x, ImGuiColorEditFlags_AlphaBar);
                    ImGui::ColorEdit4("Snaplines", &settings.snaplineColor.x, ImGuiColorEditFlags_AlphaBar);

                    ImGui::Spacing();
                    ImGui::TextDisabled("Tip: lower alpha for softer visuals.");
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Misc")) {
                    ImGui::Spacing();
                    ImGui::SeparatorText("Windows");
                    ImGui::Checkbox("Player list", &showPlayerList);
                    ImGui::Checkbox("Debug", &showDebugWindow);

                    ImGui::Spacing();
                    ImGui::SeparatorText("Status");
                    ImGui::Text("Alive players detected: %d", entityCount);
                    ImGui::Text("Boxes drawn last frame: %d", (lastDrawn < 0 ? 0 : lastDrawn));
                    ImGui::Text("Local team: %d (%s)", localTeam,
                        localTeam == Offsets::TEAM_T ? "T" : (localTeam == Offsets::TEAM_CT ? "CT" : (localTeam == Offsets::TEAM_SPECTATOR ? "SPEC" : "?")));
                    ImGui::Text("Spectating: %s", game.IsSpectating() ? "yes" : "no");
                    ImGui::Text("Overlay: %dx%d", overlay.GetWidth(), overlay.GetHeight());

                    ImGui::Spacing();
                    ImGui::TextDisabled("If spectate ESP is misaligned, toggle ESP (F2) once after the view settles.");
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            /* footer status bar — pinned to the bottom of the window */
            ImGui::SetCursorPosY(ImGui::GetContentRegionMax().y - ImGui::GetFrameHeightWithSpacing() - 2.0f);
            ImGui::Separator();
            if (gameWindowSynced) {
                ImGui::TextColored(kGoodGreen, "synced");
                ImGui::SameLine();
                ImGui::TextDisabled("pid %d | entities %d | spectate %s",
                    game.GetMemory()->GetProcessId(), entityCount, game.IsSpectating() ? "on" : "off");
            } else {
                ImGui::TextColored(kBadRed, "no game window - set CS2 Display Mode to Windowed Fullscreen");
            }
        }
        ImGui::End();
        PopMenuTheme();

        if (!showMenu) {
            ApplyMenuState();
        }
    }

    void RenderDebugWindow() {
        if (!showDebugWindow) return;

        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(320, 180), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("Debug", &showDebugWindow)) {
            ImGui::Text("Alive players: %d", entityCount);
            ImGui::Text("Game pid: %d", game.GetMemory()->GetProcessId());
            ImGui::Text("Local team: %d", localTeam);
            ImGui::Text("Drawn: %d", (lastDrawn < 0 ? 0 : lastDrawn));
            ImGui::Text("Skip local/team/dist/off/tiny: %d/%d/%d/%d/%d",
                lastDrawStats.local, lastDrawStats.team, lastDrawStats.dist,
                lastDrawStats.offscreen, lastDrawStats.tiny);
            ImGui::Separator();
            ImGui::Text("Menu open: %s", showMenu ? "yes" : "no");
            ImGui::Text("ESP enabled: %s", settings.enabled ? "yes" : "no");
            ImGui::Text("Game window: %s", gameWindowSynced ? "locked" : "NOT FOUND");
            if (!gameWindowSynced) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                    "Set CS2 Display Mode to Borderless Windowed!");
            }
        }
        ImGui::End();
    }

    void RenderPlayerList(const std::vector<GameEntity>& entities) {
        if (!showPlayerList) return;

        ImGui::SetNextWindowPos(ImVec2(540, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("Players", &showPlayerList)) {
            if (ImGui::BeginTable("plist", 6, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Idx", ImGuiTableColumnFlags_WidthFixed, 36.0f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthFixed, 42.0f);
                ImGui::TableSetupColumn("HP", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("Pos");
                ImGui::TableHeadersRow();

                for (const auto& entity : entities) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", entity.index);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(entity.name.empty() ? "(unknown)" : entity.name.c_str());
                    if (entity.isLocal) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(you)");
                    }

                    ImGui::TableSetColumnIndex(2);
                    ImVec4 col = (localTeam != 0 && entity.team == localTeam) ? settings.teamColor : settings.enemyColor;
                    ImGui::TextColored(col, "%s", entity.GetTeamName().c_str());

                    ImGui::TableSetColumnIndex(3);
                    float hp = (float)entity.health / 100.0f;
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                        ImVec4(1.0f - hp, hp, 0.15f, 1.0f));
                    ImGui::ProgressBar(hp, ImVec2(-FLT_MIN, 0), "");
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.0f, 4.0f);
                    ImGui::Text("%d", entity.health);

                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.0fm", entity.distance);

                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%.0f %.0f %.0f", entity.position.x, entity.position.y, entity.position.z);
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    void RenderESP(const std::vector<GameEntity>& entities) {
        if (!settings.enabled) {
            if (lastDrawn != 0) {
                lastDrawn = 0;
            }
            return;
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)overlay.GetWidth(), (float)overlay.GetHeight()));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoInputs |
                                  ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoBackground;

        ImGui::Begin("##esp", nullptr, flags);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        int drawn = 0;
        DrawStats stats;
        stats.total = static_cast<int>(entities.size());
        const float scrW = (float)overlay.GetWidth();
        const float scrH = (float)overlay.GetHeight();
        const bool spectating = game.IsSpectating();

        /* per-pawn screen-space smoothing. the memory pipeline samples at game
         * tick rate while the overlay presents at monitor rate, so raw
         * projections carry one-tick steps that read as jitter. an exponential
         * lerp toward the fresh target (keyed by pawn address, which survives
         * the 500 ms discovery rebuild) takes that out without lagging. */
        struct smooth_t { float cx = 0.f, top = 0.f, h = 0.f; int stamp = 0; bool init = false; };
        static std::unordered_map<uintptr_t, smooth_t> smoothBoxes;
        static int frameStamp = 0;
        static auto lastFrameTime = std::chrono::steady_clock::now();
        ++frameStamp;

        /* frame-rate independent smoothing: a fixed per-frame factor converges
         * twice as fast at 120 Hz as at 60, so the same overlay felt snappy on
         * one monitor and laggy on another. 26/s is the old 0.35-at-60fps
         * rate, expressed in real time. */
        const auto frameNow = std::chrono::steady_clock::now();
        float frameDt = std::chrono::duration<float>(frameNow - lastFrameTime).count();
        lastFrameTime = frameNow;
        frameDt = (std::max)(0.001f, (std::min)(frameDt, 0.1f));
        const float smoothK = 1.0f - std::exp(-frameDt * 26.0f);
        if ((frameStamp & 0x1FF) == 0) {
            for (auto it = smoothBoxes.begin(); it != smoothBoxes.end();) {
                if (frameStamp - it->second.stamp > 180) it = smoothBoxes.erase(it);
                else ++it;
            }
        }

        for (const auto& entity : entities) {
            /* the camera pawn is detected and named like everyone else, but a
             * box on your own head is noise in first person. the menu toggle
             * turns it back on for spectate/debug work. */
            if (entity.isLocal && !settings.showSelf) { stats.local++; continue; }
            if (settings.teamCheck && !spectating && !game.IsSingleTeamServer() &&
                localTeam != Offsets::TEAM_SPECTATOR && entity.team == localTeam) { stats.team++; continue; }
            if (settings.maxDistance > 0.0f && entity.distance > settings.maxDistance) { stats.dist++; continue; }

            Vector3 headWorld = entity.position;
            headWorld.z += 72.0f;

            Vector2 feet, head;
            if (!game.WorldToScreen(entity.position, feet)) { stats.offscreen++; continue; }
            if (!game.WorldToScreen(headWorld, head)) { stats.offscreen++; continue; }

            float rawH = feet.y - head.y;
            /* never drop a player just because the projection got extreme.
             * point-blank bodies can exceed a full screen height; distant
             * bodies can be one pixel tall. both still get a box, clamped to
             * a sane bound so the draw list cannot explode. */
            if (!std::isfinite(rawH) || rawH < 1.0f) { stats.tiny++; continue; }
            if (rawH > scrH * 4.0f) rawH = scrH * 4.0f;

            const float rawCx = (feet.x + head.x) * 0.5f;

            smooth_t& sm = smoothBoxes[entity.address];
            if (!sm.init) {
                sm.cx = rawCx; sm.top = head.y; sm.h = rawH; sm.init = true;
            } else if (std::fabs(rawCx - sm.cx) > 320.0f || std::fabs(head.y - sm.top) > 320.0f) {
                /* respawn / teleport: snap instead of gliding across the map */
                sm.cx = rawCx; sm.top = head.y; sm.h = rawH;
            } else {
                const float k = smoothK;
                sm.cx += (rawCx - sm.cx) * k;
                sm.top += (head.y - sm.top) * k;
                sm.h += (rawH - sm.h) * k;
            }
            sm.stamp = frameStamp;

            const float cx = sm.cx;
            const float boxH = sm.h;
            const float topY = sm.top;
            const float botY = sm.top + sm.h;

            float boxW = boxH * 0.5f;
            boxW = (std::max)(8.0f, (std::min)(boxW, scrW * 0.5f));

            const bool friendly = (localTeam != 0 && entity.team == localTeam);
            const ImVec4& baseCol = friendly ? settings.teamColor : settings.enemyColor;
            const ImU32 col = ToU32(baseCol);
            const ImU32 outline = ToU32(settings.outlineColor);

            ImVec2 boxMin(cx - boxW * 0.5f, topY);
            ImVec2 boxMax(cx + boxW * 0.5f, botY);

            if (settings.showSnaplines) {
                const ImVec2 from(scrW * 0.5f, scrH);
                const ImVec2 to(cx, botY);
                if (settings.showOutline) {
                    drawList->AddLine(from, to, outline, settings.snaplineThickness + 1.5f);
                }
                drawList->AddLine(from, to, ToU32(settings.snaplineColor), settings.snaplineThickness);
            }

            if (settings.filledBox) {
                drawList->AddRectFilled(boxMin, boxMax, ToU32(baseCol, 0.15f), 0.0f, 0);
            }

            if (settings.showBox) {
                /* corner-accent style: faint full frame + bold corner
                 * brackets — reads cleanly against busy backgrounds */
                const float cw = (std::min)(boxW, boxH) * 0.28f;
                if (settings.showOutline) {
                    drawList->AddRect(ImVec2(boxMin.x - 1, boxMin.y - 1), ImVec2(boxMax.x + 1, boxMax.y + 1), outline, 0.0f, 0, 1.0f);
                }
                drawList->AddRect(boxMin, boxMax, ToU32(baseCol, 0.45f), 0.0f, 0, 1.0f);
                // TL bracket
                drawList->AddLine(ImVec2(boxMin.x, boxMin.y + cw), ImVec2(boxMin.x, boxMin.y), col, settings.boxThickness);
                drawList->AddLine(ImVec2(boxMin.x, boxMin.y), ImVec2(boxMin.x + cw, boxMin.y), col, settings.boxThickness);
                // TR bracket
                drawList->AddLine(ImVec2(boxMax.x - cw, boxMin.y), ImVec2(boxMax.x, boxMin.y), col, settings.boxThickness);
                drawList->AddLine(ImVec2(boxMax.x, boxMin.y), ImVec2(boxMax.x, boxMin.y + cw), col, settings.boxThickness);
                // BL bracket
                drawList->AddLine(ImVec2(boxMin.x, boxMax.y - cw), ImVec2(boxMin.x, boxMax.y), col, settings.boxThickness);
                drawList->AddLine(ImVec2(boxMin.x, boxMax.y), ImVec2(boxMin.x + cw, boxMax.y), col, settings.boxThickness);
                // BR bracket
                drawList->AddLine(ImVec2(boxMax.x - cw, boxMax.y), ImVec2(boxMax.x, boxMax.y), col, settings.boxThickness);
                drawList->AddLine(ImVec2(boxMax.x, boxMax.y), ImVec2(boxMax.x, boxMax.y - cw), col, settings.boxThickness);
            }

            if (settings.showHealthBar) {
                float hp = (float)entity.health / 100.0f;
                hp = (std::max)(0.0f, (std::min)(1.0f, hp));
                const float barW = 3.0f;
                const float pad = 3.0f;
                ImVec2 hbMin(boxMin.x - pad - barW, boxMin.y);
                ImVec2 hbMax(boxMin.x - pad, boxMax.y);

                drawList->AddRectFilled(ImVec2(hbMin.x - 1, hbMin.y - 1), ImVec2(hbMax.x + 1, hbMax.y + 1), outline);
                drawList->AddRectFilled(hbMin, hbMax, IM_COL32(18, 18, 22, 210));

                ImVec2 fillMin(hbMin.x, hbMax.y - (boxH * hp));
                ImVec2 fillMax(hbMax.x, hbMax.y);
                const ImU32 hpCol = HpColor(hp);
                const ImU32 hpDim = DimCol(hpCol, 0.55f);
                /* vertical gradient: dimmed at the top of the fill, bright at
                 * the bottom — subtle depth, no extra outline pass */
                drawList->AddRectFilledMultiColor(fillMin, fillMax, hpDim, hpDim, hpCol, hpCol);

                if (settings.showHealthText) {
                    char hpTxt[16];
                    sprintf_s(hpTxt, "%d", entity.health);
                    ImVec2 ts = ImGui::CalcTextSize(hpTxt);
                    ImVec2 tp(hbMin.x - ts.x - 3.0f, fillMin.y - ts.y * 0.5f);
                    drawList->AddText(ImVec2(tp.x + 1, tp.y + 1), outline, hpTxt);
                    drawList->AddText(tp, ToU32(settings.textColor), hpTxt);
                }
            }

            // name line above the box. every discovered pawn gets a label,
            // even if its controller name has not resolved yet.
            if (settings.showNames) {
                const std::string label = entity.name.empty()
                    ? ("#" + std::to_string(entity.index))
                    : entity.name;
                ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                ImVec2 tp(cx - ts.x * 0.5f, boxMin.y - ts.y - 4.0f);
                drawList->AddText(ImVec2(tp.x + 1, tp.y + 1), outline, label.c_str());
                drawList->AddText(tp, ToU32(settings.textColor), label.c_str());
            }

            // team + distance line below the box, tinted by team color
            {
                std::string meta;
                if (settings.showTeamName) {
                    meta += friendly ? "TEAM " : "ENEMY ";
                    meta += entity.GetTeamName();
                }
                if (settings.showDistance) {
                    if (!meta.empty()) meta += " | ";
                    char dbuf[32];
                    sprintf_s(dbuf, "%.0fm", entity.distance);
                    meta += dbuf;
                }
                if (!meta.empty()) {
                    ImVec2 ts = ImGui::CalcTextSize(meta.c_str());
                    ImVec2 tp(cx - ts.x * 0.5f, boxMax.y + 4.0f);
                    drawList->AddText(ImVec2(tp.x + 1, tp.y + 1), outline, meta.c_str());
                    drawList->AddText(tp, ToU32(baseCol, 0.92f), meta.c_str());
                }
            }

            drawn++;
            stats.drawn++;
        }

        lastDrawn = drawn;
        lastDrawStats = stats;

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }

    void Run() {
        /* keep the present cadence stable while CS2 saturates the CPU (map
         * loads, smokes, 20-player fights). above-normal is enough to win the
         * occasional scheduling round without fighting the game. */
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

        while (true) {
            // END exits
            if (GetAsyncKeyState(VK_END) & 0x8000) {
                break;
            }

            // INSERT toggles menu
            static bool insertPressed = false;
            if (GetAsyncKeyState(VK_INSERT) & 0x8000) {
                if (!insertPressed) {
                    ToggleMenu();
                    insertPressed = true;
                }
            } else {
                insertPressed = false;
            }

            // F1 toggles debug window
            static bool f1Pressed = false;
            if (GetAsyncKeyState(VK_F1) & 0x8000) {
                if (!f1Pressed) {
                    showDebugWindow = !showDebugWindow;
                    f1Pressed = true;
                }
            } else {
                f1Pressed = false;
            }

            // F2 toggles ESP
            static bool f2Pressed = false;
            if (GetAsyncKeyState(VK_F2) & 0x8000) {
                if (!f2Pressed) {
                    settings.enabled = !settings.enabled;
                    f2Pressed = true;
                }
            } else {
                f2Pressed = false;
            }

            if (!overlay.ProcessMessages()) break;

            bool size_changed = false;
            gameWindowSynced = overlay.SyncToGameWindow(&size_changed);
            if (size_changed) {
                renderer.Resize(overlay.GetWidth(), overlay.GetHeight());
                Log("[+] Overlay resized to " + std::to_string(overlay.GetWidth()) + "x" + std::to_string(overlay.GetHeight()));
            }

            if (!gameWindowSynced) {
                /* game window is not in front or not found: the overlay has
                 * parked itself. nothing is presented while parked, but keep a
                 * slow memory pass going so the controller registry and name
                 * bindings stay warm instead of going stale every time the
                 * window loses focus. */
                static auto lastParkedPass = std::chrono::steady_clock::now();
                const auto parkedNow = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(parkedNow - lastParkedPass).count() >= 1000) {
                    lastParkedPass = parkedNow;
                    game.Update();
                    (void)game.GetEntities();
                }
                Sleep(16);
                continue;
            }

            game.SetScreenSize(overlay.GetWidth(), overlay.GetHeight());
            game.Update();
            auto entities = game.GetEntities();
            entityCount = (int)entities.size();
            localTeam = game.GetLocalTeam();

            /* draw-path telemetry — the log tells us exactly what the frame
             * pipeline sees, every ~2s */
            static int telemetryFrame = 0;
            if ((telemetryFrame++ % 120) == 0) {
                int projected = 0;
                for (const auto& e : entities) {
                    Vector2 s;
                    if (game.WorldToScreen(e.position, s)) projected++;
                }
                Log("[draw] entities=" + std::to_string(entityCount) +
                    " projected=" + std::to_string(projected) +
                    " drawn=" + std::to_string(lastDrawStats.drawn) +
                    " skip[local=" + std::to_string(lastDrawStats.local) +
                    " team=" + std::to_string(lastDrawStats.team) +
                    " dist=" + std::to_string(lastDrawStats.dist) +
                    " off=" + std::to_string(lastDrawStats.offscreen) +
                    " tiny=" + std::to_string(lastDrawStats.tiny) + "]" +
                    " menu=" + (showMenu ? "1" : "0") +
                    " esp=" + (settings.enabled ? "1" : "0") +
                    " synced=" + (gameWindowSynced ? "1" : "0") +
                    " local=" + std::to_string(localTeam) +
                    " spectate=" + (game.IsSpectating() ? "1" : "0") +
                    " single=" + (game.IsSingleTeamServer() ? "1" : "0"));

                /* one line naming what is actually on screen, so a wrong side
                 * can be diagnosed without screenshots */
                std::string roster = "[draw] list:";
                int shown = 0;
                for (const auto& e : entities) {
                    if (shown++ >= 24) { roster += " ..."; break; }
                    roster += " t" + std::to_string(e.team);
                    if (e.isLocal) roster += "(you)";
                    roster += " idx" + std::to_string(e.index);
                    if (!e.name.empty()) roster += " '" + e.name + "'";
                    roster += " " + std::to_string(static_cast<int>(e.distance)) + "m";
                }
                if (entities.empty()) roster += " (none)";
                Log(roster);
            }

            /* ALWAYS render every frame — keybinds flip flags, and flags only
             * mean anything if a frame actually draws them. ESP waits for a
             * locked game window; the menu draws no matter what. Present
             * paces to vsync, so no artificial sleep on any path. */
            renderer.BeginFrame();
            if (gameWindowSynced) RenderESP(entities);
            RenderMenu();
            RenderDebugWindow();
            RenderPlayerList(entities);
            renderer.EndFrame();

            // No artificial sleep: Present(1,0) paces to vsync. Yield lightly only if minimized/occluded paths occur.
        }
    }

    void Shutdown() {
        renderer.Shutdown();
        overlay.Shutdown();
        game.Cleanup();
    }
};

int main() {
    /* background mode: no console window exists. helper_debug.log is the
     * diagnostic surface, and INSERT/F2/END keep working once the game is up. */
    Log("[*] boot (background)");
    SetProcessDPIAware();

    CS2ESP esp;

    /* resident helper: if the game is not up yet, wait for it instead of
     * dying. this is what lets the exe be started once and forgotten. END
     * still exits while waiting. */
    bool announced_wait = false;
    while (!esp.Initialize(false)) {
        if (!announced_wait) {
            Log("[*] cs2 not available - waiting in background");
            announced_wait = true;
        }
        for (int tick = 0; tick < 25; tick++) {
            if (GetAsyncKeyState(VK_END) & 0x8000) return 0;
            Sleep(200);
        }
    }

    Log("[+] overlay armed");
    esp.Run();
    esp.Shutdown();

    return 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam)) {
        return true;
    }

    switch (uMsg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
