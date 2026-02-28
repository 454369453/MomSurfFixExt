#include "extension.h"


#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <dlfcn.h>
#include <algorithm>
#include <cmath>


#include <tier0/platform.h>
#include <tier0/memalloc.h>
#include <tier1/convar.h>
#include <gametrace.h>
#include <soundflags.h>
#include <ihandleentity.h>
#if __has_include(<interfaces/interfaces.h>)
#include <interfaces/interfaces.h>
#else
#include <tier1/interface.h>
#endif

class CBasePlayer;
class CBaseEntity;

enum PLAYER_ANIM 
{ 
    PLAYER_IDLE = 0, PLAYER_WALK, PLAYER_JUMP, PLAYER_SUPERJUMP, PLAYER_DIE, PLAYER_ATTACK1
};

#include <engine/IEngineTrace.h>
#include <ispatialpartition.h>
#include <igamemovement.h>
#include <tier0/vprof.h>
#include "simple_detour.h"

// Global variables
#ifndef MAXPLAYERS
#define MAXPLAYERS 65
#endif

MomSurfFixExt g_MomSurfFixExt;

SDKExtension *g_pExtensionIface = &g_MomSurfFixExt;

IEngineTrace *enginetrace = nullptr;
typedef void* (*CreateInterfaceFn)(const char *pName, int *pReturnCode);

// Forward declaration for callback
void OnEnableChanged(IConVar *var, const char *pOldValue, float flOldValue);

// ConVars
ConVar g_cvEnable("momsurffix_enable", "1", 0, "Enable Surf Bug Fix", OnEnableChanged);
ConVar g_cvDebug("momsurffix_debug", "0", 0, "Print debug info");

// Sensitivity threshold
ConVar g_cvSensitivity("momsurffix_sensitivity", "0.97", 0, "Sensitivity threshold for speed loss detection (0.90 - 0.99)");

ConVar g_cvRampNormalZ("momsurffix_ramp_normalz", "0.7", 0, "Slope normal Z threshold. Surfaces steeper than this (lower Z) are treated as ramps/walls. (Default 0.7 ~= 45 deg)");

ConVar g_cvAdaptiveMode("momsurffix_adaptive_mode", "1", 0, "Adaptive sensitivity mode (0=fixed, 1=speed-based)");
ConVar g_cvSpeedThresholdLow("momsurffix_speed_threshold_low", "500.0", 0, "Speed threshold for low-speed mode (units/sec)");
ConVar g_cvSpeedThresholdHigh("momsurffix_speed_threshold_high", "1000.0", 0, "Speed threshold for high-speed mode (units/sec)");
ConVar g_cvSensitivityLow("momsurffix_sensitivity_low", "0.95", 0, "Sensitivity for low speed (< threshold_low)");
ConVar g_cvSensitivityMid("momsurffix_sensitivity_mid", "0.97", 0, "Sensitivity for mid speed (threshold_low ~ threshold_high)");
ConVar g_cvSensitivityHigh("momsurffix_sensitivity_high", "0.99", 0, "Sensitivity for high speed (> threshold_high)");

ConVar g_cvMinSpeed("momsurffix_min_speed", "250.0", 0,
    "Minimum speed (u/s) required to trigger fix. Default 250 = max walk speed.");

ConVar g_cvEnableSmoothTransition("momsurffix_enable_smooth_transition", "1", 0,
    "Enable smooth sensitivity transition between speed tiers (linear interpolation)");


// Offsets
int g_off_Player = -1;
int g_off_MV = -1;
int g_off_VecVelocity = -1; 
int g_off_VecAbsOrigin = -1;
int g_off_GroundEntity = -1;
int g_off_VecMins = -1;
int g_off_VecMaxs = -1;

// Compatibility offsets
int g_off_WaterLevel = -1;
int g_off_MoveType = -1;

CSimpleDetour *g_pDetour = nullptr;

// ============================================================================
// Statistics
// ============================================================================
struct FixStatistics {
    int totalFixes;
    int fixesBySpeed[3];  // [low < 500, mid 500-1000, high > 1000]
    float totalSpeedLoss;
    float totalSpeedGain;
    int sampleCount;

