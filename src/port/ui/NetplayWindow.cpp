#include "NetplayWindow.h"

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <string>
#include <vector>

#include <SDL2/SDL.h>

#include "PortMenu.h"
#include "UIWidgets.h"
#include "port/Game.h"
#include "port/GamepadAssignment.h"
#include "engine/World.h"
#include "port/netplay/KartSmoothing.h"
#include "port/netplay/Lobby.h"
#include "port/netplay/NetSession.h"
#include "port/netplay/Netplay.h"
#include "port/netplay/RoomCodeInput.h"
#include "port/netplay/VoiceChat.h"

namespace GameUI {
extern std::shared_ptr<PortMenu> mPortMenu;

namespace {

constexpr const char* kWindowName = "Netplay";
constexpr const char* kWindowCVar = "gNetplay.WindowOpen";
using Lobby::kDefaultRelay;
using Lobby::kNameCVar;
using Lobby::kRelayCVar;
constexpr const char* kCharacterNames[] = { "Mario", "Luigi", "Yoshi", "Toad", "Donkey Kong", "Wario", "Peach", "Bowser" };
constexpr const char* kCCNames[] = { "50cc", "100cc", "150cc", "Extra (mirrored)" };
const ImVec4 kErrorColor = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
const ImVec4 kDimColor = ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
const ImVec4 kTalkingColor = ImVec4(0.45f, 1.0f, 0.45f, 1.0f);

// Joining by code: the directions entered so far, and whether controller 1 is entering them (Netplay::SetPadCapture())
std::string sCodeDraft;
bool sCapturingCode = false;
RoomCodeInput::Reader sCodeReader;
constexpr f32 kArrowCellSize = 22.0f;

struct TrackChoice {
    std::string ResourceName;
    std::string Name;
};

const char* GetModeName(s32 mode) {
    switch (mode) {
        case GRAND_PRIX:
            return "Grand Prix";
        case BATTLE:
            return "Battle";
        default:
            return "Versus";
    }
}

Cup* GetBattleCup() {
    return GetWorld()->Cups.back();
}

/** Race tracks for Versus, or arenas for Battle, in the track browser's order. */
std::vector<TrackChoice> GetTrackChoices(s32 mode) {
    const std::vector<std::string>& arenas = GetBattleCup()->mTracks;
    std::vector<const TrackInfo*> tracks = gTrackRegistry.GetAllInfo();
    std::sort(tracks.begin(), tracks.end(), [](const TrackInfo* a, const TrackInfo* b) { return a->Id < b->Id; });
    std::vector<TrackChoice> choices;
    for (const TrackInfo* track : tracks) {
        const bool isArena = std::find(arenas.begin(), arenas.end(), track->ResourceName) != arenas.end();
        if ((track->ResourceName != "mk:podium_ceremony") && (isArena == (mode == BATTLE))) {
            choices.push_back({ track->ResourceName, track->Name });
        }
    }
    return choices;
}

std::string GetMemberName(const Lobby::Member& member, size_t slot) {
    return member.Name.empty() ? "Player " + std::to_string(slot + 1) : member.Name;
}

void InputCVarString(const char* label, const char* cvar, const char* fallback, size_t maxLength) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s", CVarGetString(cvar, fallback));
    if (ImGui::InputText(label, buffer, std::min(sizeof(buffer), maxLength + 1))) {
        CVarSetString(cvar, buffer);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
}

std::string GetPlayerName() {
    const std::string name = CVarGetString(kNameCVar, "");
    return name.empty() ? "Player" : name;
}

/** One direction of a room code, as an arrow in a cell at the cursor; an empty cell for 0. */
void DrawCodeCell(char letter, bool next) {
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    const ImVec2 size(kArrowCellSize, kArrowCellSize);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 frame = ImGui::GetColorU32(next ? ImGuiCol_Text : ImGuiCol_Border);
    draw->AddRect(corner, ImVec2(corner.x + size.x, corner.y + size.y), frame, 3.0f);
    const ImVec2 c(corner.x + size.x / 2.0f, corner.y + size.y / 2.0f);
    const f32 r = size.x * 0.32f;
    const ImU32 color = ImGui::GetColorU32(UIWidgets::ColorValues.at(UIWidgets::Colors::Yellow));
    switch (letter) {
        case 'U':
            draw->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(c.x - r, c.y + r), color);
            break;
        case 'D':
            draw->AddTriangleFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y - r), ImVec2(c.x, c.y + r), color);
            break;
        case 'L':
            draw->AddTriangleFilled(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y - r), ImVec2(c.x + r, c.y + r), color);
            break;
        case 'R':
            draw->AddTriangleFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x - r, c.y + r), color);
            break;
        default:
            break;
    }
    ImGui::Dummy(size);
}

