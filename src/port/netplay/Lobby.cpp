#include "Lobby.h"

#include <algorithm>
#include <chrono>
#include <random>

#include "NetplayIdentity.h"
#include "RelayClient.h"

using namespace RelayProtocol;
using Clock = std::chrono::steady_clock;

namespace Lobby {
namespace {

// Each member re-sends its full state this often, which also replaces lost packets.
constexpr auto kBroadcastInterval = std::chrono::milliseconds(250);
// RaceEnded is sent once, not repeated; sending it a few times makes losing every copy unlikely.
constexpr int kRaceEndedRepeats = 3;
constexpr s8 kNumCharacters = BOWSER + 1;
constexpr size_t kMaxMessageSize = 512;

enum class MessageType : u8 {
    MemberState = 1,  // u8 character, u8 ready, u32 race running (0 for none), u8 name length, name
    RoomSettings = 2, // Host only: u8 mode, u8 cc, u8 cup, u8 track length, track, u32 race (0 for none),
                      // then for a race: u8 player count, the slot of each player, RaceConfig
    RaceEnded = 3,    // u32 race, u8 SessionResult, u8 message length, message
};

struct LobbyState {
    RelayClient Relay;
    State Status = State::Offline;
    std::string Error;
    std::string Name;
    std::array<Member, kMaxMembers> Members;
    Settings RoomSettings;
    s8 Character = 0;
    bool CharacterChosen = false;
    bool Ready = false;

    // The latest race the host started, which it keeps announcing so that every guest hears of it
    u32 RaceId = 0;
    RaceConfig Race;
    std::vector<u8> RaceSlots;
    // Guest: the latest race announced. Races already running when this peer joined, or announced again, are not
    // started. Unset until the host's settings first arrive.
    std::optional<u32> SeenRaceId;
    u32 RunningRaceId = 0;
    std::optional<RaceStart> PendingStart;
    std::optional<RaceEnd> EndedElsewhere;
    std::string LastRaceMessage;