    float GetAvgSpeedLoss() const {
        return sampleCount > 0 ? totalSpeedLoss / sampleCount : 0.0f;
    }

    float GetAvgSpeedGain() const {
        return sampleCount > 0 ? totalSpeedGain / sampleCount : 0.0f;
    }

    void Reset() {
        totalFixes = 0;
        fixesBySpeed[0] = fixesBySpeed[1] = fixesBySpeed[2] = 0;
        totalSpeedLoss = totalSpeedGain = 0.0f;
        sampleCount = 0;
    }
};

FixStatistics g_GlobalStats = {0};

// ============================================================================
// Forward + Native interface
// ============================================================================
struct LastClipData {
    float inVel[3];
    float planeNormal[3];
    float outVel[3];
    float timestamp;
};

LastClipData g_LastClipData[MAXPLAYERS + 1];
IForward *g_pOnClipVelocity = nullptr;

// ----------------------------------------------------------------------------
// Dynamic enable/disable
// ----------------------------------------------------------------------------
void UpdateDetourState()
{
    if (!g_pDetour) return;

    if (g_cvEnable.GetBool())
    {
        g_pDetour->Enable();
        Msg("[MomSurfFix] Status: ENABLED (Hook Active)\n");
    }
    else
    {
        g_pDetour->Disable();
        Msg("[MomSurfFix] Status: DISABLED (Vanilla Physics)\n");
    }
}

void OnEnableChanged(IConVar *var, const char *pOldValue, float flOldValue)
{
    UpdateDetourState();
}

// ----------------------------------------------------------------------------
// Helper classes and functions
// ----------------------------------------------------------------------------
class CTraceFilterSimple : public ITraceFilter
{
public:
    CTraceFilterSimple(const IHandleEntity *passentity, int collisionGroup)
        : m_pPassEnt(passentity), m_collisionGroup(collisionGroup) {}

    virtual bool ShouldHitEntity(IHandleEntity *pHandleEntity, int contentsMask)
    {
        return pHandleEntity != m_pPassEnt;
    }

    virtual TraceType_t GetTraceType() const
    {
        return TRACE_EVERYTHING;
    }

private:
    const IHandleEntity *m_pPassEnt;
    int m_collisionGroup;
};

void TracePlayerBBox(const Vector &start, const Vector &end, void *pPlayer, IHandleEntity *pPlayerEntity, int collisionGroup, CGameTrace &pm)
{
    if (!enginetrace) return;

    Ray_t ray;
    Vector mins, maxs;

    if (g_off_VecMins != -1 && g_off_VecMaxs != -1)
    {
        mins = *(Vector *)((uintptr_t)pPlayer + g_off_VecMins);
        maxs = *(Vector *)((uintptr_t)pPlayer + g_off_VecMaxs);
    }
    else
    {
        mins = Vector(-16, -16, 0);
        maxs = Vector(16, 16, 72);
    }

    ray.Init(start, end, mins, maxs);

    CTraceFilterSimple traceFilter(pPlayerEntity, collisionGroup);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &traceFilter, &pm);
}

// ----------------------------------------------------------------------------
// Adaptive sensitivity system
// ----------------------------------------------------------------------------
float GetAdaptiveSensitivity(float speed)
{
    int mode = g_cvAdaptiveMode.GetInt();
    if (mode == 0) {
        return g_cvSensitivity.GetFloat();
    }

    // Speed-adaptive mode (mode == 1)
    float lowThreshold = g_cvSpeedThresholdLow.GetFloat();
    float highThreshold = g_cvSpeedThresholdHigh.GetFloat();
    float sensLow = g_cvSensitivityLow.GetFloat();
    float sensMid = g_cvSensitivityMid.GetFloat();
    float sensHigh = g_cvSensitivityHigh.GetFloat();

    if (speed < lowThreshold) {
        return sensLow;
    } else if (speed > highThreshold) {
        return sensHigh;
    }

    // Mid range: smooth or stepped
    if (!g_cvEnableSmoothTransition.GetBool()) {
        // Stepped (legacy compat)
        return sensMid;
    }

    // Linear interpolation: two segments (low→mid, mid→high)
    float t = (speed - lowThreshold) / (highThreshold - lowThreshold);

    if (t < 0.5f) {
        // First half: low to mid
        return sensLow + (sensMid - sensLow) * (t * 2.0f);
    } else {
        // Second half: mid to high
        return sensMid + (sensHigh - sensMid) * ((t - 0.5f) * 2.0f);
    }
}

