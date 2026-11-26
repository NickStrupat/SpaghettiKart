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
#include "RoomCodeInput.h"

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
// How long the note after L or R in a room stays up.
constexpr auto kRefusedNoteTime = std::chrono::milliseconds(2500);
// The list scrolls past this many.
constexpr s32 kMaxRoomsShown = 3;

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

// Room codes are stick directions, shown as arrows (tools/make-code-arrows.py): the menus' font has none. Small ones
// fit beside Game Select's buttons.
constexpr s32 kArrowSize = 16;
constexpr s32 kSmallArrowSize = 10;
alignas(8) const char kArrowUpPath[] = "__OTR__textures/menu/code_arrow_up.rgba16";
alignas(8) const char kArrowDownPath[] = "__OTR__textures/menu/code_arrow_down.rgba16";
alignas(8) const char kArrowLeftPath[] = "__OTR__textures/menu/code_arrow_left.rgba16";
alignas(8) const char kArrowRightPath[] = "__OTR__textures/menu/code_arrow_right.rgba16";
alignas(8) const char kSmallArrowUpPath[] = "__OTR__textures/menu/code_arrow_small_up.rgba16";
alignas(8) const char kSmallArrowDownPath[] = "__OTR__textures/menu/code_arrow_small_down.rgba16";
alignas(8) const char kSmallArrowLeftPath[] = "__OTR__textures/menu/code_arrow_small_left.rgba16";
alignas(8) const char kSmallArrowRightPath[] = "__OTR__textures/menu/code_arrow_small_right.rgba16";
#define ARROW_TEXTURE(path, size)                                                                       \
    {                                                                                                  \
        { 1, reinterpret_cast<u64*>(const_cast<char*>(path)), size, size, 0, 0, 0 },                   \
        { 0, nullptr, 0, 0, 0, 0, 0 },                                                                 \
    }
// In the order of RelayProtocol::kCodeAlphabet: up, down, left, right
MenuTexture sArrows[4][2] = {
    ARROW_TEXTURE(kArrowUpPath, kArrowSize),
    ARROW_TEXTURE(kArrowDownPath, kArrowSize),
    ARROW_TEXTURE(kArrowLeftPath, kArrowSize),
    ARROW_TEXTURE(kArrowRightPath, kArrowSize),
};
MenuTexture sSmallArrows[4][2] = {
    ARROW_TEXTURE(kSmallArrowUpPath, kSmallArrowSize),
    ARROW_TEXTURE(kSmallArrowDownPath, kSmallArrowSize),
    ARROW_TEXTURE(kSmallArrowLeftPath, kSmallArrowSize),
    ARROW_TEXTURE(kSmallArrowRightPath, kSmallArrowSize),
};
#undef ARROW_TEXTURE

// The name grid: letters and digits, then the two commands
const char* const kGridRows[] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789-. " };
constexpr s32 kNumGridRows = sizeof(kGridRows) / sizeof(kGridRows[0]);
constexpr s32 kCommandRow = kNumGridRows; // DEL, END
constexpr s32 kGridColumns = 10;

enum class Screen { Rooms, Name, Code };

// The ONLINE screen's items, top to bottom; the rooms follow them.
enum Item : s32 {
    kQuickMatchItem,
    kHostPublicItem,
    kHostPrivateItem,
    kJoinByCodeItem,
    kNameItem,
    kFirstRoomItem,
};

