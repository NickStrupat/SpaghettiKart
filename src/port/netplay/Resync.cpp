#include "Resync.h"

#include <algorithm>
#include <cstring>

#include <zlib.h>

#include "SimArena.h"
#include "tools/netplay-relay/RelayProtocol.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace {

using RelayProtocol::ByteReader;
using RelayProtocol::ByteWriter;

// "SKRS"
constexpr u32 kPackedMagic = 0x534B5253;
// Bytes of packed state per chunk. With the chunk's and the lobby message's headers, a chunk fits the relay's payload.
constexpr size_t kChunkSize = 1024;
constexpr size_t kChunkHeaderSize = 13;
static_assert(kChunkSize + kChunkHeaderSize + 5 <= RelayProtocol::kMaxPayload);
// A pull asks for this many chunks, and for the missing ones again after this long.
constexpr u16 kChunksPerPull = 32;
constexpr auto kPullRetry = std::chrono::milliseconds(150);
// A packed state larger than this is damaged; a real one is under 1 MB unpacked.
constexpr u32 kMaxUnpackedSize = 64 * 1024 * 1024;

enum class Region : u8 { Section, Arena, World };

struct HeapSlot {
    Region Where;
    u32 Offset;
};

std::vector<u8>& GetRegion(SimSnapshot& snapshot, Region region) {
    switch (region) {
        case Region::Section:
            return snapshot.Section;
        case Region::Arena:
            return snapshot.Arena;
        default:
            return snapshot.World;
    }
}

const std::vector<u8>& GetRegion(const SimSnapshot& snapshot, Region region) {
    return GetRegion(const_cast<SimSnapshot&>(snapshot), region);
}

#if defined(__APPLE__)
bool IsMallocTag(unsigned int tag) {
    switch (tag) {
        case VM_MEMORY_MALLOC:
        case VM_MEMORY_MALLOC_SMALL:
        case VM_MEMORY_MALLOC_LARGE:
        case VM_MEMORY_MALLOC_HUGE:
        case VM_MEMORY_REALLOC:
        case VM_MEMORY_MALLOC_TINY:
        case VM_MEMORY_MALLOC_LARGE_REUSABLE:
        case VM_MEMORY_MALLOC_LARGE_REUSED:
        case VM_MEMORY_MALLOC_NANO:
        case VM_MEMORY_MALLOC_MEDIUM:
            return true;
        default:
            return false;
    }
}
#endif

/** Finds whether values point into the regular heap, remembering the last memory region: pointers come in runs. */
class HeapTest {
  public:
    bool IsHeapPointer(u64 value) {
#if defined(__APPLE__)
        // Below and above every address a process can use
        if ((value < 0x100000000ULL) || (value >= 0x800000000000ULL)) {
            return false;
        }
        if ((value >= mStart) && (value < mEnd)) {
            return mHeap;
        }
        mach_vm_address_t address = value;
        mach_vm_size_t size = 0;
        vm_region_extended_info_data_t info = {};
        mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &address, &size, VM_REGION_EXTENDED_INFO, (vm_region_info_t) &info,
                           &count, &object) != KERN_SUCCESS) {
            return false;
        }
        if (address > value) {
            // Not mapped: remember the gap up to the next region.
            mStart = value;
            mEnd = address;
            mHeap = false;
            return false;
        }
        mStart = address;
        mEnd = address + size;
        mHeap = IsMallocTag(info.user_tag);
        return mHeap;
#else
        (void) value;
        return false;
#endif
    }

  private:
    u64 mStart = 0;
    u64 mEnd = 0;
    bool mHeap = false;
};

void WriteMessageHeader(ByteWriter& writer, Resync::MessageKind kind, u32 segment) {
    writer.Write8((u8) kind);
    writer.Write32(segment);
}

} // namespace