/** A room code as a row of arrows: the default font has none. Unfilled cells are empty; the next one is outlined. */
void DrawCodeCells(const std::string& code, bool showNext) {
    for (size_t i = 0; i < RelayProtocol::kCodeLength; i++) {
        if (i > 0) {
            ImGui::SameLine(0.0f, 4.0f);
        }
        DrawCodeCell((i < code.size()) ? code[i] : 0, showNext && (i == code.size()));
    }
}

/** A room code as text with arrows, for pasting into a chat. */
std::string GetCodeText(const std::string& code) {
    std::string text;
    for (char letter : code) {
        text += (letter == 'U') ? "\u2191" : (letter == 'D') ? "\u2193" : (letter == 'L') ? "\u2190" : "\u2192";
    }
    return text;
}

void StopCodeCapture() {
    sCapturingCode = false;
    Netplay::SetPadCapture(false);
}

/** Adds an entry to the code being typed; the seventh direction joins the room. */
void EnterCode(RoomCodeInput::Entry entry, const std::string& relay) {
    if (!RoomCodeInput::Apply(entry, &sCodeDraft) || (sCodeDraft.size() < RelayProtocol::kCodeLength)) {
        return;
    }
    StopCodeCapture();
    Lobby::Join(relay, sCodeDraft, GetPlayerName());
    sCodeDraft.clear();
}

