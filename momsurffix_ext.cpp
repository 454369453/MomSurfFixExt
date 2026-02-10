// ============================================================================
// 【第一区】标准库与底层兼容层
// ============================================================================
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <stdlib.h> // posix_memalign

// 1. Linux 内存分配垫片
static inline void* linux_aligned_malloc(size_t size, size_t align)
{
    void* ptr = nullptr;
    if (posix_memalign(&ptr, align, size) != 0)
        return nullptr;
    return ptr;
}

// 2. 宏映射
#undef _aligned_malloc
#undef _aligned_free
#define _aligned_malloc(size, align) linux_aligned_malloc(size, align)
#define _aligned_free free

// 3. 基础宏
#ifndef abstract_class
    #define abstract_class class
#endif
#ifndef OVERRIDE
    #define OVERRIDE override
#endif

// ============================================================================
// 【第二区】SDK 核心头文件
// ============================================================================
#include <tier0/platform.h>
#include <tier0/memalloc.h>

// 4. 手动补全被 SDK 可能屏蔽的内联函数
#ifndef MemAlloc_AllocAlignedFileLine
inline void *MemAlloc_AllocAlignedFileLine( size_t size, size_t align, const char *pFileName, int nLine )
{
    return linux_aligned_malloc(size, align);
}
#endif

#ifndef MemAlloc_AllocAligned
inline void *MemAlloc_AllocAligned( size_t size, size_t align )
{
    return linux_aligned_malloc(size, align);
}
#endif

#ifndef MemAlloc_FreeAligned
inline void MemAlloc_FreeAligned( void *pMemBlock )
{
    free(pMemBlock);
}
inline void MemAlloc_FreeAligned( void *pMemBlock, const char *pFileName, int nLine )
{
    free(pMemBlock);
}
#endif

#ifndef MEM_ALLOC_CREDIT_CLASS
    #define MEM_ALLOC_CREDIT_CLASS()
#endif

// ============================================================================
// 【第三区】SourceMod 扩展入口
// ============================================================================
#include "extension.h"
#include "smsdk_config.h"

// ============================================================================
// 【第四区】业务逻辑头文件
// ============================================================================
#include <ihandleentity.h>

class CBaseEntity : public IHandleEntity {};
class CBasePlayer : public CBaseEntity {};

enum PLAYER_ANIM { 
    PLAYER_IDLE, PLAYER_WALK, PLAYER_JUMP, PLAYER_SUPERJUMP, PLAYER_DIE, PLAYER_ATTACK1 
};

#include <engine/IEngineTrace.h>
#include <ispatialpartition.h> 
#include <igamemovement.h>
#include <tier0/vprof.h>
#include "simple_detour.h"

// ============================================================================
// 全局变量
// ============================================================================
#ifndef MAXPLAYERS
#define MAXPLAYERS 65
#endif
#ifndef MAX_CLIP_PLANES
#define MAX_CLIP_PLANES 5
#endif

MomSurfFixExt g_MomSurfFixExt;
IEngineTrace *enginetrace = nullptr;

typedef void* (*CreateInterfaceFn)(const char *pName, int *pReturnCode);

ConVar g_cvRampBumpCount("momsurffix_ramp_bumpcount", "8", FCVAR_NOTIFY);
ConVar g_cvRampInitialRetraceLength("momsurffix_ramp_retrace_length", "0.2", FCVAR_NOTIFY);
ConVar g_cvNoclipWorkaround("momsurffix_enable_noclip_workaround", "1", FCVAR_NOTIFY);
ConVar g_cvBounce("sv_bounce", "0");

int g_off_Player = -1;
int g_off_MV = -1;
int g_off_VecVelocity = -1; 
int g_off_VecAbsOrigin = -1;
int g_off_GroundEntity = -1;

CSimpleDetour *g_pDetour = nullptr;

static CGameTrace g_TempTraces[MAXPLAYERS + 1];
static Vector g_TempPlanes[MAX_CLIP_PLANES];

// 辅助类
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