// ----------------------------------------------------------------------------
// Record fix statistics
// ----------------------------------------------------------------------------
void RecordFix(float preSpeed, float postSpeed, float fixedSpeed)
{
    g_GlobalStats.totalFixes++;

    float speedLoss = preSpeed - postSpeed;
    float speedGain = fixedSpeed - postSpeed;

    g_GlobalStats.totalSpeedLoss += speedLoss;
    g_GlobalStats.totalSpeedGain += speedGain;
    g_GlobalStats.sampleCount++;

    if (preSpeed < 500.0f) {
        g_GlobalStats.fixesBySpeed[0]++;
    } else if (preSpeed < 1000.0f) {
        g_GlobalStats.fixesBySpeed[1]++;
    } else {
        g_GlobalStats.fixesBySpeed[2]++;
    }
}

// ----------------------------------------------------------------------------
// Statistics commands
// ----------------------------------------------------------------------------
CON_COMMAND(momsurffix_stats, "Show MomSurfFix statistics")
{
    Msg("========================================\n");
    Msg("  MomSurfFix Statistics\n");
    Msg("========================================\n");
    Msg("Total Fixes: %d\n", g_GlobalStats.totalFixes);
    Msg("  Low Speed  (< 500):  %d\n", g_GlobalStats.fixesBySpeed[0]);
    Msg("  Mid Speed  (500-1k): %d\n", g_GlobalStats.fixesBySpeed[1]);
    Msg("  High Speed (> 1k):   %d\n", g_GlobalStats.fixesBySpeed[2]);
    Msg("----------------------------------------\n");
    Msg("Avg Speed Loss: %.1f u/s\n", g_GlobalStats.GetAvgSpeedLoss());
    Msg("Avg Speed Gain: %.1f u/s\n", g_GlobalStats.GetAvgSpeedGain());
    if (g_GlobalStats.GetAvgSpeedLoss() > 0.0f) {
        Msg("Recovery Rate:  %.1f%%\n",
            g_GlobalStats.GetAvgSpeedGain() / g_GlobalStats.GetAvgSpeedLoss() * 100.0f);
    }
    Msg("========================================\n");
}

CON_COMMAND(momsurffix_reset_stats, "Reset statistics")
{
    g_GlobalStats.Reset();
    Msg("[MomSurfFix] Statistics reset\n");
}

// ----------------------------------------------------------------------------
// Detour logic
// ----------------------------------------------------------------------------
#ifndef THISCALL
    #define THISCALL
#endif
typedef int (THISCALL *TryPlayerMove_t)(void *, Vector *, CGameTrace *, float);

#define MOVETYPE_NOCLIP 8
#define MOVETYPE_LADDER 9