/** While capturing: controller 1 (through the game) and the keyboard's arrow keys (through the window) type. */
void ReadCodeCapture(const std::string& relay) {
    s8 stickX;
    s8 stickY;
    u16 pressed;
    Netplay::TakeCapturedPad(&stickX, &stickY, &pressed);
    EnterCode(sCodeReader.Read(stickX, stickY, pressed), relay);
    const std::pair<ImGuiKey, RoomCodeInput::Entry> keys[] = {
        { ImGuiKey_UpArrow, RoomCodeInput::Entry::Up },     { ImGuiKey_DownArrow, RoomCodeInput::Entry::Down },
        { ImGuiKey_LeftArrow, RoomCodeInput::Entry::Left }, { ImGuiKey_RightArrow, RoomCodeInput::Entry::Right },
        { ImGuiKey_Backspace, RoomCodeInput::Entry::Erase },
    };
    for (const auto& [key, entry] : keys) {
        if (sCapturingCode && ImGui::IsKeyPressed(key, false)) {
            EnterCode(entry, relay);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        StopCodeCapture();
    }
}

void DrawJoinByCode(const std::string& relay) {
    ImGui::TextUnformatted("Join a room by its code:");
    DrawCodeCells(sCodeDraft, true);
    if (sCapturingCode) {
        ReadCodeCapture(relay);
    }
    const std::pair<ImGuiDir, RoomCodeInput::Entry> buttons[] = {
        { ImGuiDir_Up, RoomCodeInput::Entry::Up },     { ImGuiDir_Down, RoomCodeInput::Entry::Down },
        { ImGuiDir_Left, RoomCodeInput::Entry::Left }, { ImGuiDir_Right, RoomCodeInput::Entry::Right },
    };
    for (const auto& [direction, entry] : buttons) {
        ImGui::PushID((int) direction);
        if (ImGui::ArrowButton("##direction", direction)) {
            EnterCode(entry, relay);
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(sCodeDraft.empty());
    if (ImGui::Button("Erase")) {
        EnterCode(RoomCodeInput::Entry::Erase, relay);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(sCapturingCode ? "Stop" : "Enter with controller")) {
        if (sCapturingCode) {
            StopCodeCapture();
        } else {
            sCapturingCode = true;
            sCodeReader.Reset();
            Netplay::SetPadCapture(true);
        }
    }
    if (sCapturingCode) {
        ImGui::TextColored(kDimColor, "Flick the stick, or press the D-pad, C buttons or arrow keys.");
        ImGui::TextColored(kDimColor, "B or Backspace erases, Esc stops.");
    }
}

void DrawConnect() {
    InputCVarString("Your name", kNameCVar, "Player", Lobby::kMaxNameLength);
    InputCVarString("Relay server", kRelayCVar, kDefaultRelay, 63);
    const std::string relay = CVarGetString(kRelayCVar, kDefaultRelay);
    ImGui::Spacing();

    if (ImGui::Button("Quick match")) {
        StopCodeCapture();
        Lobby::Matchmake(relay, GetPlayerName());
    }
    ImGui::SameLine();
    ImGui::TextColored(kDimColor, "Keep playing while it looks for other players");
    if (ImGui::Button("Host a public room")) {
        StopCodeCapture();
        Lobby::Host(relay, GetPlayerName(), true, Lobby::Visibility::Public);
    }
    ImGui::SameLine();
    if (ImGui::Button("Host a private room")) {
        StopCodeCapture();
        Lobby::Host(relay, GetPlayerName(), true, Lobby::Visibility::Private);
    }
    ImGui::Spacing();
    DrawJoinByCode(relay);

    if (!Lobby::GetError().empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kErrorColor, "%s", Lobby::GetError().c_str());
    }
}

void DrawMatchmaking() {
    const u32 waiting = Lobby::GetWaitingPlayers();
    if (waiting > 1) {
        ImGui::Text("Looking for a match: %u players looking...", waiting);
    } else {
        ImGui::TextUnformatted("Looking for a match...");
    }
    ImGui::TextColored(kDimColor, "Keep playing: a match takes you into the room once you are in the menus.");
    if (ImGui::Button("Cancel")) {
        Lobby::Leave();
    }
}

/** This player's side of voice chat, in a few words. */
const char* GetMicStatusText(VoiceChat::MicStatus status) {
    switch (status) {
        case VoiceChat::MicStatus::Off:
            return "off";
        case VoiceChat::MicStatus::Unavailable:
            return "not on this system";
        case VoiceChat::MicStatus::Idle:
            return "nobody to talk to";
        case VoiceChat::MicStatus::Starting:
            return "starting";
        case VoiceChat::MicStatus::Live:
            return "on";
        case VoiceChat::MicStatus::Muted:
            return "microphone muted";
        case VoiceChat::MicStatus::Denied:
            return "microphone not allowed";
        default:
            return "microphone failed";
    }
}

/** The voice chat cell of a member's row: for another member, whether they are heard, and a way to mute them. */
void DrawMemberVoice(u8 slot, bool self) {
    if (self) {
        const VoiceChat::MicStatus status = VoiceChat::GetMicStatus();
        if (VoiceChat::IsTalking()) {
            ImGui::TextColored(kTalkingColor, "talking");
        } else if (status == VoiceChat::MicStatus::Live) {
            ImGui::TextUnformatted(GetMicStatusText(status));
        } else {
            ImGui::TextColored(kDimColor, "%s", GetMicStatusText(status));
        }
        return;
    }
    const VoiceChat::MemberStatus status = VoiceChat::GetMemberStatus(slot);
    switch (status) {
        case VoiceChat::MemberStatus::None:
            return;
        case VoiceChat::MemberStatus::VoiceOff:
            ImGui::TextColored(kDimColor, "off");
            return;
        case VoiceChat::MemberStatus::NotDirect:
            ImGui::TextColored(kDimColor, "no direct connection");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Voice only goes straight between two players, never through the relay server.\n"
                                  "Your routers do not let you and this player connect directly.");
            }
            return;
        default:
            break;
    }
    bool muted = VoiceChat::IsMuted(slot);
    ImGui::PushID(slot);
    if (ImGui::Checkbox("mute", &muted)) {
        VoiceChat::SetMuted(slot, muted);
    }
    ImGui::PopID();
    if (status == VoiceChat::MemberStatus::Talking) {
        ImGui::SameLine();
        ImGui::TextColored(kTalkingColor, "talking");
    }
}

