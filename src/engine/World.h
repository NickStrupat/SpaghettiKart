#pragma once

#include <libultraship.h>

#include "CoreMath.h"
#include "engine/tracks/Track.h"
#include "engine/cameras/GameCamera.h"
#include "objects/Object.h"
#include "Cup.h"
#include "PlayerBombKart.h"
#include "TrainCrossing.h"
#include <memory>
#include <unordered_map>
#include <utility>
#include "RaceManager.h"
#include "Actor.h"
#include "StaticMeshActor.h"
#include "particles/ParticleEmitter.h"
#include "engine/sky/SkyCloud.h"

#include "editor/Editor.h"
#include "editor/GameObject.h"
#include "port/Game.h"

extern "C" {
#include "camera.h"
#include "objects.h"
};

class Cup; // <-- Forward declaration
class OObject;
class GameCamera;
class Track;
class StaticMeshActor;
class OBombKart;
class TrainCrossing;
class OLakitu;
class GameObject; // <-- Editor

class World {
typedef struct Matrix {
    Mtx Screen2D; // Orthogonal projection for UI, skybox, and such
    Mtx Ortho;
    std::array<Mtx,5> Persp;
    std::array<Mtx,5> LookAt;
    std::array<Mtx, 8 * 4> Karts; // Eight players * four screens
    std::array<Mtx, 8 * 4> Shadows; // Eight players * four screens
    std::deque<Mtx> Hud;
    std::deque<Mtx> Objects;

    Matrix()
        : Hud(200), Objects(1000)
    {}
} Matrix;
private:
    std::unique_ptr<Track> mTrack;
    Cup* CurrentCup;

public:
    static World* Instance;
    explicit World();
    ~World();

    RaceManager& GetRaceManager() { return *RaceManagerInstance; }
    void SetRaceManager(std::unique_ptr<RaceManager> manager) { RaceManagerInstance = std::move(manager); }

    void TickCameras();

    AActor* AddActor(std::unique_ptr<AActor> actor);
    struct Actor* AddBaseActor();
    void ActorBeginPlay(Actor* actor);
    AActor* GetActor(size_t index);

    void TickActors();
    void TickActors60fps();
    AActor* ConvertActorToAActor(Actor* actor);
    Actor* ConvertAActorToActor(AActor* actor);

    void DrawStaticMeshActors();
    StaticMeshActor* AddStaticMeshActor(const std::string& name, FVector pos, IRotator rot, FVector scale, const std::string& model, int32_t* collision);

    OObject* AddObject(std::unique_ptr<OObject> object);

    void TickObjects();
    void TickObjects60fps();
    void DrawObjects(s32 cameraId);
    Object *GetObjectByIndex(size_t);

    void TickParticles();
    void DrawParticles(s32 cameraId);
    ParticleEmitter* AddEmitter(std::unique_ptr<ParticleEmitter> emitter);
    void Reset(void); // Sets OObjects or AActors static member variables back to default values

    void AddCup(Cup* cup);
    void SetCurrentCup(Cup* cup);
    Cup* GetCurrentCup() {
        return CurrentCup;
    }
    void SetCupIndex(size_t index);
    const char* GetCupName();
    u32 GetCupIndex();
    u32 NextCup();
    u32 PreviousCup();

    World* GetWorld(void);
    void CleanWorld(void);
    void CleanActors(void);

    /**
     * Replaces the simulation containers with empty ones. Inside a sim heap scope, everything they allocate next
     * comes from the sim arena (see port/netplay/SimArena.h).
     */
    void ResetSimContainers();

    /**
     * Calls fn(void* data, size_t size) for each World member that a netplay rollback snapshot saves and restores.
     * The objects these containers own live in the sim arena.
     */
    template <typename Fn> void ForEachSimStateRange(Fn&& fn) {
        fn(&mTrack, sizeof(mTrack));
        fn(&Cameras, sizeof(Cameras));
        fn(&StaticMeshActors, sizeof(StaticMeshActors));
        fn(&Actors, sizeof(Actors));
        fn(&Objects, sizeof(Objects));
        fn(&Emitters, sizeof(Emitters));
        fn(&Lakitus, sizeof(Lakitus));
        fn(&mPlayerBombKart, sizeof(mPlayerBombKart));
        fn(&Crossings, sizeof(Crossings));
        // A Grand Prix moves on to its next course from within the race
        fn(&CurrentCup, sizeof(CurrentCup));
        fn(&CupIndex, sizeof(CupIndex));
        for (size_t i = 0; i < Cups.size(); i++) {
            fn(GetCupCursorPosition(i), sizeof(size_t));
        }
    }
    // Cup.h includes this header, so Cup is incomplete here.
    size_t* GetCupCursorPosition(size_t cupIndex);

    // getter/setter for current track
    Track* GetTrack() {
        return mTrack.get();
    }

    void SetCurrentTrack(std::unique_ptr<Track> track);

    Matrix world_mtx;

    std::vector<Cup*> Cups;
    size_t CupIndex = 1;

    std::vector<std::unique_ptr<GameCamera>> Cameras;

    std::vector<std::unique_ptr<StaticMeshActor>> StaticMeshActors;
    std::deque<std::unique_ptr<AActor>> Actors;
    std::deque<std::unique_ptr<OObject>> Objects;
    std::vector<std::unique_ptr<ParticleEmitter>> Emitters;

    std::unordered_map<s32, OLakitu*> Lakitus;

    /** Objects **/
    PlayerBombKart mPlayerBombKart[4]; // Used in battle mode

    TrainCrossing* AddCrossing(Vec3f position, u32 waypointMin, u32 waypointMax, f32 approachRadius, f32 exitRadius);
    std::vector<std::shared_ptr<TrainCrossing>> Crossings;
private:
    std::unique_ptr<RaceManager> RaceManagerInstance;
};
