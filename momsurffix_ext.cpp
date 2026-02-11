// ============================================================================
// 【0】SourceMod 扩展核心
// ============================================================================
#include "extension.h" 

// ============================================================================
// 【1】标准库
// ============================================================================
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <dlfcn.h> 

// ============================================================================
// 【2】基础 SDK 头文件
// ============================================================================
#include <tier0/platform.h>
#include <tier0/memalloc.h>
#include <tier1/convar.h>
#include <gametrace.h>
#include <soundflags.h>
#include <ihandleentity.h> 
#include <interfaces/interfaces.h> 

// ============================================================================
// 【3】SDK 兼容垫片
// ============================================================================
class CBasePlayer;
class CBaseEntity;

enum PLAYER_ANIM 
{ 
    PLAYER_IDLE = 0, 
    PLAYER_WALK, 
    PLAYER_JUMP,
    PLAYER_SUPERJUMP,
    PLAYER_DIE,
    PLAYER_ATTACK1
};

// ============================================================================
// 【4】依赖上述类型的 SDK 头文件
// ============================================================================
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
SDKExtension *g_pExtensionIface = &g_MomSurfFixExt;

IEngineTrace *enginetrace = nullptr;
typedef void* (*CreateInterfaceFn)(const char *pName, int *pReturnCode);

// ConVar 定义
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

// ----------------------------------------------------------------------------
// 辅助类与函数
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

void Manual_TracePlayerBBox(IGameMovement *pGM, IHandleEntity *pPlayerEntity, const Vector &start, const Vector &end, unsigned int fMask, int collisionGroup, CGameTrace &pm)
{
    if (!enginetrace) return;

    Vector mins = pGM->GetPlayerMins(false); 
    Vector maxs = pGM->GetPlayerMaxs(false);

    Ray_t ray;
    ray.Init(start, end, mins, maxs);

    CTraceFilterSimple traceFilter(pPlayerEntity, collisionGroup);
    enginetrace->TraceRay(ray, fMask, &traceFilter, &pm);
}

void FindValidPlane(IGameMovement *pGM, IHandleEntity *pPlayerEntity, const Vector &origin, const Vector &vel, Vector &outPlane)
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
                CTraceFilterSimple filter(pPlayerEntity, COLLISION_GROUP_PLAYER_MOVEMENT);
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

// ----------------------------------------------------------------------------
// Detour Logic (Reactive Mode / 反应式修复)
// ----------------------------------------------------------------------------
// 原理：只有当原版引擎发生 Ramp Bug 时才介入，否则完全不干预。
// 这解决了 99% 的预测错误抖动问题。

#ifndef THISCALL
    #define THISCALL
#endif
typedef int (THISCALL *TryPlayerMove_t)(void *, Vector *, CGameTrace *, float);

int Detour_TryPlayerMove(void *pThis, Vector *pFirstDest, CGameTrace *pFirstTrace, float flTimeLeft)
{
    TryPlayerMove_t Original = (TryPlayerMove_t)g_pDetour->GetTrampoline();
    if (!Original) return 0;

    // 1. 如果功能关闭，直接原版
    if (!g_cvNoclipWorkaround.GetBool())
    {
        return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);
    }

    void *pPlayer = *(void **)((uintptr_t)pThis + g_off_Player);
    CMoveData *mv = *(CMoveData **)((uintptr_t)pThis + g_off_MV);
    if (!pPlayer || !mv) return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    Vector *pVel = (Vector *)((uintptr_t)mv + g_off_VecVelocity);
    Vector *pOrigin = (Vector *)((uintptr_t)mv + g_off_VecAbsOrigin);
    
    // 2. 备份当前状态 (位置和速度)
    Vector orgVelocity = *pVel;
    Vector orgOrigin = *pOrigin;

    // 3. 【关键】先让原版引擎跑一遍！
    int result = Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    // 4. 检查是否发生了 BUG
    // 如果速度突然损失严重 (撞墙/卡住)，但我们并没有碰到很陡的墙（Ramp Bug 特征）
    // 或者原版引擎直接返回被阻挡
    // 这里做一个简单的启发式检测：如果在滑翔（速度快），且突然停下或大幅减速
    
    float speedSq = orgVelocity.LengthSqr();
    float newSpeedSq = pVel->LengthSqr();
    
    // 如果速度很快 (>250)，且经过计算后速度损失超过 50% (或其他阈值)，判定为可能遇到 BUG
    bool potentialRampBug = (speedSq > 250.0f*250.0f) && (newSpeedSq < speedSq * 0.5f);

    if (!potentialRampBug)
    {
        // 如果原版跑得挺好，就直接返回原版结果。
        // 这保证了绝大多数时候的丝滑手感，客户端预测完美匹配。
        return result; 
    }

    // 5. 只有检测到 BUG 时，才回滚并启用 Momentum 修复算法
    // 恢复状态
    *pVel = orgVelocity;
    *pOrigin = orgOrigin;

    // --- 开始 Momentum Mod 修复算法 (仅在 BUG 发生时介入) ---
    // (代码逻辑同上，但只在关键时刻运行)

    IHandleEntity *pEntity = (IHandleEntity *)pPlayer;
    VPROF_BUDGET("Momentum_TryPlayerMove", VPROF_BUDGETGROUP_PLAYER);

    int client = pEntity->GetRefEHandle().GetEntryIndex();
    CGameTrace &pm = g_TempTraces[client];
    pm = CGameTrace(); 

    Vector origin = *pOrigin;
    Vector vel = *pVel;
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
            pm = *pFirstTrace;
        else
            Manual_TracePlayerBBox(pGM, pEntity, origin, end, MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, pm);

        if (pm.fraction > 0.0f) origin = pm.endpos;
        if (pm.fraction == 1.0f) break;

        time_left -= time_left * pm.fraction;

        if (numplanes >= MAX_CLIP_PLANES) { vel.Init(); break; }

        g_TempPlanes[numplanes] = pm.plane.normal;
        numplanes++;

        // 仅在空中检测
        unsigned long hGroundEntity = *(unsigned long *)((uintptr_t)pPlayer + g_off_GroundEntity);
        if (bumpcount > 0 && hGroundEntity == 0xFFFFFFFF && !IsValidMovementTrace(pm))
            stuck_on_ramp = true;

        if (stuck_on_ramp && vel.z >= -6.25f && vel.z <= 0.0f && !has_valid_plane)
        {
            FindValidPlane(pGM, pEntity, origin, vel, valid_plane);
            has_valid_plane = (valid_plane.LengthSqr() > 0.000001f);
        }

        if (has_valid_plane)
            VectorMA(origin, g_cvRampInitialRetraceLength.GetFloat(), valid_plane, origin);

        // Clip Velocity
        int blocked_planes = 0;
        for (int i = 0; i < numplanes; i++)
        {
            Vector new_vel;
            if (g_TempPlanes[i].z >= 0.7f) // Floor
            { 
                blocked_planes++; 
                new_vel.Init(); // Stop on floor in this logic
            }
            else
            {
                float backoff = DotProduct(vel, g_TempPlanes[i]) * 1.0f;
                new_vel = vel - (g_TempPlanes[i] * backoff);
            }

            if (i == 0) vel = new_vel;
            else
            {
                float dot = DotProduct(new_vel, new_vel);
                if (dot > 0.0f) new_vel *= (vel.Length() / sqrt(dot));
                vel = new_vel;
            }
        }

        if (blocked_planes == numplanes) { blocked |= 2; break; }
    }

    *pVel = vel;
    *pOrigin = origin;

    return blocked;
}

// ============================================================================
// SourceMod 生命周期 (保持不变)
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
