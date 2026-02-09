#include "extension.h"

// ---------------------------------------------------------
// 兼容性修复区域
// ---------------------------------------------------------

// 1. 解决 enum PLAYER_ANIM 前置声明报错
// 我们手动定义它，骗过 imovehelper.h
enum PLAYER_ANIM { 
    PLAYER_IDLE, PLAYER_WALK, PLAYER_JUMP, PLAYER_SUPERJUMP, PLAYER_DIE, PLAYER_ATTACK1 
};

// 2. 前置声明类，避免引用 cbase.h
class CBasePlayer;
class CBaseEntity;

// 3. 引入必要的 SDK 头文件
#include <ihandleentity.h> // 用于 GetRefEHandle
#include <igamemovement.h> // 用于 CGameMovement

#include <tier0/vprof.h>
#include "smsdk_config.h"
#include "simple_detour.h"

// ---------------------------------------------------------
// 辅助类
// ---------------------------------------------------------
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

// ---------------------------------------------------------
// 全局变量
// ---------------------------------------------------------
ConVar g_cvRampBumpCount("momsurffix_ramp_bumpcount", "8", FCVAR_NOTIFY);
ConVar g_cvRampInitialRetraceLength("momsurffix_ramp_retrace_length", "0.2", FCVAR_NOTIFY);
ConVar g_cvNoclipWorkaround("momsurffix_enable_noclip_workaround", "1", FCVAR_NOTIFY);
ConVar g_cvBounce("sv_bounce", "0");

// 偏移量
int g_off_Player = -1;
int g_off_MV = -1;
int g_off_VecVelocity = -1; 
int g_off_VecAbsOrigin = -1;
int g_off_GroundEntity = -1; // 新增：用于检测地面

CSimpleDetour *g_pDetour = nullptr;

static CGameTrace g_TempTraces[MAXPLAYERS + 1];
static Vector g_TempPlanes[MAX_CLIP_PLANES];

// ---------------------------------------------------------
// 辅助函数声明
// ---------------------------------------------------------
void FindValidPlane(CGameMovement *pThis, CBasePlayer *pPlayer, const Vector &origin, const Vector &vel, Vector &outPlane);
bool IsValidMovementTrace(const CGameTrace &tr);

// ---------------------------------------------------------
// 核心 Detour 逻辑
// ---------------------------------------------------------
typedef int (*TryPlayerMove_t)(CGameMovement *, Vector *, CGameTrace *, float);

int Detour_TryPlayerMove(CGameMovement *pThis, Vector *pFirstDest, CGameTrace *pFirstTrace, float flTimeLeft)
{
    // 1. 获取 CBasePlayer 指针 (视为 void* 或 IHandleEntity*)
    void *pPlayer = *(void **)((uintptr_t)pThis + g_off_Player);
    CMoveData *mv = *(CMoveData **)((uintptr_t)pThis + g_off_MV);

    TryPlayerMove_t Original = (TryPlayerMove_t)g_pDetour->GetTrampoline();

    if (!pPlayer || !mv || !Original)
    {
        return 0;
    }

    VPROF_BUDGET("Momentum_TryPlayerMove", VPROF_BUDGETGROUP_PLAYER);

    // 2. 获取 Client Index (通过 IHandleEntity 接口，不需要 CBasePlayer 定义)
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
    int blocked = 0;
    int numbumps = g_cvRampBumpCount.GetInt();
    int numplanes = 0;
    bool stuck_on_ramp = false;
    Vector valid_plane;
    bool has_valid_plane = false;

    // 3. 核心循环
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
            // 需要强转类型以匹配函数签名
            pThis->TracePlayerBBox(origin, end, MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, pm);
        }

        if (pm.fraction > 0.0f)
        {
            origin = pm.endpos;
        }

        if (pm.fraction == 1.0f) break;

        time_left -= time_left * pm.fraction;

        if (numplanes >= MAX_CLIP_PLANES)
        {
            vel.Init();
            break;
        }

        g_TempPlanes[numplanes] = pm.plane.normal;
        numplanes++;

        // 4. 判断是否在地面的逻辑 (替代 GetGroundEntity() == NULL)
        // 读取 m_hGroundEntity 句柄
        // CHandle 其实就是一个 unsigned long，如果为 0xFFFFFFFF (-1) 则表示无效(空中)
        unsigned long hGroundEntity = *(unsigned long *)((uintptr_t)pPlayer + g_off_GroundEntity);
        bool bIsAirborne = (hGroundEntity == 0xFFFFFFFF); // INVALID_EHANDLE_INDEX

        if (bumpcount > 0 && bIsAirborne && !IsValidMovementTrace(pm))
        {
            stuck_on_ramp = true;
        }

        if (g_cvNoclipWorkaround.GetBool() && stuck_on_ramp && vel.z >= -6.25f && vel.z <= 0.0f && !has_valid_plane)
        {
            FindValidPlane(pThis, (CBasePlayer*)pPlayer, origin, vel, valid_plane);
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
            new_vel = vel - (g_TempPlanes[i] * DotProduct(vel, g_TempPlanes[i]) * 1.0f);

            if (g_TempPlanes[i].normal.z >= 0.7f)
            {
                blocked_planes++;
                new_vel.Init();
            }

            if (i == 0)
            {
                vel = new_vel;
            }
            else
            {
                float dot = DotProduct(new_vel, new_vel);
                if (dot > 0.0f)
                    new_vel *= (vel.Length() / sqrt(dot));

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

// ---------------------------------------------------------
// 辅助函数
// ---------------------------------------------------------
void FindValidPlane(CGameMovement *pThis, CBasePlayer *pPlayer, const Vector &origin, const Vector &vel, Vector &outPlane)
{
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
                // 这里的 pPlayer 实际上需要是 IHandleEntity*
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

// ---------------------------------------------------------
// 生命周期
// ---------------------------------------------------------
bool MomSurfFixExt::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
    char conf_error[255];
    IGameConfig *conf = nullptr;
    if (!gameconfs->LoadGameConfigFile("momsurffix_fix.games", &conf, conf_error, sizeof(conf_error)))
    {
        snprintf(error, maxlength, "Could not read momsurffix_fix.games: %s", conf_error);
        return false;
    }

    // 获取必要的偏移量
    if (!conf->GetOffset("CGameMovement::player", &g_off_Player) ||
        !conf->GetOffset("CGameMovement::mv", &g_off_MV) ||
        !conf->GetOffset("CMoveData::m_vecVelocity", &g_off_VecVelocity) ||
        !conf->GetOffset("CMoveData::m_vecAbsOrigin", &g_off_VecAbsOrigin))
    {
        snprintf(error, maxlength, "Failed to get core offsets from gamedata.");
        gameconfs->CloseGameConfigFile(conf);
        return false;
    }

    // 获取 GroundEntity 偏移 (如果 gamedata 没更新，这里会失败)
    if (!conf->GetOffset("CBasePlayer::m_hGroundEntity", &g_off_GroundEntity))
    {
        snprintf(error, maxlength, "Missing 'CBasePlayer::m_hGroundEntity' in gamedata.");
        gameconfs->CloseGameConfigFile(conf);
        return false;
    }

    void *pTryPlayerMoveAddr = nullptr;
    if (!conf->GetMemSig("CGameMovement::TryPlayerMove", &pTryPlayerMoveAddr) || !pTryPlayerMoveAddr)
    {
        snprintf(error, maxlength, "Failed to find signature for TryPlayerMove.");
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
