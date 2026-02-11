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

// ConVar 定义
// 这里的参数意义变了：主要控制是否开启修正
ConVar g_cvEnable("momsurffix_enable", "1", FCVAR_NOTIFY, "Enable Surf Bug Fix");
ConVar g_cvDebug("momsurffix_debug", "0", FCVAR_NOTIFY, "Print debug info when fix activates");

int g_off_Player = -1;
int g_off_MV = -1;
int g_off_VecVelocity = -1; 
int g_off_VecAbsOrigin = -1;
int g_off_GroundEntity = -1;

CSimpleDetour *g_pDetour = nullptr;

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

// 简单的射线检测，用于找回丢失的法线
void TracePlayerBBox(const Vector &start, const Vector &end, IHandleEntity *pPlayerEntity, int collisionGroup, CGameTrace &pm)
{
    if (!enginetrace) return;
    
    // CS:GO 玩家标准碰撞盒 (这里取通用的滑翔 Hull 大小)
    // 为了更通用的修复，我们用射线探测前方即可，不需要极其精确的 Hull，
    // 因为 Ramp Bug 通常发生在表面判定错误上。
    Ray_t ray;
    Vector mins(-16, -16, 0);
    Vector maxs(16, 16, 72); 
    ray.Init(start, end, mins, maxs);

    CTraceFilterSimple traceFilter(pPlayerEntity, collisionGroup);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &traceFilter, &pm);
}

// ----------------------------------------------------------------------------
// Detour Logic (仿插件逻辑：事后修正版)
// ----------------------------------------------------------------------------
#ifndef THISCALL
    #define THISCALL
#endif
typedef int (THISCALL *TryPlayerMove_t)(void *, Vector *, CGameTrace *, float);

int Detour_TryPlayerMove(void *pThis, Vector *pFirstDest, CGameTrace *pFirstTrace, float flTimeLeft)
{
    TryPlayerMove_t Original = (TryPlayerMove_t)g_pDetour->GetTrampoline();
    if (!Original) return 0;

    // 1. 如果没开插件，直接返回原版
    if (!g_cvEnable.GetBool())
    {
        return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);
    }

    void *pPlayer = *(void **)((uintptr_t)pThis + g_off_Player);
    CMoveData *mv = *(CMoveData **)((uintptr_t)pThis + g_off_MV);
    if (!pPlayer || !mv) return Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    Vector *pVel = (Vector *)((uintptr_t)mv + g_off_VecVelocity);
    Vector *pOrigin = (Vector *)((uintptr_t)mv + g_off_VecAbsOrigin);

    // 2. 记录【移动前】的状态
    Vector preVelocity = *pVel;
    Vector preOrigin = *pOrigin;
    float preSpeedSq = preVelocity.LengthSqr();

    // 3. 【完全信任】让 CS:GO 原版引擎先跑完这一帧
    // 这保证了平时走路、跳跃、正常的滑翔手感 100% 原汁原味
    int result = Original(pThis, pFirstDest, pFirstTrace, flTimeLeft);

    // 4. 【事后验尸】检查是否发生了 Ramp Bug
    // 只有同时满足以下条件，才判定为 BUG：
    
    // 条件 A: 移动前是在滑翔状态 (速度足够快，> 250)
    if (preSpeedSq < 250.0f * 250.0f) return result;

    // 条件 B: 依然在空中 (没有落地)
    // 0xFFFFFFFF = -1 (FL_ONGROUND 标志位通常对应 hGroundEntity 有效性)
    unsigned long hGroundEntity = *(unsigned long *)((uintptr_t)pPlayer + g_off_GroundEntity);
    bool bIsAirborne = (hGroundEntity == 0xFFFFFFFF);
    if (!bIsAirborne) return result;

    // 条件 C: 移动后速度骤降
    // Ramp Bug 的特征是：明明在滑翔，速度突然归零或大幅损失。
    // 这里我们设定阈值：如果速度保留不到原来的 70%，就算作异常。
    float postSpeedSq = pVel->LengthSqr();
    if (postSpeedSq > preSpeedSq * 0.7f) return result;

    // 5. 【执行修复】这就是你要的“像插件一样”的处理
    // 既然原版引擎算错了（把你卡停了），我们就手动算一个“正确的滑行方向”还给你
    
    IHandleEntity *pEntity = (IHandleEntity *)pPlayer;
    CGameTrace trace;
    
    // 重新发射一条射线，探测前方撞到了什么
    // 预测位置：按原速度移动一帧的距离
    Vector endPos = preOrigin + (preVelocity * flTimeLeft);
    TracePlayerBBox(preOrigin, endPos, pEntity, COLLISION_GROUP_PLAYER_MOVEMENT, trace);

    // 如果真的撞到了东西 (DidHit)，而且这个东西是个陡坡 (plane.z < 0.7)
    // 注意：如果是平地 (z >= 0.7) 导致的减速，那是正常落地，不需要修
    if (trace.DidHit() && trace.plane.normal.z < 0.7f)
    {
        // 核心修复算法：ClipVelocity
        // 计算完美的滑行向量：去掉垂直于墙面的速度分量
        // newVel = oldVel - (normal * (oldVel dot normal))
        // 这里的 1.0f 代表无摩擦力反弹（滑翔理想状态）
        float backoff = DotProduct(preVelocity, trace.plane.normal);
        
        // 只有当实际上是撞向墙面时才修复（避免背离墙面时被吸回去）
        if (backoff < 0.0f)
        {
            Vector fixVel = preVelocity - (trace.plane.normal * backoff);

            // 【安全限制】防止飞天
            // 如果这个新速度的垂直分量太离谱，强制压住。
            // 800 u/s 差不多是正常弹跳极限，超过这个通常就是物理 Bug 了。
            if (fixVel.z > 800.0f) fixVel.z = 800.0f;

            // 应用修复！把正确的速度写回引擎
            *pVel = fixVel;
            
            // Noclip Workaround: 稍微把人往法线方向推一点点，防止下一帧还陷在墙里
            // 只推 0.1 单位，微不可察，但能防止连续卡顿
            if (trace.plane.normal.z > 0.0f) 
            {
                 *pOrigin = trace.endpos + (trace.plane.normal * 0.1f);
            }

            if (g_cvDebug.GetBool())
            {
                // 如果需要调试，可以在控制台看到修复日志
                // Msg("[MomSurfFix] Fixed Ramp Bug! Speed restored.\n");
            }
        }
    }

    return result;
}

// ... (以下 SDK_OnLoad 等代码保持完全一致，包含手动注册 ConVar 逻辑) ...
// 为了确保完整性，请保留上一版文件中的生命周期函数。

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
