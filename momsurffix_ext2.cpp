#include "extension.h"
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <dlfcn.h>

#include <tier0/platform.h>
#include <tier1/convar.h>
#include <gametrace.h>
#include <soundflags.h>
#include <ihandleentity.h>
#if __has_include(<interfaces/interfaces.h>)
#include <interfaces/interfaces.h>
#else
#include <tier1/interface.h>
#endif
#include <engine/IEngineTrace.h>
#include <ispatialpartition.h>
#include <eiface.h>
class CBasePlayer;
class CBaseEntity;
enum PLAYER_ANIM { PLAYER_IDLE=0, PLAYER_WALK, PLAYER_JUMP, PLAYER_SUPERJUMP, PLAYER_DIE, PLAYER_ATTACK1 };
#include <igamemovement.h>
#include "simple_detour.h"

// ============================================================================
// Globals
// ============================================================================
#ifndef MAXPLAYERS
#define MAXPLAYERS 65
#endif
MomSurfFixExt2 g_MomSurfFixExt2;
SDKExtension *g_pExtensionIface = &g_MomSurfFixExt2;

IEngineTrace *enginetrace = nullptr;
CGlobalVars *gpGlobals = nullptr;
extern IGameHelpers *gamehelpers;  // Defined in smsdk_ext.cpp

// ============================================================================
// Offsets
// ============================================================================
int g_off_Player        = -1;
int g_off_MV            = -1;
int g_off_VecVelocity   = -1;
int g_off_VecAbsOrigin  = -1;
int g_off_GroundEntity  = -1;
int g_off_SurfaceFriction = -1;
int g_off_MoveType      = -1;
int g_off_WaterLevel    = -1;

// vtable offsets
int g_vtoff_ClipVelocity        = -1;
int g_vtoff_TracePlayerBBox     = -1;
int g_vtoff_AddToTouched        = -1;
int g_vtoff_TraceRay            = -1;
int g_vtoff_CanTraceRay         = -1;
int g_vtoff_TraceRayAgainstLeaf = -1;
int g_vtoff_LockTraceFilter     = -1;
int g_vtoff_UnlockTraceFilter   = -1;

// ============================================================================
// ConVars
// ============================================================================
void OnEnableChanged(IConVar *var, const char *pOldValue, float flOldValue);

ConVar g_cvEnable("momsurffix2_enable", "1", 0, "Enable Surf Bug Fix v2", OnEnableChanged);
ConVar g_cvRampBumpCount("momsurffix2_ramp_bumpcount", "8", 0, "Max bump iterations (4-16)");
ConVar g_cvNoclipWorkaround("momsurffix2_noclip_workaround", "1", 0, "Enable noclip workaround");

// ============================================================================
// Detour
// ============================================================================
CSimpleDetour *g_pDetour = nullptr;

// ============================================================================
// Forward + Stats
// ============================================================================
IForward *g_pOnClipVelocity = nullptr;

struct LastClipData { float inVel[3]; float planeNormal[3]; float outVel[3]; };
LastClipData g_LastClipData[MAXPLAYERS + 1];

struct FixStats {
    int   totalFixes;
    float totalLoss;
    float totalGain;
    int   samples;
    float AvgLoss() const { return samples > 0 ? totalLoss / samples : 0.f; }
    float AvgGain() const { return samples > 0 ? totalGain / samples : 0.f; }
    void  Reset()   { totalFixes = samples = 0; totalLoss = totalGain = 0.f; }
};
FixStats g_Stats = {0};

// ============================================================================
// Engine version
// ============================================================================
static bool g_bIsCSGO = false;

#define MOVETYPE_NOCLIP  8
#define MOVETYPE_LADDER  9
#define MAX_CLIP_PLANES  5
#define FLT_EPSILON_VAL  1.192092896e-07f
#ifndef MASK_PLAYERSOLID
#define MASK_PLAYERSOLID 0x0201400B
#endif
#ifndef COLLISION_GROUP_PLAYER_MOVEMENT
#define COLLISION_GROUP_PLAYER_MOVEMENT 8
#endif
#ifndef MAXPLAYERS
#define MAXPLAYERS 65
#endif

// ============================================================================
// Vtable call helpers
// ============================================================================
template<typename T>
static inline T VTableCall(void *obj, int offset)
{
    void **vtable = *(void ***)obj;
    return (T)vtable[offset];
}