int Detour_TryPlayerMove(void *pThis, Vector *pFirstDest, CGameTrace *pFirstTrace, float flTimeLeft)
{
    TryPlayerMove_t Original = (TryPlayerMove_t)g_pDetour->GetTrampoline();
    
    if (!Original || !g_cvEnable.GetBool()) 
    {
        return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);
    }

    void *pPlayer = *(void **)((uintptr_t)pThis + g_off_Player);
    CMoveData *mv = *(CMoveData **)((uintptr_t)pThis + g_off_MV);
    if (!pPlayer || !mv) return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    // ========================================================================
    // Compatibility guard (ladder & water)
    // ========================================================================
    if (g_off_MoveType != -1)
    {
        unsigned char moveTypeByte = *(unsigned char *)((uintptr_t)pPlayer + g_off_MoveType);
        if (moveTypeByte == MOVETYPE_LADDER || moveTypeByte == MOVETYPE_NOCLIP)
            return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);
    }

    if (g_off_WaterLevel != -1)
    {
        unsigned char waterLevel = *(unsigned char *)((uintptr_t)pPlayer + g_off_WaterLevel);
        if (waterLevel >= 2)
            return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);
    }

    Vector *pVel = (Vector *)((uintptr_t)mv + g_off_VecVelocity);
    Vector *pOrigin = (Vector *)((uintptr_t)mv + g_off_VecAbsOrigin);

    // 1. Record pre-move state
    Vector preVelocity = *pVel;
    Vector preOrigin = *pOrigin;
    float preSpeedSq = preVelocity.LengthSqr();

    // 2. Run original engine
    int result = Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    // ========================================================================
    // Ramp fix logic
    // ========================================================================

    if (preSpeedSq < g_cvMinSpeed.GetFloat() * g_cvMinSpeed.GetFloat()) return result;

    unsigned long *pGroundEntity = (unsigned long *)((uintptr_t)pPlayer + g_off_GroundEntity);

    float postSpeedSq = pVel->LengthSqr();

    float speed = sqrt(preSpeedSq);
    float adaptiveSensitivity = GetAdaptiveSensitivity(speed);
    if (postSpeedSq > preSpeedSq * adaptiveSensitivity) return result;

    // 4. Perform fix detection
    IHandleEntity *pEntity = (IHandleEntity *)pPlayer;
    CGameTrace trace;
    
    Vector endPos = preOrigin + (preVelocity * flTimeLeft);
    TracePlayerBBox(preOrigin, endPos, pPlayer, pEntity, COLLISION_GROUP_PLAYER_MOVEMENT, trace);

    float rampNormalZ = g_cvRampNormalZ.GetFloat();

    // Only fix steep ramps (0 <= z < rampNormalZ), exclude ceilings (z < 0)
    if (trace.DidHit() && trace.plane.normal.z >= 0.0f && trace.plane.normal.z < rampNormalZ)
    {
        float backoff = DotProduct(preVelocity, trace.plane.normal);
        
        if (backoff < 0.0f)
        {
            Vector fixVel = preVelocity - (trace.plane.normal * backoff);

            *pVel = fixVel;
            *pGroundEntity = 0xFFFFFFFF;

            float fixedSpeed = fixVel.Length();

            // Record stats
            RecordFix(speed, sqrt(postSpeedSq), fixedSpeed);

            // Fire forward
            if (g_pOnClipVelocity && g_pOnClipVelocity->GetFunctionCount() > 0)
            {
                // Get client index from entity reference
            int clientIndex = gamehelpers->ReferenceToIndex(
                gamehelpers->EntityToReference((CBaseEntity*)pPlayer));

                if (clientIndex > 0 && clientIndex <= MAXPLAYERS)
                {
                    // Save last fix data
                    g_LastClipData[clientIndex].inVel[0] = preVelocity.x;
                    g_LastClipData[clientIndex].inVel[1] = preVelocity.y;
                    g_LastClipData[clientIndex].inVel[2] = preVelocity.z;
                    g_LastClipData[clientIndex].planeNormal[0] = trace.plane.normal.x;
                    g_LastClipData[clientIndex].planeNormal[1] = trace.plane.normal.y;
                    g_LastClipData[clientIndex].planeNormal[2] = trace.plane.normal.z;
                    g_LastClipData[clientIndex].outVel[0] = fixVel.x;
                    g_LastClipData[clientIndex].outVel[1] = fixVel.y;
                    g_LastClipData[clientIndex].outVel[2] = fixVel.z;
                    g_LastClipData[clientIndex].timestamp = 0.0f;

                    g_pOnClipVelocity->PushCell(clientIndex);
                    g_pOnClipVelocity->PushArray((cell_t *)g_LastClipData[clientIndex].inVel, 3);
                    g_pOnClipVelocity->PushArray((cell_t *)g_LastClipData[clientIndex].planeNormal, 3);
                    g_pOnClipVelocity->PushArray((cell_t *)g_LastClipData[clientIndex].outVel, 3);
                    g_pOnClipVelocity->Execute(nullptr);
                }
            }

            if (g_cvDebug.GetBool())
                Msg("[MomSurfFix] FIXED! Speed: %.0f -> %.0f (Sensitivity: %.3f)\n", speed, fixedSpeed, adaptiveSensitivity);
        }
    }

    return result;
}

