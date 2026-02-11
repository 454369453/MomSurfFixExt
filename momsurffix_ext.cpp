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
#include <algorithm>
#include <cmath>

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
    PLAYER_IDLE = 0, PLAYER_WALK, PLAYER_JUMP, PLAYER_SUPERJUMP, PLAYER_DIE, PLAYER_ATTACK1
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

MomSurfFixExt g_MomSurfFixExt;

// 关键：定义全局接口指针，供 SDK 自动生成的入口使用
SDKExtension *g_pExtensionIface = &g_MomSurfFixExt;

IEngineTrace *enginetrace = nullptr;
typedef void* (*CreateInterfaceFn)(const char *pName, int *pReturnCode);

// 前向声明回调
void OnEnableChanged(IConVar *var, const char *pOldValue, float flOldValue);

// ConVar 定义
ConVar g_cvEnable("momsurffix_enable", "1", 0, "Enable Surf Bug Fix", OnEnableChanged);
ConVar g_cvDebug("momsurffix_debug", "0", 0, "Print debug info");

// 偏移量定义
int g_off_Player = -1;
int g_off_MV = -1;
int g_off_VecVelocity = -1; 
int g_off_VecAbsOrigin = -1;
int g_off_GroundEntity = -1;
int g_off_VecMins = -1;
int g_off_VecMaxs = -1;

CSimpleDetour *g_pDetour = nullptr;

// ----------------------------------------------------------------------------
// 动态开关逻辑
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
// Detour Logic (智能判断版：Surf无感出坡 + Bhop正常起跳)
// ----------------------------------------------------------------------------
#ifndef THISCALL
    #define THISCALL
#endif
typedef int (THISCALL *TryPlayerMove_t)(void *, Vector *, CGameTrace *, float);

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

    Vector *pVel = (Vector *)((uintptr_t)mv + g_off_VecVelocity);
    Vector *pOrigin = (Vector *)((uintptr_t)mv + g_off_VecAbsOrigin);

    // 1. 记录原始状态
    Vector preVelocity = *pVel;
    Vector preOrigin = *pOrigin;
    float preSpeedSq = preVelocity.LengthSqr();
    
    unsigned long *pGroundEntity = (unsigned long *)((uintptr_t)pPlayer + g_off_GroundEntity);
    unsigned long hGroundEntityPre = *pGroundEntity;

    // 2. 运行原版引擎
    int result = Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    // ========================================================================
    // 【智能落地优化】Smart Anti-Landing-Punch
    // ========================================================================
    // 目的：区分"Surf滑出坡"和"Bhop跳落地"
    // 逻辑：如果刚落地，且垂直速度很小(<100)，说明是滑行切出，强制滞空以消除震动。
    //       如果垂直速度大(>100)，说明是跳跃落地，保留落地状态以便起跳。
    
    unsigned long hGroundEntityPost = *pGroundEntity;

    // 检查：刚落地 + 速度快(Surf状态)
    if (hGroundEntityPre == 0xFFFFFFFF && hGroundEntityPost != 0xFFFFFFFF && preSpeedSq > 250.0f * 250.0f)
    {
        // 检查垂直速度 (绝对值)
        // Bhop落地通常 > 300 (800重力下起跳落地)
        // Surf出坡通常 < 50
        if (std::abs(pVel->z) < 100.0f)
        {
             // 判定为 Surf 平滑出坡 -> 强制滞空，消除顿挫
             *pGroundEntity = 0xFFFFFFFF;
        }
    }

    // ========================================================================
    // 撞坡修复逻辑 (Ramp Fix)
    // ========================================================================

    if (preSpeedSq < 250.0f * 250.0f) return result;

    float postSpeedSq = pVel->LengthSqr();
    if (postSpeedSq > preSpeedSq * 0.97f) return result;

    IHandleEntity *pEntity = (IHandleEntity *)pPlayer;
    CGameTrace trace;
    
    Vector endPos = preOrigin + (preVelocity * flTimeLeft);
    TracePlayerBBox(preOrigin, endPos, pPlayer, pEntity, COLLISION_GROUP_PLAYER_MOVEMENT, trace);

    if (trace.DidHit() && trace.plane.normal.z < 0.7f)
    {
        float backoff = DotProduct(preVelocity, trace.plane.normal);
        
        if (backoff < 0.0f)
        {
            Vector fixVel = preVelocity - (trace.plane.normal * backoff);

            if (trace.plane.normal.z > 0.0f) 
            {
                 *pOrigin = trace.endpos + (trace.plane.normal * 0.01f);
            }

            *pVel = fixVel;
            
            // 撞坡修复时，强制滞空 (防止Bug导致的震动)
            *pGroundEntity = 0xFFFFFFFF;
            
            if (g_cvDebug.GetBool())
                Msg("[MomSurfFix] FIXED! Speed: %.0f -> %.0f\n", sqrt(preSpeedSq), fixVel.Length());
        }
    }

    return result;
}

// ----------------------------------------------------------------------------
// SDK 生命周期 (保持不变)
// ----------------------------------------------------------------------------
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

    if (gamehelpers->FindSendPropInfo("CBasePlayer", "m_vecMins", &info))
    {
        g_off_VecMins = info.actual_offset;
    }
    if (gamehelpers->FindSendPropInfo("CBasePlayer", "m_vecMaxs", &info))
    {
        g_off_VecMaxs = info.actual_offset;
    }

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
}

void MomSurfFixExt::SDK_OnAllLoaded()
{
}

bool MomSurfFixExt::QueryRunning(char *error, size_t maxlength)
{
    return true;
}