// ClipVelocity(in, normal, out, overbounce)
typedef void (*ClipVelocity_t)(void*, Vector&, Vector&, Vector&, float);
static void DoClipVelocity(void *pGM, Vector &in, Vector &normal, Vector &out, float overbounce)
{
    VTableCall<ClipVelocity_t>(pGM, g_vtoff_ClipVelocity)(pGM, in, normal, out, overbounce);
}

// TracePlayerBBox(start, end, mask, collisionGroup, trace)
typedef void (*TracePlayerBBox_t)(void*, const Vector&, const Vector&, unsigned int, int, CGameTrace&);
static void DoTracePlayerBBox(void *pGM, const Vector &start, const Vector &end,
                               unsigned int mask, int collisionGroup, CGameTrace &trace)
{
    VTableCall<TracePlayerBBox_t>(pGM, g_vtoff_TracePlayerBBox)(pGM, start, end, mask, collisionGroup, trace);
}

// AddToTouched(trace, vel) — called on IMoveHelper singleton
typedef void (*AddToTouched_t)(void*, const CGameTrace&, const Vector&);
static void *g_pMoveHelper = nullptr;
static void DoAddToTouched(const CGameTrace &trace, const Vector &vel)
{
    if (!g_pMoveHelper) return;
    VTableCall<AddToTouched_t>(g_pMoveHelper, g_vtoff_AddToTouched)(g_pMoveHelper, trace, vel);
}

// ============================================================================
// Inline math helpers
// ============================================================================
static inline bool CE_Vec(const Vector &a, const Vector &b, float eps = FLT_EPSILON_VAL)
{
    return fabsf(a.x-b.x) <= eps && fabsf(a.y-b.y) <= eps && fabsf(a.z-b.z) <= eps;
}
static inline bool CloseEnoughF(float a, float b, float eps = FLT_EPSILON_VAL)
{
    return fabsf(a-b) <= eps;
}
// VectorMA: use SDK's inline version

ConVar g_cvRetrace("momsurffix2_retrace", "0", 0, "Enable 27-trace retrace for stuck-on-ramp (may cause stutter, default off)");
ConVar g_cvRetraceLen("momsurffix2_retrace_length", "0.2", 0, "Retrace offset length");

ConVar *g_pSvBounce = nullptr;

// ============================================================================
// Accessor helpers
// ============================================================================
static inline void*      GetPlayer(void *pGM)    { return *(void**)((uintptr_t)pGM + g_off_Player); }
static inline CMoveData* GetMV(void *pGM)         { return *(CMoveData**)((uintptr_t)pGM + g_off_MV); }
static inline Vector&    GetVelocity(CMoveData *mv){ return *(Vector*)((uintptr_t)mv + g_off_VecVelocity); }
static inline Vector&    GetOrigin(CMoveData *mv)  { return *(Vector*)((uintptr_t)mv + g_off_VecAbsOrigin); }
static inline uint32_t&  GetGroundEnt(void *pl)   { return *(uint32_t*)((uintptr_t)pl + g_off_GroundEntity); }
static inline float      GetSurfFriction(void *pl){ return *(float*)((uintptr_t)pl + g_off_SurfaceFriction); }
static inline uint8_t    GetMoveType(void *pl)    { return (g_off_MoveType!=-1) ? *(uint8_t*)((uintptr_t)pl+g_off_MoveType) : 0; }
static inline uint8_t    GetWaterLevel(void *pl)  { return (g_off_WaterLevel!=-1) ? *(uint8_t*)((uintptr_t)pl+g_off_WaterLevel) : 0; }

// ============================================================================
// TryPlayerMove detour — full replacement
// ============================================================================
typedef int (*TryPlayerMove_t)(void*, Vector*, CGameTrace*, float);