// 业务逻辑函数
void Manual_TracePlayerBBox(IGameMovement *pGM, const Vector &start, const Vector &end, unsigned int fMask, int collisionGroup, CGameTrace &pm)
{
    if (!enginetrace) return;

    Vector mins = pGM->GetPlayerMins(false); 
    Vector maxs = pGM->GetPlayerMaxs(false);

    Ray_t ray;
    ray.Init(start, end, mins, maxs);

    IHandleEntity *playerEntity = (IHandleEntity *)pGM->GetMovingPlayer();
    
    CTraceFilterSimple traceFilter(playerEntity, collisionGroup);
    enginetrace->TraceRay(ray, fMask, &traceFilter, &pm);
}

void FindValidPlane(IGameMovement *pGM, CBasePlayer *pPlayer, const Vector &origin, const Vector &vel, Vector &outPlane)
{
    if (!enginetrace) return;

    Vector sum_normal = vec3_origin;
    int count = 0;

    for (int x = -1; x <= 1; x++)
    {
        for (int y = -1; y <= 1; y++)
        {
            for (int z = -1; z <= 1; z++)
            {
                if (x == 0 && y == 0 && z == 0) continue;

                Vector dir(x * 0.03125f, y * 0.03125f, z * 0.03125f);
                dir.NormalizeInPlace();
                Vector end = origin + (dir * 0.0625f);

                CGameTrace trace;
                CTraceFilterSimple filter((IHandleEntity*)pPlayer, COLLISION_GROUP_PLAYER_MOVEMENT);
                Ray_t ray;
                ray.Init(origin, end);
                enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &trace);

                if (trace.fraction < 1.0f && trace.plane.normal.z > 0.7f)
                {
                    sum_normal += trace.plane.normal;
                    count++;
                }
            }
        }
    }

    if (count > 0)
    {
        outPlane = sum_normal * (1.0f / count);
        outPlane.NormalizeInPlace();
    }
    else
    {
        outPlane = vec3_origin;
    }
}

bool IsValidMovementTrace(const CGameTrace &tr)
{
    return (tr.fraction > 0.0f || tr.startsolid);
}

// Detour 函数
#ifndef THISCALL
    #define THISCALL
#endif
typedef int (THISCALL *TryPlayerMove_t)(void *, Vector *, CGameTrace *, float);