struct OnlineMenuState {
    Screen Current = Screen::Rooms;
    s32 Cursor = 0; // An Item, or kFirstRoomItem plus a room's place in the list
    s32 FirstRoomShown = 0;
    RelayRoomList RoomList;
    std::chrono::steady_clock::time_point LastRefresh;
    std::string NameDraft;
    s32 GridColumn = 0;
    s32 GridRow = 0;
    std::string CodeDraft;
    RoomCodeInput::Reader CodeReader;
    // When L or R last asked for OPTION or DATA in a room, for the note that says why nothing happened. Drawing only.
    std::chrono::steady_clock::time_point OptionDataRefusedAt;
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

s32 GetNumRooms() {
    return (s32) sOnline.RoomList.GetRooms().size();
}

s32 GetLastItem() {
    return kFirstRoomItem + GetNumRooms() - 1;
}

/** Scrolls the room list so that the cursor, when on a room, is in view. */
void ScrollToCursor() {
    const s32 room = sOnline.Cursor - kFirstRoomItem;
    if (room < 0) {
        sOnline.FirstRoomShown = 0;
    } else if (room < sOnline.FirstRoomShown) {
        sOnline.FirstRoomShown = room;
    } else if (room >= sOnline.FirstRoomShown + kMaxRoomsShown) {
        sOnline.FirstRoomShown = room - kMaxRoomsShown + 1;
    }
    sOnline.FirstRoomShown = std::clamp(sOnline.FirstRoomShown, 0, std::max(GetNumRooms() - kMaxRoomsShown, 0));
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

/** Leaving the ONLINE screen with B keeps matchmaking going: a match takes the player into the room from anywhere. */
void ActRooms(u16 pressed) {
    if ((pressed & D_JPAD) && (sOnline.Cursor < GetLastItem())) {
        sOnline.Cursor++;
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    if ((pressed & U_JPAD) && (sOnline.Cursor > 0)) {
        sOnline.Cursor--;
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    ScrollToCursor();
    if (pressed & B_BUTTON) {
        fade_to_online_menu();
        play_sound2(SOUND_MENU_GO_BACK);
        return;
    }
    if (!(pressed & A_BUTTON)) {
        return;
    }
    if (sOnline.Cursor >= kFirstRoomItem) {
        Lobby::Join(GetRelayAddress(), sOnline.RoomList.GetRooms()[sOnline.Cursor - kFirstRoomItem].Code, GetName());
        play_sound2(SOUND_MENU_SELECT);
    } else if (sOnline.Cursor == kQuickMatchItem) {
        if (Lobby::GetState() == Lobby::State::Matchmaking) {
            Lobby::Leave();
            play_sound2(SOUND_MENU_GO_BACK);
        } else {
            Lobby::Matchmake(GetRelayAddress(), GetName());
            play_sound2(SOUND_MENU_SELECT);
        }
    } else if ((sOnline.Cursor == kHostPublicItem) || (sOnline.Cursor == kHostPrivateItem)) {
        const bool isPrivate = sOnline.Cursor == kHostPrivateItem;
        Lobby::Host(GetRelayAddress(), GetName(), true,
                    isPrivate ? Lobby::Visibility::Private : Lobby::Visibility::Public);
        play_sound2(SOUND_MENU_SELECT);
    } else if (sOnline.Cursor == kJoinByCodeItem) {
        sOnline.Current = Screen::Code;
        sOnline.CodeDraft.clear();
        sOnline.CodeReader.Reset();
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

/** JOIN BY CODE: each flick or press adds a direction; the seventh joins the room. B on an empty code goes back. */
void ActCode(const Controller* controller) {
    const RoomCodeInput::Entry entry =
        sOnline.CodeReader.Read((s8) controller->rawStickX, (s8) controller->rawStickY, controller->buttonPressed);
    if ((entry == RoomCodeInput::Entry::Erase) && sOnline.CodeDraft.empty()) {
        sOnline.Current = Screen::Rooms;
        play_sound2(SOUND_MENU_GO_BACK);
        return;
    }
    if (!RoomCodeInput::Apply(entry, &sOnline.CodeDraft)) {
        return;
    }
    if (entry == RoomCodeInput::Entry::Erase) {
        play_sound2(SOUND_MENU_GO_BACK);
    } else if (sOnline.CodeDraft.size() < RelayProtocol::kCodeLength) {
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    } else {
        Lobby::Join(GetRelayAddress(), sOnline.CodeDraft, GetName());
        play_sound2(SOUND_MENU_SELECT);
        // A failed join comes back here, with the error, to type the code again.
        sOnline.CodeDraft.clear();
    }
}

/** A room code's arrows, left to right from this corner, this far apart. */
void DrawCodeArrows(const std::string& code, s32 column, s32 row, s32 spacing, bool useSmall) {
    for (size_t i = 0; i < code.size(); i++) {
        const char* letter = strchr(RelayProtocol::kCodeAlphabet, code[i]);
        if ((letter == nullptr) || (code[i] == 0)) {
            continue;
        }
        const size_t direction = (size_t) (letter - RelayProtocol::kCodeAlphabet);
        MenuTexture* texture = useSmall ? sSmallArrows[direction] : sArrows[direction];
        gDisplayListHead = func_8009BC9C(gDisplayListHead, texture, column + (s32) i * spacing, row, 2, 0);
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

void DrawError(s32 row) {
    const std::string& error = !Lobby::GetError().empty() ? Lobby::GetError() : sOnline.RoomList.GetError();
    if (!error.empty()) {
        set_text_color(TEXT_RED);
        print_text_mode_1(0x1E, row, (char*) ToMenuText(error, 40).c_str(), 0, 0.45f, 0.45f);
    }
}

void DrawRooms(MenuItem* item) {
    constexpr s32 kItemColumn = 0x32;
    constexpr s32 kFirstItemRow = 0x52;
    constexpr s32 kItemSpacing = 0x10;
    constexpr s32 kRoomsRow = kFirstItemRow + kFirstRoomItem * kItemSpacing + 0x6;
    constexpr s32 kFirstRoomRow = kRoomsRow + 0x10;
    const bool matchmaking = Lobby::GetState() == Lobby::State::Matchmaking;
    const char* const labels[] = { matchmaking ? "CANCEL MATCH" : "QUICK MATCH", "HOST PUBLIC ROOM",
                                   "HOST PRIVATE ROOM", "JOIN BY CODE", "NAME" };
    for (s32 i = 0; i < kFirstRoomItem; i++) {
        set_text_color_rainbow_if_selected(sOnline.Cursor, i, TEXT_YELLOW);
        print_text_mode_1(kItemColumn, kFirstItemRow + i * kItemSpacing, (char*) labels[i], 0, 0.7f, 0.7f);
    }
    set_text_color(TEXT_YELLOW);
    print_text_mode_1(0x28, kRoomsRow, (char*) "ROOMS", 0, 0.6f, 0.6f);

    // Text that changes comes last (see above).
    set_text_color(TEXT_GREEN);
    print_text_mode_1(0x98, kFirstItemRow + kNameItem * kItemSpacing,
                      (char*) ToMenuText(GetName(), kMaxNameLength).c_str(), 0, 0.7f, 0.7f);
    if (matchmaking) {
        const u32 waiting = Lobby::GetWaitingPlayers();
        const std::string text = (waiting == 0) ? "SEARCHING..." : "SEARCHING - " + std::to_string(waiting) + " WAITING";
        set_text_color(TEXT_GREEN);
        print_text_mode_1(0xA8, kFirstItemRow, (char*) text.c_str(), 0, 0.5f, 0.5f);
    }
    const std::vector<RelayRoomList::Room>& rooms = sOnline.RoomList.GetRooms();
    if (GetNumRooms() > kMaxRoomsShown) {
        const std::string count = std::to_string(GetNumRooms()) + " OPEN";
        set_text_color(TEXT_BLUE);
        print_text_mode_1(0x60, kRoomsRow, (char*) count.c_str(), 0, 0.5f, 0.5f);
    }
    s32 row = kFirstRoomRow;
    for (s32 i = sOnline.FirstRoomShown; i < std::min(GetNumRooms(), sOnline.FirstRoomShown + kMaxRoomsShown); i++) {
        const RelayRoomList::Room& room = rooms[i];
        set_text_color_rainbow_if_selected(sOnline.Cursor, kFirstRoomItem + i, TEXT_GREEN);
        print_text_mode_1(kItemColumn, row, (char*) ToMenuText(room.HostName, kMaxNameLength).c_str(), 0, 0.65f,
                          0.65f);
        // The font has no slash.
        const std::string players = std::to_string(room.Members) + ((room.Members == 1) ? " PLAYER" : " PLAYERS") +
                                    (room.Racing ? " RACING" : "");
        print_text_mode_1(0xA8, row, (char*) players.c_str(), 0, 0.5f, 0.5f);
        if (sOnline.Cursor == kFirstRoomItem + i) {
            DrawCursor(item, kItemColumn, row);
        }
        row += kItemSpacing;
    }
    if (rooms.empty()) {
        set_text_color(TEXT_BLUE);
        const char* text = sOnline.RoomList.HasList() ? "NO ROOMS YET" : "LOOKING FOR ROOMS...";
        print_text_mode_1(kItemColumn, row, (char*) text, 0, 0.55f, 0.55f);
    }
    if (sOnline.Cursor < kFirstRoomItem) {
        DrawCursor(item, kItemColumn, kFirstItemRow + sOnline.Cursor * kItemSpacing);
    }
    DrawError(0xE4);
}

/** JOIN BY CODE: a slot per direction, filled with arrows as they are entered; the next one blinks. */
void DrawCode() {
    constexpr s32 kSlotSpacing = 0x1C;
    constexpr s32 kFirstSlotColumn = 0xA0 - (s32) (RelayProtocol::kCodeLength * kSlotSpacing) / 2 + 6;
    constexpr s32 kSlotRow = 0x78;
    set_text_color(TEXT_YELLOW);
    print_text_mode_1(0x28, 0x50, (char*) "JOIN BY CODE", 0, 0.75f, 0.75f);
    set_text_color(TEXT_BLUE);
    print_text1_center_mode_1(0xA0, 0xA8, (char*) "FLICK THE STICK OR PRESS THE D-PAD", 0, 0.5f, 0.5f);
    print_text1_center_mode_1(0xA0, 0xB8, (char*) "THE ARROW KEYS WORK TOO", 0, 0.5f, 0.5f);
    print_text1_center_mode_1(0xA0, 0xC8, (char*) "B ERASES", 0, 0.5f, 0.5f);
    // A dash under every slot, always drawn so the letters keep their order; the next slot's blinks.
    for (s32 i = 0; i < (s32) RelayProtocol::kCodeLength; i++) {
        const bool next = i == (s32) sOnline.CodeDraft.size();
        set_text_color((next && ((gGlobalTimer / 8) % 2 == 0)) ? TEXT_YELLOW : TEXT_BLUE);
        print_text_mode_1(kFirstSlotColumn + i * kSlotSpacing + 3, kSlotRow + 0x1A, (char*) "-", 0, 0.75f, 0.75f);
    }
    DrawCodeArrows(sOnline.CodeDraft, kFirstSlotColumn, kSlotRow, kSlotSpacing, false);
    DrawError(0xE4);
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

/**
 * On a shared console: each player's controller number and name, along the bottom of Game Select. Four fit in a row;
 * with more than four, the first row moves up and the rest go below it.
 */
void DrawConsolePlayers() {
    constexpr size_t kPerRow = 4;
    const auto& members = Lobby::GetMembers();
    size_t shown = 0;
    for (size_t slot = 0; slot < members.size(); slot++) {
        shown += (members[slot].Present && (Netplay::GetConsolePlayer((u8) slot) >= 0)) ? 1 : 0;
    }
    const s32 firstRow = (shown > kPerRow) ? 0xDA : 0xE4;
    size_t index = 0;
    for (size_t slot = 0; slot < members.size(); slot++) {
        const s32 player = Netplay::GetConsolePlayer((u8) slot);
        if (!members[slot].Present || (player < 0)) {
            continue;
        }
        const std::string text = std::to_string(player + 1) + "P " + ToMenuText(members[slot].Name, 8);
        const s32 column = 0x15 + (s32) (index % kPerRow) * 0x48;
        const s32 row = firstRow + (s32) (index / kPerRow) * 0x0A;
        set_text_color(TEXT_GREEN);
        print_text_mode_1(column, row, (char*) text.c_str(), 0, 0.45f, 0.45f);
        index++;
    }
}

/**
 * On a private room's Game Select: its code, for the players to pass on to friends. Right of the LEAVE button, the one
 * free place on the screen: a small label over small arrows.
 */
void DrawRoomCode(const MenuItem* item) {
    constexpr s32 kSpacing = kSmallArrowSize + 1;
    if (!Lobby::IsRoomPrivate() || (item->param1 >= 0x20)) {
        return;
    }
    const s32 column = item->column + kButtonWidth + 6;
    set_text_color(TEXT_YELLOW);
    print_text_mode_1(column, item->row + 6, (char*) "ROOM CODE", 0, 0.4f, 0.4f);
    DrawCodeArrows(Lobby::GetRoomCode(), column, item->row + 8, kSpacing, true);
}

/**
 * Game Select in a room: why a choice will not start, where choosing it only makes the menu's "no" sound (menus.c).
 * Time trials race the ghosts in each player's own save, so the note shows while Time Trials is the mode chosen;
 * OPTION and DATA work on each player's own save and Controller Pak, so it shows for a while after L or R.
 */
void DrawUnavailableNote() {
    const char* text = nullptr;
    std::string placesText;
    if ((gMainMenuSelection == MAIN_MENU_MODE_SELECT) &&
        (gGameModePlayerSelection[gPlayerCount - 1][gGameModeMenuColumn[gPlayerCount - 1]] == TIME_TRIALS)) {
        text = "TIME TRIALS ARE NOT AVAILABLE ONLINE";
    } else if ((gMainMenuSelection == MAIN_MENU_PLAYER_SELECT) &&
               (std::chrono::steady_clock::now() - sOnline.OptionDataRefusedAt < kRefusedNoteTime)) {
        text = "OPTION AND DATA ARE NOT AVAILABLE ONLINE";
    } else if ((gMainMenuSelection == MAIN_MENU_PLAYER_SELECT) && (gPlayerCount > Netplay_GetConsolePlaces())) {
        placesText = "THIS ROOM HAS " + std::to_string(Netplay_GetConsolePlaces()) + " PLAYERS";
        text = placesText.c_str();
    }
    if (text != nullptr) {
        set_text_color(TEXT_YELLOW);
        print_text1_center_mode_1(0xA0, 0xC4, (char*) text, 0, 0.6f, 0.6f);
    }
}

} // namespace

extern "C" void online_menu_refuse_option_data(void) {
    sOnline.OptionDataRefusedAt = std::chrono::steady_clock::now();
}

extern "C" void online_menu_enter(void) {
    sOnline.Current = Screen::Rooms;
    sOnline.Cursor = 0;
    sOnline.FirstRoomShown = 0;
    RefreshRooms();
    PageVersion::Check();
}

extern "C" void online_menu_act(struct Controller* controller) {
    sOnline.RoomList.Update();
    const Lobby::State state = Lobby::GetState();
    const bool choosing = (state == Lobby::State::Offline) || (state == Lobby::State::Matchmaking);
    if (choosing && !sOnline.RoomList.IsBusy() &&
        (std::chrono::steady_clock::now() - sOnline.LastRefresh >= kRefreshInterval)) {
        RefreshRooms();
    }
    if (is_screen_being_faded()) {
        return;
    }
    u16 pressed = controller->buttonPressed | controller->stickPressed;
    if (!choosing) {
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
    if (sOnline.Current == Screen::Code) {
        ActCode(controller);
        return;
    }
    if (pressed & START_BUTTON) {
        pressed |= A_BUTTON;
    }
    sOnline.Cursor = std::min(sOnline.Cursor, GetLastItem());
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
            const auto away = std::find_if(members.begin(), members.end(),
                                           [](const Lobby::Member& member) { return member.Present && member.Away; });
            if (racing) {
                DrawStatus("WAITING FOR THE RACE TO END");
            } else if (away != members.end()) {
                // A player matched while racing offline
                DrawStatus(("WAITING FOR " + ToMenuText(away->Name, kMaxNameLength) + " TO FINISH A RACE").c_str());
            } else {
                DrawStatus("JOINING...");
            }
            return;
        }
        default:
            break;
    }
    if (sOnline.Current == Screen::Name) {
        DrawName(item);
    } else if (sOnline.Current == Screen::Code) {
        DrawCode();
    } else {
        DrawRooms(item);
        DrawVersionNote();
    }
}

extern "C" void render_online_button(MenuItem* item) {
    const bool console = Netplay_IsSharedConsole();
    if (console) {
        DrawUnavailableNote();
        DrawConsolePlayers();
        DrawRoomCode(item);
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