static int Detour_TryPlayerMove(void *pThis, Vector *pFirstDest, CGameTrace *pFirstTrace, float flTimeLeft)
{
    TryPlayerMove_t Original = (TryPlayerMove_t)g_pDetour->GetTrampoline();

    if (!g_cvEnable.GetBool())
        return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    void      *pPlayer = GetPlayer(pThis);
    CMoveData *mv      = GetMV(pThis);
    if (!pPlayer || !mv)
        return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    uint8_t mt = GetMoveType(pPlayer);
    if (mt == MOVETYPE_LADDER || mt == MOVETYPE_NOCLIP)
        return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);
    if (GetWaterLevel(pPlayer) >= 2)
        return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    Vector &vecVelocity  = GetVelocity(mv);
    Vector &vecAbsOrigin = GetOrigin(mv);

    Vector original_velocity = vecVelocity;
    Vector primal_velocity   = vecVelocity;
    Vector fixed_origin      = vecAbsOrigin;
    Vector new_velocity(0,0,0), end(0,0,0), dir(0,0,0), valid_plane(0,0,0);

    float planes[MAX_CLIP_PLANES][3] = {};
    float allFraction = 0.f, time_left = gpGlobals->interval_per_tick, d;
    int   bumpcount, blocked = 0, numplanes = 0;
    int   numbumps = g_cvRampBumpCount.GetInt();
    bool  stuck_on_ramp = false, has_valid_plane = false;

    float bounceVal = g_pSvBounce ? g_pSvBounce->GetFloat() : 0.f;
    bool  doRetrace = g_cvRetrace.GetBool();

    CGameTrace pm;
    memset(&pm, 0, sizeof(pm));
    pm.fraction = 1.f;

    for (bumpcount = 0; bumpcount < numbumps; bumpcount++)
    {
        if (vecVelocity.LengthSqr() == 0.f) break;

        if (stuck_on_ramp)
        {
            if (!has_valid_plane)
            {
                Vector pn = pm.plane.normal;
                if (!CE_Vec(pn, vec3_origin) &&
                    fabsf(pn.x)<=1.f && fabsf(pn.y)<=1.f && fabsf(pn.z)<=1.f &&
                    !(pn.x==valid_plane.x && pn.y==valid_plane.y && pn.z==valid_plane.z))
                {
                    valid_plane = pn;
                    has_valid_plane = true;
                }
                else
                {
                    for (int i = numplanes-1; i >= 0; i--)
                    {
                        Vector p(planes[i][0], planes[i][1], planes[i][2]);
                        if (!CE_Vec(p, vec3_origin) &&
                            fabsf(p.x)<=1.f && fabsf(p.y)<=1.f && fabsf(p.z)<=1.f &&
                            !(p.x==valid_plane.x && p.y==valid_plane.y && p.z==valid_plane.z))
                        {
                            valid_plane = p;
                            has_valid_plane = true;
                            break;
                        }
                    }
                }
            }

            if (has_valid_plane)
            {
                float ob = (valid_plane.z >= 0.7f) ? 1.f
                         : 1.f + bounceVal * (1.f - GetSurfFriction(pPlayer));
                DoClipVelocity(pThis, vecVelocity, valid_plane, vecVelocity, ob);
                original_velocity = vecVelocity;
                VectorMA(fixed_origin, g_cvRetraceLen.GetFloat(), valid_plane, fixed_origin);
            }
            else if (doRetrace &&
                     (!g_cvNoclipWorkaround.GetBool() || vecVelocity.z < -6.25f || vecVelocity.z > 0.f))
            {
                // 5×5×5 = 125-trace retrace
                float offsets[5] = {
                    (float(bumpcount)*2.f) * -g_cvRetraceLen.GetFloat() * 2.f,
                    (float(bumpcount)*2.f) * -g_cvRetraceLen.GetFloat(),
                    0.f,
                    (float(bumpcount)*2.f) *  g_cvRetraceLen.GetFloat(),
                    (float(bumpcount)*2.f) *  g_cvRetraceLen.GetFloat() * 2.f,
                };
                Vector accum(0,0,0);
                int valid_planes = 0;
                valid_plane = vec3_origin;

                for (int i=0;i<5;i++) for (int j=0;j<5;j++) for (int h=0;h<5;h++)
                {
                    Vector off(offsets[i], offsets[j], offsets[h]);
                    Vector s(fixed_origin.x+off.x, fixed_origin.y+off.y, fixed_origin.z+off.z);
                    Vector e(end.x-off.x, end.y-off.y, end.z-off.z);
                    CGameTrace rt; memset(&rt,0,sizeof(rt)); rt.fraction=1.f;
                    DoTracePlayerBBox(pThis, s, e, MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, rt);
                    Vector pn = rt.plane.normal;
                    if (fabsf(pn.x)<=1.f && fabsf(pn.y)<=1.f && fabsf(pn.z)<=1.f &&
                        rt.fraction>0.f && rt.fraction<1.f && !rt.startsolid)
                    { valid_planes++; accum.x+=pn.x; accum.y+=pn.y; accum.z+=pn.z; }
                }

                if (valid_planes > 0 && !CE_Vec(accum, vec3_origin))
                {
                    has_valid_plane = true;
                    VectorNormalize(accum);
                    valid_plane = accum;
                    continue;
                }
            }

            if (!has_valid_plane) { stuck_on_ramp = false; continue; }
        }

        VectorMA(fixed_origin, time_left, vecVelocity, end);

        if (pFirstDest && pFirstTrace &&
            CloseEnoughF(end.x,pFirstDest->x) && CloseEnoughF(end.y,pFirstDest->y) && CloseEnoughF(end.z,pFirstDest->z))
        {
            pm = *pFirstTrace;
        }
        else
        {
            Vector &traceStart = (stuck_on_ramp && has_valid_plane) ? fixed_origin : vecAbsOrigin;
            DoTracePlayerBBox(pThis, traceStart, end, MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, pm);
        }

        // Stuck detection
        if (bumpcount > 0 && GetGroundEnt(pPlayer) == 0xFFFFFFFF)
        {
            bool valid = !pm.allsolid && !pm.startsolid && !CloseEnoughF(pm.fraction, 0.f);
            if (valid)
            {
                Vector pn = pm.plane.normal;
                // exclude ceiling (normal.z <= -0.7) and floor (normal.z >= 0.7 handled by ground check)
                valid = fabsf(pn.x)<=1.f && fabsf(pn.y)<=1.f && fabsf(pn.z)<=1.f
                        && pn.z > -0.7f;
            }
            if (valid)
            {
                CGameTrace stuck; memset(&stuck,0,sizeof(stuck)); stuck.fraction=1.f;
                DoTracePlayerBBox(pThis, pm.endpos, pm.endpos, MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, stuck);
                valid = !stuck.startsolid && CloseEnoughF(stuck.fraction, 1.f);
            }
            if (!valid) { has_valid_plane=false; stuck_on_ramp=true; continue; }
        }

        if (pm.fraction > 0.f)
        {
            if ((bumpcount==0 || GetGroundEnt(pPlayer)!=0xFFFFFFFF) && numbumps>0 && CloseEnoughF(pm.fraction,1.f))
            {
                CGameTrace stuck; memset(&stuck,0,sizeof(stuck)); stuck.fraction=1.f;
                DoTracePlayerBBox(pThis, pm.endpos, pm.endpos, MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, stuck);
                if ((stuck.startsolid || !CloseEnoughF(stuck.fraction,1.f)) && bumpcount==0)
                    { has_valid_plane=false; stuck_on_ramp=true; continue; }
                else if (stuck.startsolid || !CloseEnoughF(stuck.fraction,1.f))
                    { vecVelocity=vec3_origin; break; }
            }
            has_valid_plane=false; stuck_on_ramp=false;
            original_velocity = vecVelocity;
            vecAbsOrigin = pm.endpos;
            fixed_origin = pm.endpos;
            allFraction += pm.fraction;
            numplanes = 0;
        }

        if (CloseEnoughF(pm.fraction, 1.f)) break;

        DoAddToTouched(pm, vecVelocity);

        if (pm.plane.normal.z >= 0.7f) blocked |= 1;
        if (CloseEnoughF(pm.plane.normal.z, 0.f)) blocked |= 2;

        time_left -= time_left * pm.fraction;
        if (numplanes >= MAX_CLIP_PLANES) { vecVelocity=vec3_origin; break; }

        planes[numplanes][0]=pm.plane.normal.x;
        planes[numplanes][1]=pm.plane.normal.y;
        planes[numplanes][2]=pm.plane.normal.z;
        numplanes++;

        if (numplanes==1 && GetGroundEnt(pPlayer)!=0xFFFFFFFF)
        {
            float ob = (planes[0][2]>=0.7f) ? 1.f : 1.f+bounceVal*(1.f-GetSurfFriction(pPlayer));
            Vector pn(planes[0][0],planes[0][1],planes[0][2]);
            DoClipVelocity(pThis, original_velocity, pn, new_velocity, ob);
            vecVelocity=new_velocity; original_velocity=new_velocity;
        }
        else
        {
            int i,j;
            for (i=0;i<numplanes;i++)
            {
                Vector pn(planes[i][0],planes[i][1],planes[i][2]);
                DoClipVelocity(pThis, original_velocity, pn, vecVelocity, 1.f);
                for (j=0;j<numplanes;j++)
                    if (j!=i) { Vector pj(planes[j][0],planes[j][1],planes[j][2]); if(vecVelocity.Dot(pj)<0.f) break; }
                if (j==numplanes) break;
            }
            if (i==numplanes)
            {
                if (numplanes!=2) { vecVelocity=vec3_origin; break; }
                Vector p0(planes[0][0],planes[0][1],planes[0][2]);
                Vector p1(planes[1][0],planes[1][1],planes[1][2]);
                if (CE_Vec(p0,p1)) { VectorMA(original_velocity,20.f,p0,new_velocity); vecVelocity.x=new_velocity.x; vecVelocity.y=new_velocity.y; break; }
                CrossProduct(p0,p1,dir); VectorNormalize(dir);
                d=vecVelocity.Dot(dir); vecVelocity=dir*d;
            }
            d=vecVelocity.Dot(primal_velocity);
            if (d<=0.f) { vecVelocity=vec3_origin; break; }
        }
    }

    if (CloseEnoughF(allFraction, 0.f)) vecVelocity=vec3_origin;
    return blocked;
}