void DrawMembers() {
    const auto& members = Lobby::GetMembers();
    if (!ImGui::BeginTable("members", 5, ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn("Player");
    ImGui::TableSetupColumn("Character");
    ImGui::TableSetupColumn("Status");
    ImGui::TableSetupColumn("Connection");
    ImGui::TableSetupColumn("Voice");
    ImGui::TableHeadersRow();
    for (size_t slot = 0; slot < members.size(); slot++) {
        const Lobby::Member& member = members[slot];
        if (!member.Present) {
            continue;
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool self = (s32) slot == Lobby::GetLocalSlot();
        ImGui::Text("%s%s", GetMemberName(member, slot).c_str(), self ? " (you)" : "");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(member.Known ? kCharacterNames[member.Character] : "...");
        ImGui::TableNextColumn();
        if (Lobby::IsSharedConsole()) {
            const s32 player = Netplay::GetConsolePlayer((u8) slot);
            if (!member.Known) {
                ImGui::TextColored(kDimColor, "Connecting");
            } else if (player < 0) {
                ImGui::TextColored(kDimColor, "Joins after the race");
            } else {
                ImGui::Text("Controller %d%s", player + 1, member.Racing ? ", racing" : "");
            }
        } else if (slot == 0) {
            ImGui::TextUnformatted(member.InRace ? "Host, racing" : "Host");
        } else if (!member.Known) {
            ImGui::TextColored(kDimColor, "Connecting");
        } else {
            ImGui::TextUnformatted(member.InRace ? "Racing" : (member.Ready ? "Ready" : "Not ready"));
        }
        ImGui::TableNextColumn();
        // Your own row stays empty: your ping to the relay server is shown above the table, and on this row it read
        // as if you were connected through the relay.
        if (self) {
        } else if (Lobby::IsDirect((u8) slot)) {
            const s32 ping = Lobby::GetDirectPingMs((u8) slot);
            if (ping >= 0) {
                ImGui::Text("direct %d ms", ping);
            } else {
                ImGui::TextUnformatted("direct");
            }
        } else if (member.Known) {
            ImGui::TextColored(kDimColor, "relayed");
        }
        ImGui::TableNextColumn();
        DrawMemberVoice((u8) slot, self);
    }
    ImGui::EndTable();
}

void DrawCharacterChoice(bool locked) {
    const auto& members = Lobby::GetMembers();
    const s8 current = members[Lobby::GetLocalSlot()].Character;
    ImGui::BeginDisabled(locked);
    if (ImGui::BeginCombo("Character", kCharacterNames[current])) {
        for (s8 character = 0; character < (s8) IM_ARRAYSIZE(kCharacterNames); character++) {
            bool taken = false;
            for (size_t slot = 0; slot < members.size(); slot++) {
                taken |= members[slot].Present && members[slot].Known && ((s32) slot != Lobby::GetLocalSlot()) &&
                         (members[slot].Character == character);
            }
            if (ImGui::Selectable(kCharacterNames[character], character == current,
                                  taken ? ImGuiSelectableFlags_Disabled : 0)) {
                Lobby::SetCharacter(character);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}

void DrawTrackCombo(Lobby::Settings* settings) {
    const std::vector<TrackChoice> choices = GetTrackChoices(settings->Mode);
    const char* current = settings->TrackName.c_str();
    for (const TrackChoice& choice : choices) {
        if (choice.ResourceName == settings->TrackName) {
            current = choice.Name.c_str();
        }
    }
    if (ImGui::BeginCombo(settings->Mode == BATTLE ? "Arena" : "Track", current)) {
        for (const TrackChoice& choice : choices) {
            if (ImGui::Selectable(choice.Name.c_str(), choice.ResourceName == settings->TrackName)) {
                settings->TrackName = choice.ResourceName;
            }
        }
        ImGui::EndCombo();
    }
}

void DrawSettings(bool editable) {
    Lobby::Settings settings = Lobby::GetSettings();
    const Lobby::Settings before = settings;
    ImGui::BeginDisabled(!editable);

    if (ImGui::BeginCombo("Mode", GetModeName(settings.Mode))) {
        for (s32 mode : { GRAND_PRIX, VERSUS, BATTLE }) {
            if (ImGui::Selectable(GetModeName(mode), mode == settings.Mode) && (mode != settings.Mode)) {
                settings.Mode = mode;
                const std::vector<TrackChoice> choices = GetTrackChoices(mode);
                settings.TrackName = choices.empty() ? "" : choices.front().ResourceName;
            }
        }
        ImGui::EndCombo();
    }
    if (settings.Mode != BATTLE) {
        ImGui::Combo("Speed", &settings.CC, kCCNames, IM_ARRAYSIZE(kCCNames));
    }
    if (settings.Mode == GRAND_PRIX) {
        const std::vector<Cup*>& cups = GetWorld()->Cups;
        const size_t numGrandPrixCups = cups.size() - 1; // The battle cup is last
        const s32 cup = std::min(settings.CupIndex, (s32) numGrandPrixCups - 1);
        if (ImGui::BeginCombo("Cup", cups[cup]->Name)) {
            for (size_t i = 0; i < numGrandPrixCups; i++) {
                if (ImGui::Selectable(cups[i]->Name, (s32) i == settings.CupIndex)) {
                    settings.CupIndex = (s32) i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextColored(kDimColor, "All four courses, then the podium.");
    } else {
        DrawTrackCombo(&settings);
    }
    ImGui::EndDisabled();

    if (editable && ((settings.Mode != before.Mode) || (settings.CC != before.CC) ||
                     (settings.CupIndex != before.CupIndex) || (settings.TrackName != before.TrackName))) {
        Lobby::SetSettings(settings);
    }
    if (editable) {
        ImGui::TextColored(kDimColor, "Your gameplay enhancements and rules apply to everyone.");
    }
}

/** A room playing on a shared console: everything is picked in the game's own menus. */
void DrawSharedConsole(bool racing, bool host) {
    const s32 player = Netplay::GetConsolePlayer((u8) Lobby::GetLocalSlot());
    if (player >= 0) {
        ImGui::TextWrapped("Everyone plays on one console, as if your controllers were plugged into it: pick the "
                           "mode, characters and course in the game's menus. You are controller %d.", player + 1);
    } else {
        ImGui::TextWrapped("You join the console when the race in progress is over.");
    }
    if (racing) {
        if (ImGui::Button("Leave race")) {
            Netplay::LeaveRace();
        }
        ImGui::SameLine();
        ImGui::TextColored(kDimColor, "Everyone goes back to the menus.");
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button(host ? "Close room" : "Leave room")) {
        Lobby::Leave();
    }
}

void DrawRoom() {
    const std::string code = Lobby::GetRoomCode();
    ImGui::AlignTextToFramePadding();
    ImGui::Text(Lobby::IsRoomPrivate() ? "Private room, code:" : "Room code:");
    ImGui::SameLine();
    DrawCodeCells(code, false);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        ImGui::SetClipboardText(GetCodeText(code).c_str());
    }
    if (Lobby::GetPingMs() >= 0) {
        ImGui::SameLine();
        ImGui::TextColored(kDimColor, "  relay server ping %d ms", Lobby::GetPingMs());
    }
    ImGui::Separator();
    DrawMembers();
    ImGui::Separator();

    const bool racing = Netplay::IsRaceRunning();
    const bool host = Lobby::IsHost();
    if (Lobby::IsSharedConsole()) {
        DrawSharedConsole(racing, host);
        return;
    }
    const Lobby::Member& self = Lobby::GetMembers()[Lobby::GetLocalSlot()];
    DrawCharacterChoice(racing || (!host && self.Ready));
    if (!host) {
        bool ready = self.Ready;
        ImGui::BeginDisabled(racing);
        if (ImGui::Checkbox("Ready", &ready)) {
            Lobby::SetReady(ready);
        }
        ImGui::EndDisabled();
    }
    ImGui::Spacing();
    DrawSettings(host && !racing);
    ImGui::Spacing();

    if (racing) {
        if (ImGui::Button("Leave race")) {
            Netplay::LeaveRace();
        }
        if (Netplay::CanRestartRace()) {
            ImGui::SameLine();
            if (ImGui::Button("Restart race")) {
                Netplay::RestartRace();
            }
        }
        ImGui::TextColored(kDimColor, "Leaving takes everyone back to the room.");
    } else if (host) {
        const std::string blocker = Lobby::GetStartBlocker();
        ImGui::BeginDisabled(!blocker.empty());
        if (ImGui::Button("Start race")) {
            Lobby::StartRace();
        }
        ImGui::EndDisabled();
        if (!blocker.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kDimColor, "%s", blocker.c_str());
        }
    } else {
        ImGui::TextColored(kDimColor, self.Ready ? "Waiting for the host to start" : "Get ready when you are set");
    }
    if (!Lobby::GetLastRaceMessage().empty()) {
        ImGui::TextWrapped("Last race: %s", Lobby::GetLastRaceMessage().c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button(host ? "Close room" : "Leave room")) {
        Lobby::Leave();
    }
}

std::string GetGamepadLabel(const GamepadAssignment::Gamepad& gamepad) {
    return "Controller " + std::to_string(gamepad.Number) + ": " + gamepad.Name;
}

/** Which gamepad drives this copy of the game, for two copies on one computer. Saved in its settings folder. */
void DrawControllerChoice() {
    const std::vector<GamepadAssignment::Gamepad> gamepads = GamepadAssignment::GetGamepads();
    const std::string choice = CVarGetString(GamepadAssignment::kCVar, "");
    std::string current = "All controllers";
    if (!choice.empty()) {
        current = choice + " (not connected)";
        for (const GamepadAssignment::Gamepad& gamepad : gamepads) {
            if (gamepad.Id == choice) {
                current = GetGamepadLabel(gamepad);
            }
        }
    }

    std::string selected = choice;
    if (ImGui::BeginCombo("Your controller", current.c_str())) {
        if (ImGui::Selectable("All controllers", choice.empty())) {
            selected = "";
        }
        for (const GamepadAssignment::Gamepad& gamepad : gamepads) {
            // The ID stays put while "(pressed)" comes and goes, so the click lands.
            const std::string label = GetGamepadLabel(gamepad) + (gamepad.AnyButtonHeld ? "  (pressed)" : "") +
                                      "###gamepad" + std::to_string(gamepad.Number);
            if (ImGui::Selectable(label.c_str(), gamepad.Id == choice)) {
                selected = gamepad.Id;
            }
        }
        ImGui::EndCombo();
    }
    if (selected != choice) {
        CVarSetString(GamepadAssignment::kCVar, selected.c_str());
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    std::string pressed;
    for (const GamepadAssignment::Gamepad& gamepad : gamepads) {
        if (gamepad.AnyButtonHeld) {
            pressed += (pressed.empty() ? "" : ", ") + std::to_string(gamepad.Number);
        }
    }
    ImGui::TextColored(kDimColor, pressed.empty() ? "Hold a button to see its controller's number"
                                                  : "Button held on controller %s", pressed.c_str());
}

void CheckboxCVar(const char* label, const char* cvar, s32 fallback = 1) {
    bool enabled = CVarGetInteger(cvar, fallback) != 0;
    if (ImGui::Checkbox(label, &enabled)) {
        CVarSetInteger(cvar, enabled ? 1 : 0);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
}

/**
 * Whether races show and sound only this player's view, or the split screen of every player, where each player's
 * sounds come from one side.
 */
void DrawViewChoice() {
    CheckboxCVar("Show only my player", NetSession::kOwnViewCVar);
    CheckboxCVar("Hear only my player", NetSession::kOwnAudioCVar);
    CheckboxCVar("Show connection stats", NetSession::kStatsOverlayCVar);
    CheckboxCVar("Smooth other karts' corrections", KartSmoothing::kCVar);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When the game corrects a guess about another player's input, their kart glides to its "
                          "place\ninstead of jumping. Only changes what you see.");
    }
    ImGui::BeginDisabled(Lobby::GetState() != Lobby::State::Offline);
    CheckboxCVar("Connect directly to other players", Lobby::kDirectConnectCVar);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Faster than going through the relay server, when your routers allow it.\nThe other "
                          "players in the room see your IP address. Applies when you next host or join.");
    }
}

/** Voice chat: on or off, this player's microphone, and how loud the others are. */
void DrawVoiceChoice() {
    CheckboxCVar("Voice chat", VoiceChat::kEnabledCVar);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Talk with the other players of a room, in the menus and in races.\nVoice goes straight "
                          "between players, never through the relay server: you hear, and are heard by,\nthe "
                          "players you are connected to directly. It uses the microphone your system has selected.");
    }
    if (CVarGetInteger(VoiceChat::kEnabledCVar, 1) == 0) {
        return;
    }
    ImGui::SameLine();
    CheckboxCVar("Mute my microphone", VoiceChat::kMicMutedCVar, 0);
    constexpr s32 kMaxVolumePercent = 200;
    s32 volume = (s32) std::lround(CVarGetFloat(VoiceChat::kVolumeCVar, 1.0f) * 100.0f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::SliderInt("Voice volume", &volume, 0, kMaxVolumePercent, "%d%%")) {
        CVarSetFloat(VoiceChat::kVolumeCVar, (f32) volume / 100.0f);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    if (CVarGetInteger(Lobby::kDirectConnectCVar, 1) == 0) {
        ImGui::TextColored(kDimColor, "Voice chat needs \"Connect directly to other players\".");
    } else if (VoiceChat::GetMicStatus() == VoiceChat::MicStatus::Denied) {
        ImGui::TextColored(kErrorColor, "The microphone is not allowed: no voice chat. Allow it for SpaghettiKart in\n"
                                        "System Settings > Privacy & Security > Microphone.");
    } else if (VoiceChat::GetMicStatus() == VoiceChat::MicStatus::Failed) {
        ImGui::TextColored(kErrorColor, "The microphone failed to start: no voice chat.");
    }
}

/** How long this player's own input is held back: automatic from the ping, or a fixed number of frames. */
void DrawInputDelayChoice() {
    const s32 delay = CVarGetInteger(NetSession::kInputDelayCVar, NetSession::kAutoInputDelay);
    const auto getLabel = [](s32 frames) {
        if (frames == NetSession::kAutoInputDelay) {
            return std::string("Automatic");
        }
        return std::to_string(frames) + ((frames == 1) ? " frame" : " frames") + " (" +
               std::to_string(frames * 100 / 3) + " ms)";
    };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::BeginCombo("Input delay", getLabel(delay).c_str())) {
        for (s32 frames = NetSession::kAutoInputDelay; frames <= NetSession::kMaxInputDelay; frames++) {
            if (ImGui::Selectable(getLabel(frames).c_str(), frames == delay)) {
                CVarSetInteger(NetSession::kInputDelayCVar, frames);
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Holding your input back gives it time to reach the others, so their screens correct your "
                          "kart less often.\nAutomatic picks it from the ping when each race starts: 0 on the same "
                          "network, up to 3 far away.");
    }
    if (NetSession::IsActive()) {
        ImGui::SameLine();
        ImGui::TextColored(kDimColor, "this race: %u", NetSession::GetInputDelay());
    }
}

void RegisterNetplayWidgets() {
    mPortMenu->AddSidebarEntry("Enhancements", "Netplay", 1);
    WidgetPath path = { "Enhancements", "Netplay", SECTION_COLUMN_1 };
    mPortMenu->AddWidget(path, "Open Netplay Lobby", WIDGET_WINDOW_BUTTON)
        .CVar(kWindowCVar)
        .Options(UIWidgets::ButtonOptions().Tooltip("Race online: host a room or join one with its code"))
        .WindowName(kWindowName);
}

RegisterMenuInitFunc sInitFunc(RegisterNetplayWidgets);

} // namespace

void NetplayStatsOverlay::Draw() {
    if ((CVarGetInteger(NetSession::kStatsOverlayCVar, 1) == 0) || !NetSession::IsInRace()) {
        return;
    }
    constexpr f32 kMargin = 8.0f;
    constexpr f32 kBackgroundAlpha = 0.55f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - kMargin,
                                   viewport->WorkPos.y + kMargin),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowBgAlpha(kBackgroundAlpha);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin(GetName().c_str(), nullptr, flags)) {
        DrawElement();
    }
    ImGui::End();
}

void NetplayStatsOverlay::DrawElement() {
    NetSession::LiveStats stats;
    if (!NetSession::GetLiveStats(&stats)) {
        ImGui::TextUnformatted("Connecting...");
        return;
    }
    // One line per player: the network's share of the ping, and the share spent waiting for a frame to read packets.
    for (size_t player = 0; player < stats.PingMs.size(); player++) {
        const s32 ping = stats.PingMs[player];
        if (ping < 0) {
            continue;
        }
        const s32 wait = stats.FrameWaitMs[player];
        const char* relayed = stats.Relayed[player] ? ", relayed" : "";
        if (wait < 0) {
            ImGui::Text("Ping P%zu %d ms%s", player + 1, ping, relayed);
        } else {
            ImGui::Text("Ping P%zu %d ms network + %d ms frame wait%s", player + 1, ping - wait, wait, relayed);
        }
    }
    ImGui::Text("Input delay %u (%u ms)", stats.InputDelay, stats.InputDelay * 100u / 3u);
    if (stats.RollbackShare > 0.0f) {
        ImGui::Text("Rollback %.0f%% of frames, %.1f avg, %u max", stats.RollbackShare * 100.0f,
                    stats.AvgRollbackFrames, stats.MaxRollbackFrames);
    } else {
        ImGui::TextUnformatted("Rollback none");
    }
    if (stats.Stalls > 0) {
        ImGui::TextColored(UIWidgets::ColorValues.at(UIWidgets::Colors::Yellow), "Waited for input %u times",
                           stats.Stalls);
    }
}

void NetplayVoiceOverlay::Draw() {
    if (Lobby::GetState() != Lobby::State::InRoom) {
        return;
    }
    // Who is talking, and what the player should know about their own microphone; nothing while all is quiet.
    std::string talking;
    if (VoiceChat::IsTalking()) {
        talking = "You";
    }
    const auto& members = Lobby::GetMembers();
    for (size_t slot = 0; slot < members.size(); slot++) {
        if (VoiceChat::GetMemberStatus((u8) slot) == VoiceChat::MemberStatus::Talking) {
            talking += (talking.empty() ? "" : ", ") + GetMemberName(members[slot], slot);
        }
    }
    const VoiceChat::MicStatus status = VoiceChat::GetMicStatus();
    const bool warn = (status == VoiceChat::MicStatus::Denied) || (status == VoiceChat::MicStatus::Failed);
    const bool muted = status == VoiceChat::MicStatus::Muted;
    if (talking.empty() && !warn && !muted) {
        return;
    }
    constexpr f32 kMargin = 8.0f;
    constexpr f32 kBackgroundAlpha = 0.55f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + kMargin, viewport->WorkPos.y + kMargin), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowBgAlpha(kBackgroundAlpha);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin(GetName().c_str(), nullptr, flags)) {
        if (!talking.empty()) {
            ImGui::TextColored(kTalkingColor, "Talking: %s", talking.c_str());
        }
        if (warn || muted) {
            ImGui::TextColored(warn ? kErrorColor : kDimColor, "Voice chat: %s", GetMicStatusText(status));
        }
    }
    ImGui::End();
}