int Detour_TryPlayerMove(void *pThis, Vector *pFirstDest, CGameTrace *pFirstTrace, float flTimeLeft)
{
    void *pPlayer = *(void **)((uintptr_t)pThis + g_off_Player);
    CMoveData *mv = *(CMoveData **)((uintptr_t)pThis + g_off_MV);

    TryPlayerMove_t Original = (TryPlayerMove_t)g_pDetour->GetTrampoline();

    if (!pPlayer || !mv || !Original) return 0;

    VPROF_BUDGET("Momentum_TryPlayerMove", VPROF_BUDGETGROUP_PLAYER);

    int client = ((IHandleEntity *)pPlayer)->GetRefEHandle().GetEntryIndex();
    
    CGameTrace &pm = g_TempTraces[client];
    pm = CGameTrace(); 

    Vector *pVel = (Vector *)((uintptr_t)mv + g_off_VecVelocity);
    Vector vel = *pVel; 

    Vector *pOrigin = (Vector *)((uintptr_t)mv + g_off_VecAbsOrigin);
    Vector origin = *pOrigin;
    
    if (vel.LengthSqr() < 0.000001f)
    {
        return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft); 
    }

    float time_left = flTimeLeft;
    int numbumps = g_cvRampBumpCount.GetInt();
    int numplanes = 0;
    bool stuck_on_ramp = false;
    Vector valid_plane;
    bool has_valid_plane = false;
    int blocked = 0;

    IGameMovement *pGM = (IGameMovement *)pThis;

    for (int bumpcount = 0; bumpcount < numbumps; bumpcount++)
    {
        if (vel.LengthSqr() == 0.0f) break;

        Vector end;
        VectorMA(origin, time_left, vel, end);

        if (pFirstDest && (end == *pFirstDest))
        {
            pm = *pFirstTrace;
        }
        else
        {
            Manual_TracePlayerBBox(pGM, origin, end, MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, pm);
        }

        if (pm.fraction > 0.0f) origin = pm.endpos;
        if (pm.fraction == 1.0f) break;

        time_left -= time_left * pm.fraction;

        if (numplanes >= MAX_CLIP_PLANES)
        {
            vel.Init();
            break;
        }

        g_TempPlanes[numplanes] = pm.plane.normal;
        numplanes++;

        unsigned long hGroundEntity = *(unsigned long *)((uintptr_t)pPlayer + g_off_GroundEntity);
        bool bIsAirborne = (hGroundEntity == 0xFFFFFFFF); 

        if (bumpcount > 0 && bIsAirborne && !IsValidMovementTrace(pm))
        {
            stuck_on_ramp = true;
        }

        if (g_cvNoclipWorkaround.GetBool() && stuck_on_ramp && vel.z >= -6.25f && vel.z <= 0.0f && !has_valid_plane)
        {
            FindValidPlane(pGM, (CBasePlayer*)pPlayer, origin, vel, valid_plane);
            has_valid_plane = (valid_plane.LengthSqr() > 0.000001f);
        }

        if (has_valid_plane)
        {
            VectorMA(origin, g_cvRampInitialRetraceLength.GetFloat(), valid_plane, origin);
        }

        float allFraction = 0.0f;
        int blocked_planes = 0;
        
        for (int i = 0; i < numplanes; i++)
        {
            Vector new_vel;
            if (g_TempPlanes[i].z >= 0.7f)
            {
                blocked_planes++;
                new_vel.Init();
            }
            else
            {
                float backoff = DotProduct(vel, g_TempPlanes[i]) * 1.0f;
                new_vel = vel - (g_TempPlanes[i] * backoff);
            }

            if (i == 0)
            {
                vel = new_vel;
            }
            else
            {
                float dot = DotProduct(new_vel, new_vel);
                if (dot > 0.0f) new_vel *= (vel.Length() / sqrt(dot));
                vel = new_vel;
            }

            allFraction += pm.fraction;
        }

        if (blocked_planes == numplanes)
        {
            blocked |= 2;
            break;
        }
    }

    *pVel = vel;
    *pOrigin = origin;

    return blocked;
} 
// 注意：上面这个大括号是 Detour_TryPlayerMove 的结束，千万不能少！

// ============================================================================
// 生命周期
// ============================================================================
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

    if (!conf->GetOffset("CBasePlayer::m_hGroundEntity", &g_off_GroundEntity))
    {
         snprintf(error, maxlength, "Missing 'CBasePlayer::m_hGroundEntity'.");
         gameconfs->CloseGameConfigFile(conf);
         return false;
    }

    void *pTryPlayerMoveAddr = nullptr;
    if (!conf->GetMemSig("CGameMovement::TryPlayerMove", &pTryPlayerMoveAddr) || !pTryPlayerMoveAddr)
    {
        snprintf(error, maxlength, "Failed to find TryPlayerMove signature.");
        gameconfs->CloseGameConfigFile(conf);
        return false;
    }

    g_pDetour = new CSimpleDetour(pTryPlayerMoveAddr, (void *)Detour_TryPlayerMove);
    if (!g_pDetour->Enable())
    {
        snprintf(error, maxlength, "Failed to enable detour.");
        delete g_pDetour;
        g_pDetour = nullptr;
        gameconfs->CloseGameConfigFile(conf);
        return false;
    }

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

    gameconfs->CloseGameConfigFile(conf);
    return true;
}

void MomSurfFixExt::SDK_OnUnload()
{
    if (g_pDetour)
    {
        delete g_pDetour;
        g_pDetour = nullptr;
    }
}

void MomSurfFixExt::SDK_OnAllLoaded()
{
}

bool MomSurfFixExt::QueryRunning(char *error, size_t maxlength)
{
    return true;
}

SMEXT_LINK(&g_MomSurfFixExt);