namespace Resync {

std::vector<u8> MakeRequest(u32 segment) {
    std::vector<u8> message(5);
    ByteWriter writer(message.data(), message.size());
    WriteMessageHeader(writer, MessageKind::Request, segment);
    return message;
}

bool ReadMessage(const std::vector<u8>& bytes, Message* message) {
    ByteReader reader(bytes.data(), bytes.size());
    message->Kind = (MessageKind) reader.Read8();
    message->Segment = reader.Read32();
    switch (message->Kind) {
        case MessageKind::Request:
            break;
        case MessageKind::Pull:
            message->Index = reader.Read16();
            message->Count = reader.Read16();
            break;
        case MessageKind::Chunk:
            message->Frame = (s32) reader.Read32();
            message->Index = reader.Read16();
            message->Count = reader.Read16();
            message->Data.assign(reader.GetRemaining(), reader.GetRemaining() + reader.GetRemainingSize());
            break;
        default:
            return false;
    }
    return !reader.Failed();
}

bool IsAvailable(std::string* reason) {
#if defined(__APPLE__)
    if (_dyld_get_image_vmaddr_slide(0) != 0) {
        *reason = "the game runs with address space layout randomization";
        return false;
    }
    return true;
#else
    *reason = "desync recovery needs macOS";
    return false;
#endif
}

std::vector<u8> Pack(const SimSnapshot& snapshot, u32 checksum) {
    std::vector<HeapSlot> slots;
    HeapTest heap;
    for (Region region : { Region::Section, Region::Arena, Region::World }) {
        const std::vector<u8>& bytes = GetRegion(snapshot, region);
        for (size_t offset = 0; offset + sizeof(u64) <= bytes.size(); offset += sizeof(u64)) {
            u64 value;
            memcpy(&value, bytes.data() + offset, sizeof(value));
            if (heap.IsHeapPointer(value)) {
                slots.push_back({ region, (u32) offset });
            }
        }
    }

    // Magic, image and arena addresses, checksum, region sizes, heap slots, then the regions themselves
    const size_t headerSize = 4 + 8 + 8 + 4 + 3 * 4 + 4 + slots.size() * 5;
    std::vector<u8> unpacked(headerSize + snapshot.Section.size() + snapshot.Arena.size() + snapshot.World.size());
    ByteWriter writer(unpacked.data(), unpacked.size());
    writer.Write32(kPackedMagic);
    writer.Write64(SimState_GetImageAddress());
    writer.Write64((u64) (uintptr_t) SimArena::GetBase());
    writer.Write32(checksum);
    writer.Write32((u32) snapshot.Section.size());
    writer.Write32((u32) snapshot.Arena.size());
    writer.Write32((u32) snapshot.World.size());
    writer.Write32((u32) slots.size());
    for (const HeapSlot& slot : slots) {
        writer.Write8((u8) slot.Where);
        writer.Write32(slot.Offset);
    }
    writer.WriteBytes(snapshot.Section.data(), snapshot.Section.size());
    writer.WriteBytes(snapshot.Arena.data(), snapshot.Arena.size());
    writer.WriteBytes(snapshot.World.data(), snapshot.World.size());

    // The unpacked size, then the zlib stream. A state is mostly zeros and repeats: 560 KB packs into about 70 KB.
    uLongf packedSize = compressBound((uLong) unpacked.size());
    std::vector<u8> packed(4 + packedSize);
    ByteWriter sizeWriter(packed.data(), 4);
    sizeWriter.Write32((u32) unpacked.size());
    if (compress2(packed.data() + 4, &packedSize, unpacked.data(), (uLong) unpacked.size(), Z_BEST_SPEED) != Z_OK) {
        return {};
    }
    packed.resize(4 + packedSize);
    return packed;
}

bool Unpack(const std::vector<u8>& packed, const SimSnapshot& own, SimSnapshot* state, u32* checksum,
            std::string* error) {
    ByteReader sizeReader(packed.data(), packed.size());
    const u32 unpackedSize = sizeReader.Read32();
    if (sizeReader.Failed() || (unpackedSize > kMaxUnpackedSize)) {
        *error = "the host's state arrived damaged";
        return false;
    }
    std::vector<u8> unpacked(unpackedSize);
    uLongf size = unpackedSize;
    if ((uncompress(unpacked.data(), &size, packed.data() + 4, (uLong) (packed.size() - 4)) != Z_OK) ||
        (size != unpackedSize)) {
        *error = "the host's state arrived damaged";
        return false;
    }

    ByteReader reader(unpacked.data(), unpacked.size());
    const u32 magic = reader.Read32();
    const u64 imageAddress = reader.Read64();
    const u64 arenaAddress = reader.Read64();
    *checksum = reader.Read32();
    const u32 sectionSize = reader.Read32();
    const u32 arenaSize = reader.Read32();
    const u32 worldSize = reader.Read32();
    std::vector<HeapSlot> slots(reader.Read32());
    if (reader.Failed() || (magic != kPackedMagic) || (slots.size() > unpackedSize / 5)) {
        *error = "the host's state arrived damaged";
        return false;
    }
    if (imageAddress != SimState_GetImageAddress()) {
        *error = "the host's game and this one are loaded at different addresses";
        return false;
    }
    if (arenaAddress != (u64) (uintptr_t) SimArena::GetBase()) {
        *error = "the host's sim arena and this one are at different addresses";
        return false;
    }
    if ((sectionSize != own.Section.size()) || (worldSize != own.World.size())) {
        *error = "the host's state has another layout";
        return false;
    }
    for (HeapSlot& slot : slots) {
        slot.Where = (Region) reader.Read8();
        slot.Offset = reader.Read32();
    }
    state->Section.resize(sectionSize);
    state->Arena.resize(arenaSize);
    state->World.resize(worldSize);
    reader.ReadBytes(state->Section.data(), sectionSize);
    reader.ReadBytes(state->Arena.data(), arenaSize);
    reader.ReadBytes(state->World.data(), worldSize);
    if (reader.Failed()) {
        *error = "the host's state arrived damaged";
        return false;
    }

    // The host's heap pointers mean nothing here: keep this player's own, which point to its copy of the same thing.
    HeapTest heap;
    for (const HeapSlot& slot : slots) {
        if (slot.Where > Region::World) {
            *error = "the host's state arrived damaged";
            return false;
        }
        std::vector<u8>& target = GetRegion(*state, slot.Where);
        const std::vector<u8>& mine = GetRegion(own, slot.Where);
        if (slot.Offset + sizeof(u64) > target.size()) {
            *error = "the host's state arrived damaged";
            return false;
        }
        u64 theirs;
        u64 ours = 0;
        memcpy(&theirs, target.data() + slot.Offset, sizeof(theirs));
        if (slot.Offset + sizeof(u64) <= mine.size()) {
            memcpy(&ours, mine.data() + slot.Offset, sizeof(ours));
        }
        if (theirs == ours) {
            continue; // Not a pointer after all, but data both have
        }
        if (!heap.IsHeapPointer(ours)) {
            *error = "this game's state is too far from the host's";
            return false;
        }
        memcpy(target.data() + slot.Offset, &ours, sizeof(ours));
    }
    return true;
}

Server::Server(u32 segment, s32 frame, std::vector<u8> packed)
    : mSegment(segment), mFrame(frame), mPacked(std::move(packed)),
      mNumChunks((u16) ((mPacked.size() + kChunkSize - 1) / kChunkSize)) {
}

void Server::HandlePull(s32 player, const Message& pull, const SendFunction& send) const {
    const u16 end = (u16) std::min<u32>((u32) pull.Index + pull.Count, mNumChunks);
    for (u16 index = pull.Index; index < end; index++) {
        const size_t start = (size_t) index * kChunkSize;
        const size_t size = std::min(kChunkSize, mPacked.size() - start);
        std::vector<u8> message(kChunkHeaderSize + size);
        ByteWriter writer(message.data(), message.size());
        WriteMessageHeader(writer, MessageKind::Chunk, mSegment);
        writer.Write32((u32) mFrame);
        writer.Write16(index);
        writer.Write16(mNumChunks);
        writer.WriteBytes(mPacked.data() + start, size);
        send(player, message);
    }
}

Collector::Collector(u32 segment) : mSegment(segment) {
}

void Collector::Add(const Message& chunk) {
    if (mChunks.empty() && (chunk.Count > 0)) {
        mChunks.resize(chunk.Count);
        mFrame = chunk.Frame;
    }
    if ((chunk.Count != mChunks.size()) || (chunk.Frame != mFrame) || (chunk.Index >= mChunks.size()) ||
        chunk.Data.empty() || !mChunks[chunk.Index].empty()) {
        return;
    }
    mChunks[chunk.Index] = chunk.Data;
    mNumReceived++;
}

void Collector::Update(const SendFunction& send) {
    if (IsComplete()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    bool lastArrived = mPulled && !mChunks.empty();
    for (size_t index = 0; lastArrived && (index < mPullEnd); index++) {
        lastArrived = !mChunks[index].empty();
    }
    if (mPulled && !lastArrived && (now - mPulledAt < kPullRetry)) {
        return;
    }
    // The first missing chunk on; before any chunk has arrived, the host's chunk count is not known yet.
    u16 first = 0;
    while ((first < mChunks.size()) && !mChunks[first].empty()) {
        first++;
    }
    const u16 count = mChunks.empty() ? kChunksPerPull : (u16) std::min<size_t>(kChunksPerPull, mChunks.size() - first);
    std::vector<u8> message(9);
    ByteWriter writer(message.data(), message.size());
    WriteMessageHeader(writer, MessageKind::Pull, mSegment);
    writer.Write16(first);
    writer.Write16(count);
    send(0, message);
    mPullEnd = (u16) (first + count);
    mPulledAt = now;
    mPulled = true;
}

bool Collector::IsComplete() const {
    return !mChunks.empty() && (mNumReceived == mChunks.size());
}

s32 Collector::GetFrame() const {
    return mFrame;
}

std::vector<u8> Collector::TakePacked() {
    std::vector<u8> packed;
    for (std::vector<u8>& chunk : mChunks) {
        packed.insert(packed.end(), chunk.begin(), chunk.end());
    }
    mChunks.clear();
    mNumReceived = 0;
    return packed;
}

} // namespace Resync
