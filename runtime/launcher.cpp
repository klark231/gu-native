// The game folder screen on a desktop (runtime/gamefolder.c opens it when no folder is known, or the one known can't be
// played): Android's GameFolderActivity, in the settings menu's Hunter look, in a window of its own before the game's.
// Dear ImGui with its SDL3 and SDL_Renderer backends (third_party/cimgui/imgui/backends), so it needs nothing of the
// game's Vulkan renderer. Mouse, keyboard and controller work it.
#ifndef __ANDROID__
#include <SDL3/SDL.h>

#include <atomic>
#include <string>
#include <thread>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include "gamefolder.h"
#include "settings_locale.h"

namespace {
// the settings menu's Hunter theme (settings_panel.cpp): wine, the game's red header, gold
const ImU32 PANEL = IM_COL32(50, 25, 24, 255), HEADER = IM_COL32(108, 46, 46, 255), EDGE = IM_COL32(0, 0, 0, 255),
            GOLD = IM_COL32(236, 212, 44, 255), GOLD_BRIGHT = IM_COL32(255, 232, 84, 255), TEXT = IM_COL32(242, 238, 232, 255),
            DIM = IM_COL32(196, 176, 166, 255), WARN = IM_COL32(255, 96, 72, 255), BRONZE = IM_COL32(96, 58, 34, 255),
            BRONZE_LIT = IM_COL32(150, 98, 46, 255), RED_RULE = IM_COL32(176, 34, 30, 255), GREEN_RULE = IM_COL32(80, 142, 54, 255);
int lang;
const char *T(const char *en) { return settings_text(lang, en); }

struct Pick {
    std::atomic<int> state{0};  // 0 idle, 1 the dialog is open, 2 a folder came back, 3 checking, 4 checked
    std::string picked, utf8, native, why;
} P;

void SDLCALL picked(void *, const char *const *list, int) {
    if (list && list[0]) P.picked = list[0], P.state = 2;
    else P.state = 0;  // (cancelled)
}

// a button with the game's look: bronze, a gold edge, gold when it has the focus or the mouse
bool button(const char *label, ImVec2 size, bool primary) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(primary ? BRONZE_LIT : BRONZE));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(IM_COL32(176, 145, 24, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(GOLD));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(GOLD));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.f);
    bool hit = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return hit;
}
}  // namespace

