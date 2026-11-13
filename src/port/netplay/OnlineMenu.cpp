#include "OnlineMenu.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "Lobby.h"
#include "port/AssetsVersion.h"
#include "Netplay.h"
#include "NetplayIdentity.h"
#include "PageVersion.h"
#include "RelayClient.h"

extern "C" {
#include "main.h"
#include "menus.h"
#include "defines.h"
#include "sounds.h"
#include "audio/external.h"
}

namespace {

// Room names come from the host's name, shown in the menus' font.
constexpr size_t kMaxNameLength = 12;
constexpr auto kRefreshInterval = std::chrono::seconds(3);
constexpr size_t kMaxRoomsShown = 5;

// Game Select's button: one picture with the key and the word, the size of the OPTION and DATA ones, drawn and folded
// the same way. The art is the port's own (tools/make-online-button.py).
constexpr s32 kButtonWidth = 58;
constexpr s32 kButtonHeight = 19;
// Aligned like the game's own asset paths: the renderer takes odd addresses for raw texture data.
alignas(8) const char kOnlineButtonPath[] = "__OTR__textures/menu/online_button.rgba16";
alignas(8) const char kLeaveButtonPath[] = "__OTR__textures/menu/leave_button.rgba16";
MenuTexture sOnlineButton[] = {
    { 1, reinterpret_cast<u64*>(const_cast<char*>(kOnlineButtonPath)), kButtonWidth, kButtonHeight, 0, 0, 0 },
    { 0, nullptr, 0, 0, 0, 0, 0 },
};
MenuTexture sLeaveButton[] = {
    { 1, reinterpret_cast<u64*>(const_cast<char*>(kLeaveButtonPath)), kButtonWidth, kButtonHeight, 0, 0, 0 },
    { 0, nullptr, 0, 0, 0, 0, 0 },
};

// The name grid: letters and digits, then the two commands
const char* const kGridRows[] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789-. " };
constexpr s32 kNumGridRows = sizeof(kGridRows) / sizeof(kGridRows[0]);
constexpr s32 kCommandRow = kNumGridRows; // DEL, END
constexpr s32 kGridColumns = 10;

enum class Screen { Rooms, Name };

struct OnlineMenuState {
    Screen Current = Screen::Rooms;
    s32 Cursor = 0; // Over the rooms, then HOST A ROOM, then NAME
    RelayRoomList RoomList;
    std::chrono::steady_clock::time_point LastRefresh;
    std::string NameDraft;
    s32 GridColumn = 0;
    s32 GridRow = 0;
};

OnlineMenuState sOnline;

std::string GetRelayAddress() {
    return CVarGetString(Lobby::kRelayCVar, Lobby::kDefaultRelay);
}

std::string GetName() {
    const std::string name = CVarGetString(Lobby::kNameCVar, "");
    return name.empty() ? "PLAYER" : name;
}

/** Text the menus' font can show: uppercase, with characters it has no letter for as spaces. */
std::string ToMenuText(const std::string& text, size_t maxLength) {
    std::string shown;
    for (char c : text.substr(0, maxLength)) {
        char upper[2] = { (char) toupper((unsigned char) c), 0 };
        shown += (char_to_glyph_index(upper) >= 0) ? upper[0] : ' ';
    }
    return shown;
}

size_t GetNumRooms() {
    return std::min(sOnline.RoomList.GetRooms().size(), kMaxRoomsShown);
}

s32 GetHostItem() {
    return (s32) GetNumRooms();
}

s32 GetNameItem() {
    return GetHostItem() + 1;
}

void RefreshRooms() {
    sOnline.RoomList.Refresh(GetRelayAddress(), RelayProtocol::MakeBuildId(NetplayIdentity::GetBuildId()),
                             NetplayIdentity::ComputeContentHash());
    sOnline.LastRefresh = std::chrono::steady_clock::now();
}

void SaveName(const std::string& name) {
    CVarSetString(Lobby::kNameCVar, name.c_str());
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

void ActRooms(u16 pressed) {
    const s32 last = GetNameItem();
    if ((pressed & D_JPAD) && (sOnline.Cursor < last)) {
        sOnline.Cursor++;
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    if ((pressed & U_JPAD) && (sOnline.Cursor > 0)) {
        sOnline.Cursor--;
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    if (pressed & B_BUTTON) {
        fade_to_online_menu();
        play_sound2(SOUND_MENU_GO_BACK);
        return;
    }
    if (!(pressed & A_BUTTON)) {
        return;
    }
    if (sOnline.Cursor < GetHostItem()) {
        Lobby::Join(GetRelayAddress(), sOnline.RoomList.GetRooms()[sOnline.Cursor].Code, GetName());
        play_sound2(SOUND_MENU_SELECT);
    } else if (sOnline.Cursor == GetHostItem()) {
        Lobby::Host(GetRelayAddress(), GetName(), true);
        play_sound2(SOUND_MENU_SELECT);
    } else {
        sOnline.Current = Screen::Name;
        sOnline.NameDraft = ToMenuText(GetName(), kMaxNameLength);
        sOnline.GridColumn = 0;
        sOnline.GridRow = 0;
        play_sound2(SOUND_MENU_SELECT);
    }
}

void ActName(u16 pressed) {
    const s32 columns = (sOnline.GridRow == kCommandRow) ? 2 : kGridColumns;
    if ((pressed & R_JPAD) && (sOnline.GridColumn < columns - 1)) {
        sOnline.GridColumn++;
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    if ((pressed & L_JPAD) && (sOnline.GridColumn > 0)) {
        sOnline.GridColumn--;
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    if ((pressed & D_JPAD) && (sOnline.GridRow < kCommandRow)) {
        sOnline.GridRow++;
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    if ((pressed & U_JPAD) && (sOnline.GridRow > 0)) {
        sOnline.GridRow--;
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    if (sOnline.GridRow == kCommandRow) {
        sOnline.GridColumn = std::min(sOnline.GridColumn, 1);
    }
    const bool erase = (pressed & B_BUTTON) || ((pressed & A_BUTTON) && (sOnline.GridRow == kCommandRow) &&
                                                (sOnline.GridColumn == 0));
    const bool done = (pressed & START_BUTTON) ||
                      ((pressed & A_BUTTON) && (sOnline.GridRow == kCommandRow) && (sOnline.GridColumn == 1));
    if (done) {
        if (!sOnline.NameDraft.empty()) {
            SaveName(sOnline.NameDraft);
        }
        sOnline.Current = Screen::Rooms;
        play_sound2(SOUND_MENU_OK_CLICKED);
    } else if (erase) {
        if (sOnline.NameDraft.empty()) {
            sOnline.Current = Screen::Rooms;
        } else {
            sOnline.NameDraft.pop_back();
        }
        play_sound2(SOUND_MENU_GO_BACK);
    } else if ((pressed & A_BUTTON) && (sOnline.NameDraft.size() < kMaxNameLength)) {
        sOnline.NameDraft += kGridRows[sOnline.GridRow][sOnline.GridColumn];
        play_sound2(SOUND_MENU_SELECT);
    }
}

/** The cursor box the OPTION screen uses, around text at this position. */
void DrawCursor(MenuItem* item, s32 column, s32 row) {
    Unk_D_800E70A0 position = {};
    position.column = (s16) (column - 0xA);
    position.row = (s16) (row - 8);
    pause_menu_item_box_cursor(item, &position);
}

// Frame interpolation pairs each letter with the same-sized letter drawn at the same place in the order last frame,
// so these screens draw their fixed text first and text that changes (names, the room list) after it: letters added
// or removed then never shift the others, which would make them slide sideways.

/**
 * Above the room list: why a friend's room may be missing. The relay lists only rooms of this build and these game
 * files. The browser build updates itself in the background (PageVersion.h), so a friend can be one restart behind or
 * ahead; game files made by an older extractor (AssetsVersion.h) stay in use until the player makes them again.
 */
void DrawVersionNote() {
    std::string text;
    const u8 hidden = sOnline.RoomList.GetOtherBuildRooms();
    const u8 otherFiles = sOnline.RoomList.GetOtherContentRooms();
    const PageVersion::State page = PageVersion::GetState();
    if (page == PageVersion::State::Waiting) {
        text = "NEW VERSION READY - RESTART TO UPDATE";
    } else if (page == PageVersion::State::Downloading) {
        text = "DOWNLOADING A NEW VERSION...";
    } else if (!AssetsVersion::IsGameArchiveCurrent()) {
        // The game offers to make them again when it starts.
        text = "YOUR GAME FILES ARE OLD - RESTART TO REMAKE";
    } else if ((hidden > 0) && (page == PageVersion::State::Newest)) {
        // This page is the server's newest: the hidden rooms' hosts have to restart into it.
        text = std::to_string(hidden) + ((hidden == 1) ? " ROOM IS ON AN OLDER VERSION" : " ROOMS ARE ON OLDER VERSIONS");
    } else if (hidden > 0) {
        text = std::to_string(hidden) + ((hidden == 1) ? " ROOM IS ON ANOTHER VERSION" : " ROOMS ARE ON OTHER VERSIONS");
    } else if (otherFiles > 0) {
        text = std::to_string(otherFiles) + ((otherFiles == 1) ? " ROOM HAS OTHER GAME FILES OR MODS"
                                                               : " ROOMS HAVE OTHER GAME FILES OR MODS");
    } else {
        return;
    }
    set_text_color(TEXT_GREEN);
    print_text1_center_mode_1(0xA0, 0x40, (char*) text.c_str(), 0, 0.5f, 0.5f);
}

void DrawRooms(MenuItem* item) {
    const std::vector<RelayRoomList::Room>& rooms = sOnline.RoomList.GetRooms();
    const s32 listRow = 0x66;
    const s32 listRows = std::max((s32) GetNumRooms(), 1);
    const s32 hostRow = listRow + listRows * 0x14 + 0x8;
    const s32 nameRow = hostRow + 0x18;

    set_text_color(TEXT_YELLOW);
    print_text_mode_1(0x28, 0x50, (char*) "ROOMS", 0, 0.75f, 0.75f);
    set_text_color_rainbow_if_selected(sOnline.Cursor, GetHostItem(), TEXT_YELLOW);
    print_text_mode_1(0x32, hostRow, (char*) "HOST A ROOM", 0, 0.75f, 0.75f);
    set_text_color_rainbow_if_selected(sOnline.Cursor, GetNameItem(), TEXT_YELLOW);
    print_text_mode_1(0x32, nameRow, (char*) "NAME", 0, 0.75f, 0.75f);
    set_text_color(TEXT_GREEN);
    print_text_mode_1(0x78, nameRow, (char*) ToMenuText(GetName(), kMaxNameLength).c_str(), 0, 0.75f, 0.75f);

    s32 row = listRow;
    for (size_t i = 0; i < GetNumRooms(); i++) {
        const RelayRoomList::Room& room = rooms[i];
        set_text_color_rainbow_if_selected(sOnline.Cursor, (s32) i, TEXT_GREEN);
        print_text_mode_1(0x32, row, (char*) ToMenuText(room.HostName, kMaxNameLength).c_str(), 0, 0.7f, 0.7f);
        // The font has no slash.
        const std::string players = std::to_string(room.Members) + ((room.Members == 1) ? " PLAYER" : " PLAYERS") +
                                    (room.Racing ? " RACING" : "");
        print_text_mode_1(0xA8, row, (char*) players.c_str(), 0, 0.55f, 0.55f);
        if (sOnline.Cursor == (s32) i) {
            DrawCursor(item, 0x32, row);
        }
        row += 0x14;
    }
    if (rooms.empty()) {
        set_text_color(TEXT_BLUE);
        const char* text = sOnline.RoomList.HasList() ? "NO ROOMS YET" : "LOOKING FOR ROOMS...";
        print_text_mode_1(0x32, row, (char*) text, 0, 0.6f, 0.6f);
    }
    if (sOnline.Cursor == GetHostItem()) {
        DrawCursor(item, 0x32, hostRow);
    } else if (sOnline.Cursor == GetNameItem()) {
        DrawCursor(item, 0x32, nameRow);
    }

    const std::string& error = !Lobby::GetError().empty() ? Lobby::GetError() : sOnline.RoomList.GetError();
    if (!error.empty()) {
        set_text_color(TEXT_RED);
        print_text_mode_1(0x1E, 0xD8, (char*) ToMenuText(error, 40).c_str(), 0, 0.45f, 0.45f);
    }
}

void DrawName(MenuItem* item) {
    set_text_color(TEXT_YELLOW);
    print_text_mode_1(0x28, 0x50, (char*) "NAME", 0, 0.75f, 0.75f);
    for (s32 gridRow = 0; gridRow <= kCommandRow; gridRow++) {
        const s32 row = 0x78 + gridRow * 0x18;
        const s32 columns = (gridRow == kCommandRow) ? 2 : kGridColumns;
        for (s32 column = 0; column < columns; column++) {
            const bool selected = (gridRow == sOnline.GridRow) && (column == sOnline.GridColumn);
            const s32 x = (gridRow == kCommandRow) ? 0x50 + column * 0x60 : 0x3C + column * 0x18;
            char letter[2] = { kGridRows[std::min(gridRow, kNumGridRows - 1)][column], 0 };
            const char* text = (gridRow == kCommandRow) ? ((column == 0) ? "DEL" : "END") : letter;
            set_text_color(selected ? TEXT_YELLOW : TEXT_BLUE);
            print_text_mode_1(x, row, (char*) text, 0, 0.75f, 0.75f);
            if (selected) {
                DrawCursor(item, x, row);
            }
        }
    }
    // The name so far, then a cursor that blinks by color: always drawn, so the letters stay in the same order.
    std::string draft = sOnline.NameDraft;
    set_text_color(TEXT_GREEN);
    print_text_mode_1(0x78, 0x50, (char*) draft.c_str(), 0, 0.75f, 0.75f);
    set_text_color(((gGlobalTimer / 8) % 2 == 0) ? TEXT_YELLOW : TEXT_BLUE);
    const s32 cursorColumn = 0x78 + (s32) (get_string_width((char*) draft.c_str()) * 0.75f);
    print_text_mode_1(cursorColumn, 0x50, (char*) "-", 0, 0.75f, 0.75f);
}

void DrawStatus(const char* text) {
    set_text_color(TEXT_YELLOW);
    print_text1_center_mode_1(0xA0, 0x78, (char*) text, 0, 0.8f, 0.8f);
    set_text_color(TEXT_BLUE);
    print_text1_center_mode_1(0xA0, 0x96, (char*) "B TO LEAVE", 0, 0.6f, 0.6f);
}

/** On a shared console: each player's controller number and name, along the bottom of Game Select. */
void DrawConsolePlayers() {
    const auto& members = Lobby::GetMembers();
    s32 column = 0x15;
    for (size_t slot = 0; slot < members.size(); slot++) {
        const s32 player = Netplay::GetConsolePlayer((u8) slot);
        if (!members[slot].Present || (player < 0)) {
            continue;
        }
        const std::string text = std::to_string(player + 1) + "P " + ToMenuText(members[slot].Name, 8);
        set_text_color(TEXT_GREEN);
        print_text_mode_1(column, 0xE4, (char*) text.c_str(), 0, 0.45f, 0.45f);
        column += 0x48;
    }
}

} // namespace

extern "C" void online_menu_enter(void) {
    sOnline.Current = Screen::Rooms;
    sOnline.Cursor = 0;
    RefreshRooms();
    PageVersion::Check();
}

extern "C" void online_menu_act(struct Controller* controller) {
    sOnline.RoomList.Update();
    const Lobby::State state = Lobby::GetState();
    if ((state == Lobby::State::Offline) && !sOnline.RoomList.IsBusy() &&
        (std::chrono::steady_clock::now() - sOnline.LastRefresh >= kRefreshInterval)) {
        RefreshRooms();
    }
    if (is_screen_being_faded()) {
        return;
    }
    u16 pressed = controller->buttonPressed | controller->stickPressed;
    if (state != Lobby::State::Offline) {
        // Connecting, or in a room whose race has to end before its console takes this player in
        if (pressed & B_BUTTON) {
            Lobby::Leave();
            play_sound2(SOUND_MENU_GO_BACK);
        }
        return;
    }
    if (sOnline.Current == Screen::Name) {
        ActName(pressed);
        return;
    }
    if (pressed & START_BUTTON) {
        pressed |= A_BUTTON;
    }
    sOnline.Cursor = std::min(sOnline.Cursor, GetNameItem());
    ActRooms(pressed);
}

extern "C" void render_online_menu(MenuItem* item) {
    gDisplayListHead = draw_box_wide(gDisplayListHead, 0, 0, 0x140, 0xF0, 0, 0, 0, 0x64);
    set_text_color(TEXT_YELLOW);
    print_text1_center_mode_1(0xA0, 0x28, (char*) "ONLINE", 0, 1.4f, 1.4f);
    switch (Lobby::GetState()) {
        case Lobby::State::Connecting:
            DrawStatus("CONNECTING...");
            return;
        case Lobby::State::InRoom: {
            const auto& members = Lobby::GetMembers();
            const bool racing =
                std::any_of(members.begin(), members.end(), [](const Lobby::Member& member) { return member.Racing; });
            DrawStatus(racing ? "WAITING FOR THE RACE TO END" : "JOINING...");
            return;
        }
        default:
            break;
    }
    if (sOnline.Current == Screen::Name) {
        DrawName(item);
    } else {
        DrawRooms(item);
        DrawVersionNote();
    }
}

extern "C" void render_online_button(MenuItem* item) {
    const bool console = Netplay_IsSharedConsole();
    if (console) {
        DrawConsolePlayers();
    }
    // As for OPTION and DATA: a box behind the picture, flashing while chosen, and both fold away together.
    if (item->param1 < 0x20) {
        const s32 fold = (item->param1 * kButtonWidth) / 64;
        const s32 left = item->column + 1 + fold;
        const s32 right = item->column - fold + kButtonWidth - 1;
        const s32 bottom = item->row + kButtonHeight - 2;
        if (gMainMenuSelection == MAIN_MENU_ONLINE) {
            gDisplayListHead = draw_flash_select_case_fast(gDisplayListHead, left, item->row, right, bottom);
        } else {
            gDisplayListHead = draw_box_fill(gDisplayListHead, left, item->row, right, bottom, 1, 1, 1, 0xFF);
        }
    }
    gDisplayListHead = func_8009BC9C(gDisplayListHead, console ? sLeaveButton : sOnlineButton, item->column, item->row,
                                     2, item->param1);
}
