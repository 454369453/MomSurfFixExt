#include "extension.h"
#include <dhooks>
#include <IGameMovement.h>
#include <CBase.h>
#include <tier0/vprof.h>
#include "smsdk_config.h"
#include "extension.h"  // 包含类声明

// ConVars
ConVar g_cvRampBumpCount("momsurffix_ramp_bumpcount", "8", FCVAR_NOTIFY);
ConVar g_cvRampInitialRetraceLength("momsurffix_ramp_retrace_length", "0.2", FCVAR_NOTIFY);
ConVar g_cvNoclipWorkaround("momsurffix_enable_noclip_workaround", "1", FCVAR_NOTIFY);
ConVar g_cvBounce("sv_bounce", "0");

// 静态池
static CGameTrace g_TempTraces[MAXPLAYERS + 1];
static Vector g_TempPlanes[MAX_CLIP_PLANES];

// DHooks 处理
static IDHooks *g_pDHooks = nullptr;

// Detour 回调
MRESReturn Momentum_TryPlayerMove(DHookReturn *hReturn, DHookParam *pParams)
{
    CGameMovement *pThis = (CGameMovement *)pParams->GetThisPointer();
    Vector *pFirstDest = pParams->Get<Vector *>(1);
    CGameTrace *pFirstTrace = pParams->Get<CGameTrace *>(2);

    VPROF_BUDGET("Momentum_TryPlayerMove", VPROF_BUDGETGROUP_PLAYER);

    CBasePlayer *pPlayer = pThis->player;
    int client = pPlayer->entindex();
    CGameTrace &pm = g_TempTraces[client];
    pm.Reset();

    Vector vel = pThis->mv->m_vecVelocity;
    if (vel.LengthSqr() < 0.000001f)
    {
        hReturn->Set(0);
        return MRES_OVERRIDE;
    }

    Vector origin = pThis->mv->GetAbsOrigin();
    float time_left = pThis->GetFrameTime();
    int blocked = 0;
    int bumpcount;
    int numbumps = g_cvRampBumpCount.GetInt();
    int numplanes = 0;
    bool stuck_on_ramp = false;
    Vector valid_plane;
    bool has_valid_plane = false;

    for (bumpcount = 0; bumpcount < numbumps; bumpcount++)
    {
        if (vel.LengthSqr() == 0.0f)
            break;

        Vector end;
        VectorMA(origin, time_left, vel, end);

        if (pFirstDest && (end == *pFirstDest))
        {
            pm = *pFirstTrace;
        }
        else
        {
            pThis->TracePlayerBBox(origin, end, MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, pm);
        }

        if (pm.fraction > 0.0f)
        {
            origin = pm.endpos;
        }

        if (pm.fraction == 1.0f)
            break;

        time_left -= time_left * pm.fraction;

        if (numplanes >= MAX_CLIP_PLANES)
        {
            vel.Init();
            hReturn->Set(0);
            return MRES_OVERRIDE;
        }

        g_TempPlanes[numplanes] = pm.plane.normal;
        numplanes++;

        // Ramp bug 修复逻辑
        if (bumpcount > 0 && pPlayer->GetGroundEntity() == nullptr && !IsValidMovementTrace(pm))
        {
            stuck_on_ramp = true;
        }

        // Noclip workaround
        if (g_cvNoclipWorkaround.GetBool() && stuck_on_ramp && vel.z >= -6.25f && vel.z <= 0.0f && !has_valid_plane)
        {
            FindValidPlane(pThis, origin, vel, valid_plane);
            has_valid_plane = (valid_plane.LengthSqr() > 0.000001f);
        }

        if (has_valid_plane)
        {
            VectorMA(origin, g_cvRampInitialRetraceLength.GetFloat(), valid_plane, origin);
        }

        // Clip velocity
        float allFraction = 0.0f;
        int blocked_planes = 0;
        for (int i = 0; i < numplanes; i++)
        {
            Vector new_vel;
            ClipVelocity(vel, g_TempPlanes[i], new_vel, 1.0f + g_cvBounce.GetFloat() * (1.0f - pThis->m_surfaceFriction));

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

        if (allFraction > 1.0f)
        {
            vel.Init();
            blocked |= 1;
            break;
        }

        if (numplanes > 1)
        {
            if (numplanes > 5)
                numplanes = 5;

            for (int i = numplanes - 2; i >= 0; i--)
            {
                pThis->TracePlayerBBox(origin, origin, MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, pm);
            }
        }
    }

    pThis->mv->m_vecVelocity = vel;
    pThis->mv->SetAbsOrigin(origin);

    hReturn->Set(blocked);
    return MRES_OVERRIDE;
}

// FindValidPlane 辅助函数 (27-ray 优化版)
void FindValidPlane(CGameMovement *pThis, const Vector &origin, const Vector &vel, Vector &outPlane)
{
    Vector sum_normal = vec3_origin;
    int count = 0;

    for (int x = -1; x <= 1; x++)
    {
        for (int y = -1; y <= 1; y++)
        {
            for (int z = -1; z <= 1; z++)
            {
                if (x == 0 && y == 0 && z == 0)
                    continue;

                Vector dir(x * 0.03125f, y * 0.03125f, z * 0.03125f);
                dir.Normalize();
                Vector end = origin + (dir * 0.0625f);

                CGameTrace trace;
                ITraceFilter *filter = new CTraceFilterSimple(pThis->player, COLLISION_GROUP_PLAYER_MOVEMENT);
                Ray_t ray;
                ray.Init(origin, end);
                enginetrace->TraceRay(ray, MASK_PLAYERSOLID, filter, &trace);
                delete filter;

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
        outPlane.Normalize();
    }
    else
    {
        outPlane = vec3_origin;
    }
}

// IsValidMovementTrace 辅助
bool IsValidMovementTrace(const CGameTrace &tr)
{
    return (tr.fraction > 0.0f || tr.startsolid);
}

// Extension 生命周期
bool MomSurfFixExt::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
    sharesys->AddDependency(myself, "dhooks.ext", true, true);

    GameData *g_pGameData = gameconfs->LoadGameConfigFile("momsurffix_fix.games");
    if (!g_pGameData)
    {
        snprintf(error, maxlength, "Could not read momsurffix_fix.games.txt");
        return false;
    }

    Handle_t hDetour = dhooks->CreateDetour(g_pGameData, "CGameMovement::TryPlayerMove", CALLCONV_THISCALL, RETURNTYPE(Int), PARAMTYPES(CGameMovement *, Vector *, CGameTrace *));
    if (!hDetour)
    {
        snprintf(error, maxlength, "Failed to setup TryPlayerMove detour");
        gameconfs->CloseGameConfigFile(g_pGameData);
        return false;
    }

    dhooks->EnableDetour(hDetour, false, Momentum_TryPlayerMove);

    gameconfs->CloseGameConfigFile(g_pGameData);
    return true;
}

void MomSurfFixExt::SDK_OnUnload()
{
    // 清理资源
}

void MomSurfFixExt::SDK_OnAllLoaded()
{
    // 额外初始化
}

bool MomSurfFixExt::QueryRunning(char *error, size_t maxlength)
{
    SM_CHECK_DEPENDENCY("dhooks.ext", error, maxlength);
    return true;
}

// 全局实例
SMEXT_LINK(&g_MomSurfFixExt);