// ============================================================================
// Enable / Disable
// ============================================================================
void UpdateDetourState()
{
    if (!g_pDetour) return;
    if (g_cvEnable.GetBool())
        g_pDetour->Enable();
    else
        g_pDetour->Disable();
}

void OnEnableChanged(IConVar *var, const char *pOldValue, float flOldValue)
{
    UpdateDetourState();
}

// ============================================================================
// SDK lifecycle
// ============================================================================
extern sp_nativeinfo_t g_Natives[];

bool MomSurfFixExt2::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
    char conf_error[255];
    IGameConfig *conf = nullptr;
    if (!gameconfs->LoadGameConfigFile("momsurffix_fix2.games", &conf, conf_error, sizeof(conf_error)))
    {
        snprintf(error, maxlength, "Could not read momsurffix_fix2.games: %s", conf_error);
        return false;
    }

    // Core struct offsets
    if (!conf->GetOffset("CGameMovement::player", &g_off_Player) ||
        !conf->GetOffset("CGameMovement::mv", &g_off_MV) ||
        !conf->GetOffset("CMoveData::m_vecVelocity", &g_off_VecVelocity) ||
        !conf->GetOffset("CMoveData::m_vecAbsOrigin", &g_off_VecAbsOrigin))
    {
        snprintf(error, maxlength, "Failed to get core CMoveData offsets.");
        gameconfs->CloseGameConfigFile(conf);
        return false;
    }

    conf->GetOffset("CBasePlayer::m_surfaceFriction", &g_off_SurfaceFriction);

    // GroundEntity: try sendprop first, fall back to gamedata
    sm_sendprop_info_t info;
    if (gamehelpers->FindSendPropInfo("CBasePlayer", "m_hGroundEntity", &info))
        g_off_GroundEntity = info.actual_offset;
    else
        conf->GetOffset("CBasePlayer::m_hGroundEntity", &g_off_GroundEntity);

    if (g_off_GroundEntity == -1)
    {
        snprintf(error, maxlength, "Missing CBasePlayer::m_hGroundEntity offset.");
        gameconfs->CloseGameConfigFile(conf);
        return false;
    }

    // Optional compat offsets
    if (gamehelpers->FindSendPropInfo("CBaseEntity", "m_nWaterLevel", &info))
        g_off_WaterLevel = info.actual_offset;
    if (gamehelpers->FindSendPropInfo("CBaseEntity", "m_MoveType", &info))
        g_off_MoveType = info.actual_offset;
    else if (gamehelpers->FindSendPropInfo("CBasePlayer", "m_MoveType", &info))
        g_off_MoveType = info.actual_offset;

    // vtable offsets
    if (!conf->GetOffset("ClipVelocity", &g_vtoff_ClipVelocity) ||
        !conf->GetOffset("TracePlayerBBox", &g_vtoff_TracePlayerBBox) ||
        !conf->GetOffset("AddToTouched", &g_vtoff_AddToTouched))
    {
        snprintf(error, maxlength, "Failed to get vtable offsets.");
        gameconfs->CloseGameConfigFile(conf);
        return false;
    }

    // Optional CSGO vtable offsets
    conf->GetOffset("TraceRay", &g_vtoff_TraceRay);
    conf->GetOffset("CanTraceRay", &g_vtoff_CanTraceRay);
    conf->GetOffset("TraceRayAgainstLeafAndEntityList", &g_vtoff_TraceRayAgainstLeaf);
    conf->GetOffset("LockTraceFilter", &g_vtoff_LockTraceFilter);
    conf->GetOffset("UnlockTraceFilter", &g_vtoff_UnlockTraceFilter);

    // TryPlayerMove signature → detour
    void *pTryPlayerMove = nullptr;
    if (!conf->GetMemSig("CGameMovement::TryPlayerMove", &pTryPlayerMove) || !pTryPlayerMove)
    {
        snprintf(error, maxlength, "Failed to find CGameMovement::TryPlayerMove signature.");
        gameconfs->CloseGameConfigFile(conf);
        return false;
    }

    gpGlobals = g_SMAPI->GetCGlobals();

    g_pDetour = new CSimpleDetour(pTryPlayerMove, (void *)Detour_TryPlayerMove);
    UpdateDetourState();

    // MoveHelper singleton
    void *pSingleton = nullptr;
    if (conf->GetAddress("sm_pSingleton", &pSingleton) && pSingleton)
        g_pMoveHelper = pSingleton;

    // sv_bounce
    if (g_pCVar)
        g_pSvBounce = g_pCVar->FindVar("sv_bounce");

    // Detect CSGO
    {
        const char *engineIface = conf->GetKeyValue("CEngineTrace");
        if (engineIface && strcmp(engineIface, "EngineTraceServer004") == 0)
            g_bIsCSGO = true;
    }

    // Forward
    g_pOnClipVelocity = forwards->CreateForward("MomSurfFix2_OnClipVelocity", ET_Ignore, 4, nullptr,
        Param_Cell,
        Param_Array,
        Param_Array,
        Param_Array
    );

    sharesys->AddNatives(myself, g_Natives);
    sharesys->RegisterLibrary(myself, "momsurffix2_ext");

    gameconfs->CloseGameConfigFile(conf);
    return true;
}