// ----------------------------------------------------------------------------
// SDK lifecycle
// ----------------------------------------------------------------------------
extern sp_nativeinfo_t g_Natives[];

bool MomSurfFixExt::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
    char conf_error[255];
    IGameConfig *conf = nullptr;
    if (!gameconfs->LoadGameConfigFile("momsurffix_fix.games", &conf, conf_error, sizeof(conf_error)))
    {
        snprintf(error, maxlength, "Could not read momsurffix_fix.games: %s", conf_error);
        return false;
    }

    if (!conf->GetOffset("CGameMovement::player", &g_off_Player) ||
        !conf->GetOffset("CGameMovement::mv", &g_off_MV) ||
        !conf->GetOffset("CMoveData::m_vecVelocity", &g_off_VecVelocity) ||
        !conf->GetOffset("CMoveData::m_vecAbsOrigin", &g_off_VecAbsOrigin))
    {
        snprintf(error, maxlength, "Failed to get core offsets.");
        gameconfs->CloseGameConfigFile(conf);
        return false;
    }

    sm_sendprop_info_t info;
    if (gamehelpers->FindSendPropInfo("CBasePlayer", "m_hGroundEntity", &info))
    {
        g_off_GroundEntity = info.actual_offset;
    }
    else
    {
        if (!conf->GetOffset("CBasePlayer::m_hGroundEntity", &g_off_GroundEntity))
        {
             snprintf(error, maxlength, "Missing 'CBasePlayer::m_hGroundEntity'.");
             gameconfs->CloseGameConfigFile(conf);
             return false;
        }
    }

    // Hitbox offsets
    if (gamehelpers->FindSendPropInfo("CBasePlayer", "m_vecMins", &info))
        g_off_VecMins = info.actual_offset;
    if (gamehelpers->FindSendPropInfo("CBasePlayer", "m_vecMaxs", &info))
        g_off_VecMaxs = info.actual_offset;

    // Compatibility offsets
    if (gamehelpers->FindSendPropInfo("CBaseEntity", "m_nWaterLevel", &info))
        g_off_WaterLevel = info.actual_offset;
    if (gamehelpers->FindSendPropInfo("CBaseEntity", "m_MoveType", &info))
        g_off_MoveType = info.actual_offset;
    else if (gamehelpers->FindSendPropInfo("CBasePlayer", "m_MoveType", &info))
        g_off_MoveType = info.actual_offset;

    void *pTryPlayerMoveAddr = nullptr;
    if (!conf->GetMemSig("CGameMovement::TryPlayerMove", &pTryPlayerMoveAddr) || !pTryPlayerMoveAddr)
    {
        snprintf(error, maxlength, "Failed to find TryPlayerMove signature.");
        gameconfs->CloseGameConfigFile(conf);
        return false;
    }

    g_pDetour = new CSimpleDetour(pTryPlayerMoveAddr, (void *)Detour_TryPlayerMove);
    UpdateDetourState();

    void *pCreateInterface = nullptr;
    if (conf->GetMemSig("CreateInterface", &pCreateInterface) && pCreateInterface)
    {
        CreateInterfaceFn factory = (CreateInterfaceFn)pCreateInterface;
        enginetrace = (IEngineTrace *)factory(INTERFACEVERSION_ENGINETRACE_SERVER, nullptr);
    }

    if (!enginetrace)
    {
        snprintf(error, maxlength, "Could not find interface: %s", INTERFACEVERSION_ENGINETRACE_SERVER);
        gameconfs->CloseGameConfigFile(conf);
        return false;
    }

    void *hVStdLib = dlopen("libvstdlib_srv.so", RTLD_NOW | RTLD_NOLOAD);
    if (!hVStdLib) hVStdLib = dlopen("libvstdlib.so", RTLD_NOW | RTLD_NOLOAD);
    
    ICvar *pCvar = nullptr;
    if (hVStdLib) {
        CreateInterfaceFn factory = (CreateInterfaceFn)dlsym(hVStdLib, "CreateInterface");
        if (factory) {
            pCvar = (ICvar *)factory(CVAR_INTERFACE_VERSION, nullptr);
        }
        dlclose(hVStdLib);
    }
    
    if (!pCvar && pCreateInterface) {
         CreateInterfaceFn factory = (CreateInterfaceFn)pCreateInterface;
         pCvar = (ICvar *)factory(CVAR_INTERFACE_VERSION, nullptr);
    }

    if (pCvar) {
        g_pCVar = pCvar;
        ConVar_Register(0);
    }

    // Register forward
    g_pOnClipVelocity = forwards->CreateForward("MomSurfFix_OnClipVelocity", ET_Ignore, 4, nullptr,
        Param_Cell,   // client index
        Param_Array,  // inVel[3]
        Param_Array,  // planeNormal[3]
        Param_Array   // outVel[3]
    );

    // Register natives
    sharesys->AddNatives(myself, g_Natives);
    sharesys->RegisterLibrary(myself, "momsurffix_ext");

    gameconfs->CloseGameConfigFile(conf);
    return true;
}