// 1: a folder that can be played was chosen (native: the runtime's spelling of it; it is kept in game_folder.txt),
// 0: the player left, -1: no window could be made (the caller asks with the system's boxes instead)
extern "C" int launcher_choose_folder(const char *problem, char *native, size_t native_size) {
    lang = gamefolder_language();
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale < 1.f) scale = 1.f;
    SDL_Window *win = SDL_CreateWindow("MHGU", (int)(880 * scale), (int)(600 * scale), SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) return -1;
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) {
        SDL_DestroyWindow(win);
        return -1;
    }
    SDL_SetRenderVSync(ren, 1);
    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL, io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 0, st.FrameRounding = 3, st.WindowBorderSize = 0, st.FramePadding = ImVec2(14, 8);
    st.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(TEXT);
    st.Colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(PANEL);
    st.Colors[ImGuiCol_NavCursor] = ImGui::ColorConvertU32ToFloat4(GOLD_BRIGHT);
    st.ScaleAllSizes(scale);
    st.FontScaleMain = 1.4f * scale;
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    std::string status = problem && *problem ? problem : "";
    bool status_bad = !status.empty();
    int result = 0;
    bool running = true;
    std::thread checker;
    // (tests: MHGU_LAUNCHER_SHOT=<file.png> saves the screen after a moment and leaves; MHGU_LAUNCHER_PICK=<folder> answers
    // the folder dialog with that folder)
    const char *test_shot = SDL_getenv("MHGU_LAUNCHER_SHOT"), *test_pick = SDL_getenv("MHGU_LAUNCHER_PICK");
    int frame = 0;
    while (running) {
        if (test_pick && *test_pick && frame == 20) P.picked = test_pick, P.state = 2;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win)))
                running = false;
            // Esc, or a controller's B, is Exit, as a dialog's Cancel (nothing is lost: the game asks again when opened).
            // B by its label, at the right of the face buttons (an Xbox controller's B, a PlayStation's circle): there
            // ImGui takes it for its own Back, where a Switch controller has its A. Not while the system's folder dialog
            // is open, whose Esc is its own
            bool esc = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_ESCAPE;
            bool back = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST;
            if (back) {
                SDL_GamepadButtonLabel label =
                    SDL_GetGamepadButtonLabelForType(SDL_GetGamepadTypeForID(e.gbutton.which), SDL_GAMEPAD_BUTTON_EAST);
                back = label == SDL_GAMEPAD_BUTTON_LABEL_B || label == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE;
            }
            if ((esc || back) && P.state != 1)
                running = false;
        }
        if (P.state == 2) {  // (a folder came back from the dialog: checked away from this loop, romfs is 35,531 files)
            P.state = 3;
            status = T("Checking the files..."), status_bad = false;
            if (checker.joinable()) checker.join();
            checker = std::thread([] {
                char utf8[1100], nat[1100], why[700];
                bool ok = gamefolder_check_picked(P.picked.c_str(), lang, utf8, sizeof utf8, nat, sizeof nat, why, sizeof why);
                P.utf8 = utf8, P.native = nat, P.why = ok ? "" : why;
                P.state = 4;
            });
        }
        if (P.state == 4) {
            P.state = 0;
            if (P.why.empty()) {
                gamefolder_remember(P.utf8.c_str());
                SDL_strlcpy(native, P.native.c_str(), native_size);
                result = 1, running = false;
            } else {
                status = P.why, status_bad = true;
            }
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        float f = ImGui::GetFontSize(), band = f * 3.2f, pad = f * 1.2f, W = vp->Size.x;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));  // (the text's margins, all of it)
        ImGui::Begin("##folder", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        // the header: the game's red band and its red and green rules, a gold title
        dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + W, vp->Pos.y + band), HEADER);
        dl->AddRectFilled(ImVec2(vp->Pos.x, vp->Pos.y + band), ImVec2(vp->Pos.x + W, vp->Pos.y + band + 3 * scale), RED_RULE);
        dl->AddRectFilled(ImVec2(vp->Pos.x, vp->Pos.y + band + 3 * scale), ImVec2(vp->Pos.x + W, vp->Pos.y + band + 5 * scale), GREEN_RULE);
        dl->AddText(ImVec2(vp->Pos.x + pad, vp->Pos.y + (band - f) * .5f), GOLD, T("MHGU: YOUR GAME FOLDER"));
        ImGui::SetCursorPos(ImVec2(pad, band + pad));
        ImGui::PushTextWrapPos(W - pad);
        ImGui::TextUnformatted(T("Choose the folder with your own copy of the game. It must hold:"));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Indent(pad);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(GOLD));
        ImGui::TextUnformatted("MHGU");
        ImGui::PopStyleColor();
        const char *tree[][2] = {{"|- exefs", "the game's exefs, version 1.4.0"},
                                 {"|- romfs", "its romfs with the 1.4.0 update applied (35,531 files)"},
                                 {"|- mhgugame.dll", "the game code for PC"},
                                 {"'- mhgugame.id", ""}};
        for (auto &t : tree) {
            ImGui::TextUnformatted(t[0]);
            if (*t[1]) {
                ImGui::SameLine(pad * 2 + ImGui::CalcTextSize("|- mhgugame.dll").x + f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(DIM));
                ImGui::TextUnformatted(T(t[1]));
                ImGui::PopStyleColor();
            }
        }
        ImGui::Unindent(pad);
        ImGui::Spacing();
        ImGui::PushTextWrapPos(W - pad);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(DIM));
        ImGui::TextUnformatted(T("mhgugame.dll and mhgugame.id are made once on this PC from your own files with the guide's tool, the "
                                 "same run that makes the phone's: python build_game.py <your folder>. Nothing is copied: the game reads "
                                 "that folder."));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (!status.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(status_bad ? WARN : GOLD));
            ImGui::TextUnformatted(status.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        // the buttons at the bottom
        float bh = f * 2.2f, bw = (W - pad * 3) * .5f, by = vp->Size.y - pad - bh;
        ImGui::SetCursorPos(ImVec2(pad, by));
        bool busy = P.state != 0;
        ImGui::BeginDisabled(busy);
        if (button(T("Choose folder"), ImVec2(bw, bh), true) && !busy) {
            P.state = 1;
            char start[1100];
            gamefolder_saved(start, sizeof start);
            SDL_ShowOpenFolderDialog(picked, NULL, win, start[0] ? start : NULL, false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine(pad * 2 + bw);
        if (button(T("Exit"), ImVec2(bw, bh), false)) running = false;
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 50, 25, 24, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (test_shot && *test_shot && ++frame == 45) {
            SDL_Surface *s = SDL_RenderReadPixels(ren, NULL);
            if (s) SDL_SavePNG(s, test_shot), SDL_DestroySurface(s);
            running = false;
        } else if (!(test_shot && *test_shot)) {
            ++frame;
        }
        SDL_RenderPresent(ren);
    }
    if (checker.joinable()) checker.join();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(previous);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    return result;
}
#endif