void NetplayMatchOverlay::Draw() {
    if (Netplay::TakeMatchAlert()) {
        // The SDL backend keeps the window's id in the main viewport.
        const auto windowId = (Uint32) (intptr_t) ImGui::GetMainViewport()->PlatformHandle;
        SDL_Window* window = SDL_GetWindowFromID(windowId);
        if ((window != nullptr) && ((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) == 0)) {
            SDL_FlashWindow(window, SDL_FLASH_UNTIL_FOCUSED);
        }
    }
    const std::string notice = Netplay::GetMatchNotice();
    if (notice.empty()) {
        return;
    }
    constexpr f32 kMargin = 8.0f;
    constexpr f32 kBackgroundAlpha = 0.7f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x / 2.0f,
                                   viewport->WorkPos.y + viewport->WorkSize.y - kMargin),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowBgAlpha(kBackgroundAlpha);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin(GetName().c_str(), nullptr, flags)) {
        ImGui::TextColored(kTalkingColor, "%s", notice.c_str());
    }
    ImGui::End();
}

void NetplayWindow::DrawElement() {
    DrawControllerChoice();
    DrawViewChoice();
    DrawVoiceChoice();
    DrawInputDelayChoice();
    ImGui::Separator();
    switch (Lobby::GetState()) {
        case Lobby::State::Offline:
            DrawConnect();
            break;
        case Lobby::State::Connecting:
            ImGui::TextUnformatted("Connecting to the relay server...");
            if (ImGui::Button("Cancel")) {
                Lobby::Leave();
            }
            break;
        case Lobby::State::Matchmaking:
            DrawMatchmaking();
            break;
        case Lobby::State::InRoom:
            DrawRoom();
            break;
    }
}

} // namespace GameUI