void MomSurfFixExt::SDK_OnUnload()
{
    if (g_pDetour)
    {
        g_pDetour->Disable();
        delete g_pDetour;
        g_pDetour = nullptr;
    }

    if (g_pOnClipVelocity)
    {
        forwards->ReleaseForward(g_pOnClipVelocity);
        g_pOnClipVelocity = nullptr;
    }
}

void MomSurfFixExt::SDK_OnAllLoaded()
{
}

void MomSurfFixExt::LevelInit(char const *pMapName)
{
    g_GlobalStats.Reset();
}

bool MomSurfFixExt::QueryRunning(char *error, size_t maxlength)
{
    return true;
}

// ============================================================================
// Native implementations
// ============================================================================
static cell_t Native_GetLastClipData(IPluginContext *pContext, const cell_t *params)
{
    int client = params[1];
    if (client < 1 || client > MAXPLAYERS)
        return pContext->ThrowNativeError("Invalid client %d", client);

    cell_t *inVel, *planeNormal, *outVel;
    pContext->LocalToPhysAddr(params[2], &inVel);
    pContext->LocalToPhysAddr(params[3], &planeNormal);
    pContext->LocalToPhysAddr(params[4], &outVel);

    for (int i = 0; i < 3; i++) {
        inVel[i] = sp_ftoc(g_LastClipData[client].inVel[i]);
        planeNormal[i] = sp_ftoc(g_LastClipData[client].planeNormal[i]);
        outVel[i] = sp_ftoc(g_LastClipData[client].outVel[i]);
    }

    return 1;
}

static cell_t Native_GetFixStats(IPluginContext *pContext, const cell_t *params)
{
    cell_t *totalFixes, *avgLoss, *avgGain;
    pContext->LocalToPhysAddr(params[1], &totalFixes);
    pContext->LocalToPhysAddr(params[2], &avgLoss);
    pContext->LocalToPhysAddr(params[3], &avgGain);

    *totalFixes = g_GlobalStats.totalFixes;
    *avgLoss = sp_ftoc(g_GlobalStats.GetAvgSpeedLoss());
    *avgGain = sp_ftoc(g_GlobalStats.GetAvgSpeedGain());

    return 1;
}

sp_nativeinfo_t g_Natives[] = {
    {"MomSurfFix_GetLastClipData", Native_GetLastClipData},
    {"MomSurfFix_GetFixStats",     Native_GetFixStats},
    {NULL, NULL}
};