void MomSurfFixExt2::SDK_OnUnload()
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

void MomSurfFixExt2::SDK_OnAllLoaded()
{
    // Manually get IGameHelpers if not already set
    if (!gamehelpers)
    {
        gamehelpers = (IGameHelpers *)sharesys->RequestInterface(SMINTERFACE_GAMEHELPERS_NAME,
                                                                  SMINTERFACE_GAMEHELPERS_VERSION,
                                                                  myself,
                                                                  nullptr);
    }
}

void MomSurfFixExt2::LevelInit(char const *pMapName)
{
    g_Stats.Reset();
}

bool MomSurfFixExt2::QueryRunning(char *error, size_t maxlength)
{
    return true;
}

// ============================================================================
// Natives
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

    for (int i = 0; i < 3; i++)
    {
        inVel[i]       = sp_ftoc(g_LastClipData[client].inVel[i]);
        planeNormal[i] = sp_ftoc(g_LastClipData[client].planeNormal[i]);
        outVel[i]      = sp_ftoc(g_LastClipData[client].outVel[i]);
    }
    return 1;
}

static cell_t Native_GetFixStats(IPluginContext *pContext, const cell_t *params)
{
    cell_t *totalFixes, *avgLoss, *avgGain;
    pContext->LocalToPhysAddr(params[1], &totalFixes);
    pContext->LocalToPhysAddr(params[2], &avgLoss);
    pContext->LocalToPhysAddr(params[3], &avgGain);

    *totalFixes = g_Stats.totalFixes;
    *avgLoss    = sp_ftoc(g_Stats.AvgLoss());
    *avgGain    = sp_ftoc(g_Stats.AvgGain());
    return 1;
}

sp_nativeinfo_t g_Natives[] =
{
    { "MomSurfFix2_GetLastClipData", Native_GetLastClipData },
    { "MomSurfFix2_GetFixStats",     Native_GetFixStats     },
    { nullptr, nullptr }
};

