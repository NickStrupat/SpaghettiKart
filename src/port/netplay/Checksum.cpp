#include "Checksum.h"

#include <cstddef>

#include "port/Game.h"

extern "C" {
#include "main.h"
#include "buffers.h"
#include "camera.h"
#include "objects.h"
#include "actor_types.h"
#include "code_800029B0.h"
#include "menu_items.h"
}

namespace {

// 64-bit FNV-1a
constexpr u64 kFnvOffsetBasis = 0xCBF29CE484222325ULL;
constexpr u64 kFnvPrime = 0x100000001B3ULL;

void HashBytes(u64& hash, const void* data, size_t size) {
    const u8* bytes = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
}

template <typename T> void HashValue(u64& hash, const T& value) {
    HashBytes(hash, &value, sizeof(T));
}

// Object stores texture, model and spline pointers in its middle; hash the pointer-free ranges around them.
void HashObject(u64& hash, const Object& object) {
    HashBytes(hash, &object, offsetof(Object, activeTLUT));
    HashBytes(hash, object.unk_078, sizeof(object.unk_078));

    const size_t tailStart = offsetof(Object, unk_084);
    const size_t tailEnd = offsetof(Object, unk_0DF) + sizeof(object.unk_0DF);
    HashBytes(hash, reinterpret_cast<const u8*>(&object) + tailStart, tailEnd - tailStart);
}

// Everything before the model pointer is plain data, except RailroadCrossing, whose crossingTrigger pointer
// overlays unk_08 and boundingBoxSize.
void HashActor(u64& hash, const struct Actor& actor) {
    const u8* bytes = reinterpret_cast<const u8*>(&actor);
    if (actor.type == ACTOR_RAILROAD_CROSSING) {
        HashBytes(hash, bytes, offsetof(struct Actor, unk_08));
        HashBytes(hash, bytes + offsetof(struct Actor, rot), offsetof(struct Actor, model) - offsetof(struct Actor, rot));
    } else {
        HashBytes(hash, bytes, offsetof(struct Actor, model));
    }
}

void HashCamera(u64& hash, const Camera& camera) {
    HashValue(hash, camera.pos);
    HashValue(hash, camera.lookAt);
    HashValue(hash, camera.up);
    HashValue(hash, camera.rot);
    HashValue(hash, camera.fieldOfView);
    HashValue(hash, camera.playerId);
}

/**
 * A menu item's state, without its texture slot and the fields add_menu_item() leaves as they were: those depend on
 * what each peer drew before the race (the title screen, other menus), and a free slot keeps its old item's values.
 */
void HashMenuItem(u64& hash, const MenuItem& item) {
    HashValue(hash, item.type);
    HashValue(hash, item.state);
    HashValue(hash, item.subState);
    HashValue(hash, item.column);
    HashValue(hash, item.row);
    HashValue(hash, item.priority);
    HashValue(hash, item.visible);
    HashValue(hash, item.param1);
    HashValue(hash, item.param2);
}

/**
 * The race's own flow: its state, the results and their menu items, and leaving it. The menu items advance in draw
 * code, and the results end the race, so they must agree too.
 */
void HashRaceFlow(u64& hash) {
    HashValue(hash, gRaceState);
    HashValue(hash, D_8015F894);
    HashValue(hash, gIsInQuitToMenuTransition);
    HashValue(hash, gQuitToMenuTransitionCounter);
    HashValue(hash, gGotoMode);
    HashValue(hash, D_8018D9D8);
    HashValue(hash, D_8018D9D9);
    for (size_t i = 0; i < MENU_ITEMS_MAX; i++) {
        if (gMenuItems[i].type != 0) {
            // As 8 bytes everywhere: size_t is 4 in the browser build, where the checksum must come out the same.
            HashValue(hash, (u64) i);
            HashMenuItem(hash, gMenuItems[i]);
        }
    }
}

} // namespace

extern "C" void Sim_WriteChecksumBreakdown(FILE* out) {
    u64 hash;
    fprintf(out, "rng %04x timer %08x\n", gRandomSeed16, *reinterpret_cast<const u32*>(&gCourseTimer));
    fprintf(out, "flow race %d results %u leaving %u/%u goto %d confirm %d/%d\n", gRaceState, D_8015F894,
            gIsInQuitToMenuTransition, gQuitToMenuTransitionCounter, gGotoMode, D_8018D9D8, D_8018D9D9);
    for (size_t i = 0; i < MENU_ITEMS_MAX; i++) {
        const MenuItem& item = gMenuItems[i];
        if (item.type != 0) {
            fprintf(out, "menu %zu type %d state %d substate %d at %d %d priority %u visible %u param %d %d\n", i,
                    item.type, item.state, item.subState, item.column, item.row, item.priority, item.visible,
                    item.param1, item.param2);
        }
    }

    for (size_t i = 0; i < NUM_PLAYERS; i++) {
        hash = kFnvOffsetBasis;
        HashBytes(hash, &gPlayers[i], sizeof(Player));
        fprintf(out, "player %zu %016llx\n", i, (unsigned long long) hash);
    }

    const size_t actorCount = CM_GetActorSize();
    fprintf(out, "actors %zu\n", actorCount);
    for (size_t i = 0; i < actorCount; i++) {
        hash = kFnvOffsetBasis;
        struct Actor* actor = CM_GetActor(i);
        HashActor(hash, *actor);
        fprintf(out, "actor %zu type %d %016llx\n", i, actor->type, (unsigned long long) hash);
    }

    for (size_t i = 0; i < OBJECT_LIST_SIZE; i++) {
        hash = kFnvOffsetBasis;
        HashObject(hash, gObjectList[i]);
        fprintf(out, "object %zu type %d %016llx\n", i, gObjectList[i].type, (unsigned long long) hash);
    }

    for (size_t i = 0; i < NUM_CAMERAS; i++) {
        hash = kFnvOffsetBasis;
        HashCamera(hash, cameras[i]);
        fprintf(out, "camera %zu %016llx\n", i, (unsigned long long) hash);
    }
}

extern "C" u64 Sim_GameplayChecksum(void) {
    u64 hash = kFnvOffsetBasis;

    HashValue(hash, gRandomSeed16);
    HashValue(hash, gCourseTimer);
    HashRaceFlow(hash);

    // Player has no pointers and its padding is spelled out as unk_ fields.
    HashBytes(hash, gPlayers, sizeof(Player) * NUM_PLAYERS);

    const u64 actorCount = CM_GetActorSize();
    HashValue(hash, actorCount);
    for (size_t i = 0; i < actorCount; i++) {
        HashActor(hash, *CM_GetActor(i));
    }

    for (size_t i = 0; i < OBJECT_LIST_SIZE; i++) {
        HashObject(hash, gObjectList[i]);
    }

    for (size_t i = 0; i < NUM_CAMERAS; i++) {
        HashCamera(hash, cameras[i]);
    }

    return hash;
}
