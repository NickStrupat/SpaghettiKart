#include "NetplayWindow.h"

#include <algorithm>
#include <imgui.h>
#include <string>
#include <vector>

#include "PortMenu.h"
#include "UIWidgets.h"
#include "port/Game.h"
#include "engine/World.h"
#include "port/netplay/Lobby.h"
#include "port/netplay/Netplay.h"

namespace GameUI {
extern std::shared_ptr<PortMenu> mPortMenu;

namespace {

constexpr const char* kWindowName = "Netplay";
constexpr const char* kWindowCVar = "gNetplay.WindowOpen";
constexpr const char* kNameCVar = "gNetplay.Name";
constexpr const char* kRelayCVar = "gNetplay.Relay";
constexpr const char* kDefaultRelay = "127.0.0.1:7777";
constexpr const char* kCharacterNames[] = { "Mario", "Luigi", "Yoshi", "Toad", "Donkey Kong", "Wario", "Peach", "Bowser" };
constexpr const char* kCCNames[] = { "50cc", "100cc", "150cc", "Extra (mirrored)" };
const ImVec4 kErrorColor = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
const ImVec4 kDimColor = ImVec4(0.65f, 0.65f, 0.65f, 1.0f);

char sRoomCodeInput[RelayProtocol::kCodeLength + 1] = "";

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

void DrawConnect() {
    InputCVarString("Your name", kNameCVar, "Player", Lobby::kMaxNameLength);
    InputCVarString("Relay server", kRelayCVar, kDefaultRelay, 63);
    const std::string relay = CVarGetString(kRelayCVar, kDefaultRelay);
    ImGui::Spacing();

    if (ImGui::Button("Host a room")) {
        Lobby::Host(relay, GetPlayerName());
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("##code", sRoomCodeInput, sizeof(sRoomCodeInput), ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    ImGui::BeginDisabled(strlen(sRoomCodeInput) != RelayProtocol::kCodeLength);
    if (ImGui::Button("Join room")) {
        Lobby::Join(relay, sRoomCodeInput, GetPlayerName());
    }
    ImGui::EndDisabled();

    if (!Lobby::GetError().empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kErrorColor, "%s", Lobby::GetError().c_str());
    }
}

void DrawMembers() {
    const auto& members = Lobby::GetMembers();
    if (!ImGui::BeginTable("members", 3, ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn("Player");
    ImGui::TableSetupColumn("Character");
    ImGui::TableSetupColumn("Status");
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
        if (slot == 0) {
            ImGui::TextUnformatted(member.InRace ? "Host, racing" : "Host");
        } else if (!member.Known) {
            ImGui::TextColored(kDimColor, "Connecting");
        } else {
            ImGui::TextUnformatted(member.InRace ? "Racing" : (member.Ready ? "Ready" : "Not ready"));
        }
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
        ImGui::TextColored(kDimColor, "Netplay runs the cup's first race; full cups come later.");
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

void DrawRoom() {
    const std::string code = Lobby::GetRoomCode();
    ImGui::Text("Room code:");
    ImGui::SameLine();
    ImGui::TextColored(UIWidgets::ColorValues.at(UIWidgets::Colors::LightBlue), "%s", code.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        ImGui::SetClipboardText(code.c_str());
    }
    if (Lobby::GetPingMs() >= 0) {
        ImGui::SameLine();
        ImGui::TextColored(kDimColor, "  relay ping %d ms", Lobby::GetPingMs());
    }
    ImGui::Separator();
    DrawMembers();
    ImGui::Separator();

    const bool racing = Netplay::IsRaceRunning();
    const bool host = Lobby::IsHost();
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

void NetplayWindow::DrawElement() {
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
        case Lobby::State::InRoom:
            DrawRoom();
            break;
    }
}

} // namespace GameUI