    Clock::time_point LastBroadcast;
    bool Changed = true;
    std::mt19937 Random{ std::random_device{}() };
};

LobbyState sLobby;

bool IsLocalHost() {
    return (sLobby.Status == State::InRoom) && (sLobby.Relay.GetSlot() == 0);
}

// A guest counts as ready only once the host's settings have arrived: any race the host starts after hearing that
// is newer than every race this guest has seen, so the guest cannot mistake it for one that was already running.
bool IsReady() {
    return sLobby.Ready && sLobby.SeenRaceId.has_value();
}

void WriteString(ByteWriter& writer, const std::string& text, size_t maxLength) {
    const size_t length = std::min(text.size(), maxLength);
    writer.Write8((u8) length);
    writer.WriteBytes(text.data(), length);
}

std::string ReadString(ByteReader& reader) {
    std::string text(reader.Read8(), '\0');
    reader.ReadBytes(text.data(), text.size());
    return text;
}

void BroadcastMemberState() {
    u8 message[kMaxMessageSize];
    ByteWriter writer(message, sizeof(message));
    writer.Write8((u8) MessageType::MemberState);
    writer.Write8((u8) sLobby.Character);
    writer.Write8(IsReady() ? 1 : 0);
    writer.Write32(sLobby.RunningRaceId);
    WriteString(writer, sLobby.Name, kMaxNameLength);
    sLobby.Relay.SendLobby(kBroadcast, message, writer.GetSize());
}

void BroadcastRoomSettings() {
    u8 message[kMaxMessageSize];
    ByteWriter writer(message, sizeof(message));
    writer.Write8((u8) MessageType::RoomSettings);
    writer.Write8((u8) sLobby.RoomSettings.Mode);
    writer.Write8((u8) sLobby.RoomSettings.CC);
    writer.Write8((u8) sLobby.RoomSettings.CupIndex);
    WriteString(writer, sLobby.RoomSettings.TrackName, 255);
    writer.Write32(sLobby.RaceId);
    if (sLobby.RaceId != 0) {
        writer.Write8((u8) sLobby.RaceSlots.size());
        writer.WriteBytes(sLobby.RaceSlots.data(), sLobby.RaceSlots.size());
        sLobby.Race.Write(writer);
    }
    sLobby.Relay.SendLobby(kBroadcast, message, writer.GetSize());
}

void HandleMemberState(u8 source, ByteReader& reader) {
    Member member;
    member.Character = (s8) reader.Read8();
    member.Ready = reader.Read8() != 0;
    member.InRace = reader.Read32() != 0;
    member.Name = ReadString(reader);
    if (reader.Failed() || (source >= kMaxMembers) || (member.Character < 0) || (member.Character >= kNumCharacters)) {
        return;
    }
    member.Present = true;
    member.Known = true;
    sLobby.Members[source] = member;
}

void HandleRaceAnnouncement(u32 raceId, ByteReader& reader) {
    if (!sLobby.SeenRaceId.has_value()) {
        // Joined while this race was already starting or running.
        sLobby.SeenRaceId = raceId;
        return;
    }
    if ((raceId == 0) || (raceId <= *sLobby.SeenRaceId)) {
        return;
    }
    sLobby.SeenRaceId = raceId;

    RaceStart start;
    start.Id = raceId;
    start.Slots.resize(reader.Read8());
    reader.ReadBytes(start.Slots.data(), start.Slots.size());
    const bool valid = start.Config.Read(reader) && ((size_t) start.Config.PlayerCount == start.Slots.size());
    const auto own = std::find(start.Slots.begin(), start.Slots.end(), (u8) sLobby.Relay.GetSlot());
    if (own == start.Slots.end()) {
        return; // Joined after the host started it
    }
    if (!valid) {
        ReportRaceEnded(raceId, SessionResult::Failed, "Could not load the host's race; are the game files the same?");
        return;
    }
    start.LocalPlayer = (s32) (own - start.Slots.begin());
    sLobby.PendingStart = start;
    sLobby.Ready = false;
    sLobby.Changed = true;
}

void HandleRoomSettings(u8 source, ByteReader& reader) {
    if ((source != 0) || IsLocalHost()) {
        return;
    }
    Settings settings;
    settings.Mode = reader.Read8();
    settings.CC = reader.Read8();
    settings.CupIndex = reader.Read8();
    settings.TrackName = ReadString(reader);
    const u32 raceId = reader.Read32();
    const bool validMode = (settings.Mode == GRAND_PRIX) || (settings.Mode == VERSUS) || (settings.Mode == BATTLE);
    if (reader.Failed() || !validMode || (settings.CC < CC_50) || (settings.CC > CC_EXTRA)) {
        return;
    }
    sLobby.RoomSettings = settings;
    HandleRaceAnnouncement(raceId, reader);
}

void HandleRaceEnded(u8 source, ByteReader& reader) {
    const u32 raceId = reader.Read32();
    const SessionResult result = (SessionResult) reader.Read8();
    const std::string message = ReadString(reader);
    if (reader.Failed() || (raceId == 0) || (source >= kMaxMembers)) {
        return;
    }
    if ((raceId == sLobby.RunningRaceId) || (sLobby.PendingStart.has_value() && (sLobby.PendingStart->Id == raceId))) {
        const std::string& name = sLobby.Members[source].Name;
        sLobby.EndedElsewhere = RaceEnd{ raceId, IsSuccess(result) ? result : SessionResult::PeerLeft,
                                         IsSuccess(result) ? message : name + ": " + message };
        sLobby.PendingStart.reset();
    }
}

void HandleMessage(const RelayClient::LobbyPacket& packet) {
    ByteReader reader(packet.Data.data(), packet.Data.size());
    switch ((MessageType) reader.Read8()) {
        case MessageType::MemberState:
            HandleMemberState(packet.Source, reader);
            break;
        case MessageType::RoomSettings:
            HandleRoomSettings(packet.Source, reader);
            break;
        case MessageType::RaceEnded:
            HandleRaceEnded(packet.Source, reader);
            break;
        default:
            break;
    }
}

void UpdateMembers() {
    const u8 occupied = sLobby.Relay.GetOccupiedSlots();
    for (size_t slot = 0; slot < kMaxMembers; slot++) {
        if ((occupied & (1 << slot)) == 0) {
            sLobby.Members[slot] = Member{};
        } else {
            sLobby.Members[slot].Present = true;
        }
    }
    Member& self = sLobby.Members[sLobby.Relay.GetSlot()];
    self.Known = true;
    self.Name = sLobby.Name;
    self.Character = sLobby.Character;
    self.Ready = IsReady();
    self.InRace = sLobby.RunningRaceId != 0;
}

void ResetRoom() {
    sLobby.Members = {};
    sLobby.RoomSettings = Settings{};
    sLobby.CharacterChosen = false;
    sLobby.Ready = false;
    sLobby.RaceId = 0;
    sLobby.SeenRaceId.reset();
    sLobby.PendingStart.reset();
    sLobby.Changed = true;
}

void Connect(const std::string& name) {
    ResetRoom();
    sLobby.Error.clear();
    sLobby.LastRaceMessage.clear();
    sLobby.Name = name.substr(0, kMaxNameLength);
    sLobby.Status = State::Connecting;
}

} // namespace

void Host(const std::string& relayAddress, const std::string& name) {
    Connect(name);
    sLobby.Relay.Create(relayAddress, MakeBuildId(NetplayIdentity::GetBuildId()),
                        NetplayIdentity::ComputeContentHash());
}

void Join(const std::string& relayAddress, const std::string& code, const std::string& name) {
    Connect(name);
    sLobby.Relay.Join(relayAddress, code, MakeBuildId(NetplayIdentity::GetBuildId()),
                      NetplayIdentity::ComputeContentHash());
}

void Leave() {
    sLobby.Relay.Leave();
    sLobby.Status = State::Offline;
    if (sLobby.RunningRaceId != 0) {
        sLobby.EndedElsewhere = RaceEnd{ sLobby.RunningRaceId, SessionResult::PeerLeft, "You left the room" };
    }
    ResetRoom();
}

void Update() {
    if (sLobby.Status == State::Offline) {
        return;
    }
    sLobby.Relay.Update();
    switch (sLobby.Relay.GetStatus()) {
        case RelayClient::Status::Failed:
        case RelayClient::Status::Idle:
            sLobby.Error = sLobby.Relay.GetError();
            if (sLobby.RunningRaceId != 0) {
                sLobby.EndedElsewhere = RaceEnd{ sLobby.RunningRaceId, SessionResult::PeerLeft, sLobby.Error };
            }
            sLobby.Status = State::Offline;
            ResetRoom();
            return;
        case RelayClient::Status::InRoom:
            if (sLobby.Status == State::Connecting) {
                sLobby.Status = State::InRoom;
                if (IsLocalHost()) {
                    sLobby.SeenRaceId = 0;
                }
            }
            break;
        default:
            return;
    }

    if (!sLobby.CharacterChosen) {
        // Everyone starts on a different character.
        sLobby.Character = (s8) sLobby.Relay.GetSlot();
    }
    for (const RelayClient::LobbyPacket& packet : sLobby.Relay.TakeLobbyPackets()) {
        HandleMessage(packet);
    }
    UpdateMembers();

    const Clock::time_point now = Clock::now();
    if (sLobby.Changed || (now - sLobby.LastBroadcast >= kBroadcastInterval)) {
        BroadcastMemberState();
        if (IsLocalHost()) {
            BroadcastRoomSettings();
        }
        sLobby.LastBroadcast = now;
        sLobby.Changed = false;
    }
}

State GetState() {
    return sLobby.Status;
}

const std::string& GetError() {
    return sLobby.Error;
}

std::string GetRoomCode() {
    return sLobby.Relay.GetRoomCode();
}

s32 GetLocalSlot() {
    return sLobby.Relay.GetSlot();
}

bool IsHost() {
    return IsLocalHost();
}

s32 GetPingMs() {
    return sLobby.Relay.GetPingMs();
}

const std::array<Member, kMaxMembers>& GetMembers() {
    return sLobby.Members;
}

const Settings& GetSettings() {
    return sLobby.RoomSettings;
}

void SetSettings(const Settings& settings) {
    if (IsLocalHost()) {
        sLobby.RoomSettings = settings;
        sLobby.Changed = true;
    }
}

void SetName(const std::string& name) {
    sLobby.Name = name.substr(0, kMaxNameLength);
    sLobby.Changed = true;
}

void SetCharacter(s8 character) {
    if ((character >= 0) && (character < kNumCharacters)) {
        sLobby.Character = character;
        sLobby.CharacterChosen = true;
        sLobby.Changed = true;
    }
}

void SetReady(bool ready) {
    sLobby.Changed |= (sLobby.Ready != ready);
    sLobby.Ready = ready;
}

std::string GetStartBlocker() {
    if (!IsLocalHost()) {
        return "Only the host can start a race";
    }
    if ((sLobby.RunningRaceId != 0) || sLobby.PendingStart.has_value()) {
        return "A race is running";
    }
    size_t count = 0;
    std::array<bool, kNumCharacters> taken = {};
    for (size_t slot = 0; slot < kMaxMembers; slot++) {
        const Member& member = sLobby.Members[slot];
        if (!member.Present) {
            continue;
        }
        count++;
        const std::string name = member.Name.empty() ? "Player " + std::to_string(slot + 1) : member.Name;
        if (!member.Known) {
            return "Waiting for " + name + " to connect";
        }
        if (member.InRace) {
            return name + " is still in the last race";
        }
        if ((slot != 0) && !member.Ready) {
            return "Waiting for " + name + " to get ready";
        }
        if ((member.Character < 0) || (member.Character >= kNumCharacters) || taken[member.Character]) {
            return name + " needs to pick another character";
        }
        taken[member.Character] = true;
    }
    if (count < 2) {
        return "Waiting for another player to join";
    }
    if ((sLobby.RoomSettings.Mode == GRAND_PRIX) && (count > 2)) {
        return "Grand Prix allows at most 2 players";
    }
    return "";
}

void StartRace() {
    if (!GetStartBlocker().empty()) {
        return;
    }
    const Settings& settings = sLobby.RoomSettings;
    RaceConfig config = RaceConfig::FromLocalCVars();
    config.ModeSelection = settings.Mode;
    config.CCSelection = settings.CC;
    config.MirrorMode = ((settings.Mode != BATTLE) && (settings.CC == CC_EXTRA)) ? 1 : 0;
    config.CupIndex = settings.CupIndex;
    config.CourseIndexInCup = 0;
    if (settings.Mode != GRAND_PRIX) {
        config.TrackName = settings.TrackName;
    }

    std::vector<u8> slots;
    std::array<bool, kNumCharacters> taken = {};
    for (size_t slot = 0; slot < kMaxMembers; slot++) {
        if (sLobby.Members[slot].Present) {
            config.Characters[slots.size()] = sLobby.Members[slot].Character;
            taken[sLobby.Members[slot].Character] = true;
            slots.push_back((u8) slot);
        }
    }
    config.PlayerCount = (s8) slots.size();
    // The game reads every entry; give unused ones the characters nobody picked.
    s8 next = 0;
    for (size_t i = slots.size(); i < config.Characters.size(); i++) {
        while (taken[next]) {
            next++;
        }
        config.Characters[i] = next;
        taken[next] = true;
    }
    config.RandomSeed = (u16) (sLobby.Random() % 0xFFFF + 1);

    sLobby.RaceId++;
    sLobby.SeenRaceId = sLobby.RaceId;
    sLobby.Race = config;
    sLobby.RaceSlots = slots;
    sLobby.PendingStart = RaceStart{ sLobby.RaceId, config, 0, slots };
    sLobby.Changed = true;
}

std::optional<RaceStart> TakeRaceStart() {
    std::optional<RaceStart> start;
    start.swap(sLobby.PendingStart);
    if (start.has_value()) {
        sLobby.RunningRaceId = start->Id;
        sLobby.LastRaceMessage.clear();
        sLobby.Changed = true;
    }
    return start;
}

GekkoNetAdapter* GetGekkoAdapter(u32 raceId) {
    return sLobby.Relay.GetGekkoAdapter((u16) raceId);
}

void ReportRaceEnded(u32 raceId, SessionResult result, const std::string& message) {
    if (sLobby.RunningRaceId == raceId) {
        sLobby.RunningRaceId = 0;
        sLobby.Changed = true;
    }
    if (sLobby.Status != State::InRoom) {
        return;
    }
    u8 data[kMaxMessageSize];
    ByteWriter writer(data, sizeof(data));
    writer.Write8((u8) MessageType::RaceEnded);
    writer.Write32(raceId);
    writer.Write8((u8) result);
    WriteString(writer, message, 255);
    for (int i = 0; i < kRaceEndedRepeats; i++) {
        sLobby.Relay.SendLobby(kBroadcast, data, writer.GetSize());
    }
}

std::optional<RaceEnd> TakeRaceEndedElsewhere() {
    std::optional<RaceEnd> end;
    end.swap(sLobby.EndedElsewhere);
    return end;
}

const std::string& GetLastRaceMessage() {
    return sLobby.LastRaceMessage;
}

void SetLastRaceMessage(const std::string& message) {
    sLobby.LastRaceMessage = message;
}

} // namespace Lobby
