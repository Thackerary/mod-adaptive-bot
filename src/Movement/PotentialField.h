#pragma once

#include "Creature.h"
#include "Unit.h"
#include "Map.h"
#include "DangerZones.h"
#include <cmath>
#include <list>
#include <vector>

struct FieldVector2D
{
    float x{ 0.0f };
    float y{ 0.0f };

    FieldVector2D() = default;
    FieldVector2D(float _x, float _y) : x(_x), y(_y) {}

    float Length() const { return std::sqrt(x * x + y * y); }

    FieldVector2D Normalized() const
    {
        float const len = Length();
        if (len < 0.0001f)
            return { 0.0f, 0.0f };
        return { x / len, y / len };
    }

    FieldVector2D operator+(FieldVector2D const& other) const { return { x + other.x, y + other.y }; }
    FieldVector2D operator-(FieldVector2D const& other) const { return { x - other.x, y - other.y }; }
    FieldVector2D operator*(float scalar) const { return { x * scalar, y * scalar }; }
};

class PotentialField
{
public:
    static bool CalculateNextPosition(
        Creature* bot,
        Unit* target,
        float optDistance,
        bool isMeleeBehind,
        bool isTankFront,
        std::vector<DangerZone> const& dangerZones,
        float& outX, float& outY, float& outZ)
    {
        if (!bot || !target || !bot->IsAlive())
            return false;

        if (bot->HasUnitState(UNIT_STATE_CASTING) || bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return false;

        FieldVector2D totalForce{ 0.0f, 0.0f };

        // 1. 引力计算 (Attraction)
        float targetAnchorX = target->GetPositionX();
        float targetAnchorY = target->GetPositionY();

        // 近战输出：引力锚点设在目标正后方 2.0 码处（背身位规避顺劈与招架加速）
        if (isMeleeBehind && !isTankFront)
        {
            // 近战输出：引力锚点直接锚定目标正后方 1.8 码处
            float const behindAngle = target->GetOrientation() + static_cast<float>(M_PI);
            targetAnchorX += 1.8f * std::cos(behindAngle);
            targetAnchorY += 1.8f * std::sin(behindAngle);

            FieldVector2D const dirToAnchor = {
                targetAnchorX - bot->GetPositionX(),
                targetAnchorY - bot->GetPositionY()
            };

            float const distToAnchor = dirToAnchor.Length();
            // 进入背后 0.8 码死区即算就位，移除背身锚点内部的离心排斥，
            // 否则随从抵达落点后会被自己的斥力推开，形成高频轨道弹簧震颤。
            if (distToAnchor > 0.8f)
            {
                totalForce = totalForce + dirToAnchor.Normalized() * std::min(1.2f, distToAnchor * 0.8f);
            }
        }
        else
        {
            // 坦克与远程：维持距目标中心 optDistance 的向心引力/离心斥力
            FieldVector2D const dirToTarget = {
                targetAnchorX - bot->GetPositionX(),
                targetAnchorY - bot->GetPositionY()
            };

            float const distToTarget = dirToTarget.Length();
            float const deadZone = 2.5f;

            if (distToTarget > optDistance + deadZone)
            {
                totalForce = totalForce + dirToTarget.Normalized() * 1.2f;
            }
            else if (distToTarget < std::max(1.0f, optDistance - deadZone))
            {
                float const pushStrength = 1.5f * (1.0f - (distToTarget / optDistance));
                totalForce = totalForce - dirToTarget.Normalized() * pushStrength;
            }
        }

        // 2. 友军防挤压斥力 (Personal Space)
        float const personalSpace = 3.5f;
        std::list<Creature*> nearbyCreatures;
        bot->GetCreaturesWithEntryInRange(nearbyCreatures, personalSpace, 0);

        for (Creature* ally : nearbyCreatures)
        {
            if (ally == bot || !ally->IsAlive() || !ally->IsFriendlyTo(bot) || !ally->IsInCombat())
                continue;

            float const dist = bot->GetDistance2d(ally);
            if (dist < 0.1f)
            {
                totalForce = totalForce + FieldVector2D(0.5f, 0.5f);
                continue;
            }

            FieldVector2D const pushDir = {
                bot->GetPositionX() - ally->GetPositionX(),
                bot->GetPositionY() - ally->GetPositionY()
            };

            float const factor = (personalSpace - dist) / personalSpace;
            totalForce = totalForce + pushDir.Normalized() * (factor * 0.7f);
        }

        // 3. 动态危险区域斥力叠加 (Danger Zones Repulsion)
        uint32 const currentMapId = bot->GetMapId();
        for (auto const& zone : dangerZones)
        {
            if (zone.mapId != 0 && zone.mapId != currentMapId)
                continue;

            if (zone.type == DangerZoneType::CIRCLE)
            {
                float const dx = bot->GetPositionX() - zone.x;
                float const dy = bot->GetPositionY() - zone.y;
                float const distToZone = std::sqrt(dx * dx + dy * dy);

                // 在火圈半径 + 2 码缓冲内产生向外强烈排斥推力
                if (distToZone < zone.radius + 2.0f)
                {
                    FieldVector2D pushDir = (distToZone < 0.1f) ? FieldVector2D(1.0f, 0.0f) : FieldVector2D(dx, dy).Normalized();
                    float const intensity = 3.0f * (1.0f - (distToZone / (zone.radius + 2.0f)));
                    totalForce = totalForce + pushDir * intensity;
                }
            }
            else if (zone.type == DangerZoneType::FRONTAL_CONE)
            {
                // 坦克彻底豁免正面顺劈/吐息斥力：坦克的核心职责是正面承伤聚怪，
                // 一旦被斥力顶去侧后方，首领仇恨随之转头，顺劈反而横扫全团。
                if (isTankFront)
                    continue;

                // 顺劈/吐息的原点与朝向必须动态锚定目标的实时坐标与面向：
                // 首领在战斗中持续转向，任何静态快照坐标都会立即失效。
                float const originX = target->GetPositionX();
                float const originY = target->GetPositionY();
                float const coneOrient = target->GetOrientation();

                float const dx = bot->GetPositionX() - originX;
                float const dy = bot->GetPositionY() - originY;
                float const dist = std::sqrt(dx * dx + dy * dy);

                float const maxDist = zone.radius + 2.0f;
                if (dist < maxDist && dist > 0.001f)
                {
                    float const phi = std::atan2(dy, dx);
                    float diffAngle = phi - coneOrient;
                    while (diffAngle > static_cast<float>(M_PI))  diffAngle -= static_cast<float>(2.0 * M_PI);
                    while (diffAngle < -static_cast<float>(M_PI)) diffAngle += static_cast<float>(2.0 * M_PI);

                    float const halfAngle = (zone.coneAngle * 0.5f) + 0.2f; // 附带 0.2 rad 角度缓冲
                    if (std::abs(diffAngle) < halfAngle)
                    {
                        // 位于扇形内部：向最近的一侧切线方向 + 背后合成逃逸合力。
                        // 侧移权重高于后退，避免瞬间把近战距离拉爆导致全程断输出。
                        float const sign = (diffAngle >= 0.0f) ? 1.0f : -1.0f;
                        FieldVector2D const lateralPush(-std::sin(phi) * sign, std::cos(phi) * sign);
                        FieldVector2D const rearPush(-std::cos(coneOrient), -std::sin(coneOrient));

                        float const distFactor = 1.0f - (dist / maxDist);
                        float const angleFactor = 1.0f - (std::abs(diffAngle) / halfAngle);
                        float const intensity = 4.0f * distFactor * (0.5f + 0.5f * angleFactor);

                        FieldVector2D const escapeDir = (lateralPush * 0.7f + rearPush * 0.5f).Normalized();
                        totalForce = totalForce + escapeDir * intensity;
                    }
                }
            }
        }

        // 4. 合力收敛与物理落点校验
        float const forceMagnitude = totalForce.Length();
        if (forceMagnitude < 0.2f)
            return false;

        FieldVector2D const moveDir = totalForce.Normalized();
        float const stepLength = 3.0f;

        outX = bot->GetPositionX() + moveDir.x * stepLength;
        outY = bot->GetPositionY() + moveDir.y * stepLength;
        outZ = bot->GetMap()->GetHeight(bot->GetPhaseMask(), outX, outY, bot->GetPositionZ());

        if (std::abs(outZ - bot->GetPositionZ()) > 3.0f)
            outZ = bot->GetPositionZ();

        return true;
    }

    /// @brief 向后兼容重载：未接入战后归因危险禁区时，退化为纯引力 + 友军防挤压势场。
    static bool CalculateNextPosition(Creature* bot, Unit* target, float optDistance, float& outX, float& outY, float& outZ)
    {
        static std::vector<DangerZone> const emptyDangerZones;
        return CalculateNextPosition(bot, target, optDistance, false, false, emptyDangerZones, outX, outY, outZ);
    }
};
