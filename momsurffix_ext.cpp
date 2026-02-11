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
SDKExtension *g_pExtensionIface = &g_MomSurfFixExt;

IEngineTrace *enginetrace = nullptr;
typedef void* (*CreateInterfaceFn)(const char *pName, int *pReturnCode);

// 前向声明回调
void OnEnableChanged(IConVar *var, const char *pOldValue, float flOldValue);

// ConVar 定义
// 注意：移除了 FCVAR_NOTIFY，添加了回调函数
ConVar g_cvEnable("momsurffix_enable", "1", 0, "Enable Surf Bug Fix", OnEnableChanged);
ConVar g_cvDebug("momsurffix_debug", "0", 0, "Print debug info");

int g_off_Player = -1;
int g_off_MV = -1;
int g_off_VecVelocity = -1; 
int g_off_VecAbsOrigin = -1;
int g_off_GroundEntity = -1;

CSimpleDetour *g_pDetour = nullptr;

// ----------------------------------------------------------------------------
// 动态开关逻辑 (Dynamic Hooking)
// ----------------------------------------------------------------------------
// 这是最彻底的开关：直接卸载钩子。
// 当 momsurffix_enable 0 时，引擎会直接调用原版函数，扩展代码一行都不会跑。
void UpdateDetourState()
{
    if (!g_pDetour) return;

    bool shouldEnable = g_cvEnable.GetBool();
    
    if (shouldEnable)
    {
        g_pDetour->Enable();
        // Msg("[MomSurfFix] Detour ENABLED.\n");
    }
    else
    {
        g_pDetour->Disable();
        // Msg("[MomSurfFix] Detour DISABLED. Running pure vanilla physics.\n");
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

void TracePlayerBBox(const Vector &start, const Vector &end, IHandleEntity *pPlayerEntity, int collisionGroup, CGameTrace &pm)
{
    if (!enginetrace) return;
    
    Ray_t ray;
    Vector mins(-16, -16, 0);
    Vector maxs(16, 16, 72); 
    ray.Init(start, end, mins, maxs);

    CTraceFilterSimple traceFilter(pPlayerEntity, collisionGroup);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &traceFilter, &pm);
}

// ----------------------------------------------------------------------------
// Detour Logic (完全信任原版 + 事后补救)
// ----------------------------------------------------------------------------
#ifndef THISCALL
    #define THISCALL
#endif
typedef int (THISCALL *TryPlayerMove_t)(void *, Vector *, CGameTrace *, float);

int Detour_TryPlayerMove(void *pThis, Vector *pFirstDest, CGameTrace *pFirstTrace, float flTimeLeft)
{
    // 如果走到这里，说明 g_cvEnable 肯定是 1 (否则钩子就被卸载了)
    // 所以不需要再判断 g_cvEnable

    TryPlayerMove_t Original = (TryPlayerMove_t)g_pDetour->GetTrampoline();
    if (!Original) return 0;

    void *pPlayer = *(void **)((uintptr_t)pThis + g_off_Player);
    CMoveData *mv = *(CMoveData **)((uintptr_t)pThis + g_off_MV);
    if (!pPlayer || !mv) return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    Vector *pVel = (Vector *)((uintptr_t)mv + g_off_VecVelocity);
    Vector *pOrigin = (Vector *)((uintptr_t)mv + g_off_VecAbsOrigin);

    // 记录原始状态
    Vector preVelocity = *pVel;
    Vector preOrigin = *pOrigin;
    float preSpeedSq = preVelocity.LengthSqr();

    // 执行原版逻辑
    int result = Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    // --- 仅在非常特定的条件下才介入修复 ---
    
    // 1. 必须是滑翔状态 (速度 > 250)
    if (preSpeedSq < 250.0f * 250.0f) return result;

    // 2. 必须在空中
    unsigned long hGroundEntity = *(unsigned long *)((uintptr_t)pPlayer + g_off_GroundEntity);
    if (hGroundEntity != 0xFFFFFFFF) return result;

    // 3. 必须发生了异常减速 (Ramp Bug 特征)
    float postSpeedSq = pVel->LengthSqr();
    // 允许损失 25% 的速度 (正常碰撞)，如果损失更多，视为 Bug
    if (postSpeedSq > preSpeedSq * 0.75f) return result;

    // --- 执行修复 ---
    IHandleEntity *pEntity = (IHandleEntity *)pPlayer;
    CGameTrace trace;
    
    // 探测前方
    Vector endPos = preOrigin + (preVelocity * flTimeLeft);
    TracePlayerBBox(preOrigin, endPos, pEntity, COLLISION_GROUP_PLAYER_MOVEMENT, trace);

    if (trace.DidHit() && trace.plane.normal.z < 0.7f)
    {
        // 重新计算滑行向量
        float backoff = DotProduct(preVelocity, trace.plane.normal);
        if (backoff < 0.0f)
        {
            Vector fixVel = preVelocity - (trace.plane.normal * backoff);

            // 安全限速 (防止飞天)
            if (fixVel.z > 600.0f) fixVel.z = 600.0f;

            // 应用修复
            *pVel = fixVel;
            
            // Noclip 推离墙体
            if (trace.plane.normal.z > 0.0f) 
                 *pOrigin = trace.endpos + (trace.plane.normal * 0.1f);

            if (g_cvDebug.GetBool())
                Msg("[MomSurfFix] Fixed Ramp Bug! %.0f -> %.0f\n", sqrt(preSpeedSq), fixVel.Length());
        }
    }

    return result;
}

// ----------------------------------------------------------------------------
// SDK 生命周期
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

    // 初始化 Detour 对象，但先不 Enable
    g_pDetour = new CSimpleDetour(pTryPlayerMoveAddr, (void *)Detour_TryPlayerMove);
    
    // 根据 ConVar 初始值决定是否 Enable
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

    // 手动注册 ConVar (因为我们是手动模式)
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
        // 确保卸载时恢复原状
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
