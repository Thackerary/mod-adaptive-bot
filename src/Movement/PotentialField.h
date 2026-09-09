#pragma once

#include "Creature.h"
#include "Unit.h"
#include "Map.h"
#include <cmath>
#include <list>

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
    static bool CalculateNextPosition(Creature* bot, Unit* target, float optDistance, float& outX, float& outY, float& outZ)
    {
        if (!bot || !target || !bot->IsAlive())
            return false;

        if (bot->HasUnitState(UNIT_STATE_CASTING | UNIT_STATE_CHANNELING))
            return false;

        FieldVector2D totalForce{ 0.0f, 0.0f };

        float const distToTarget = bot->GetDistance2d(target);
        FieldVector2D const dirToTarget = {
            target->GetPositionX() - bot->GetPositionX(),
            target->GetPositionY() - bot->GetPositionY()
        };

        float const deadZone = 2.5f;

        if (distToTarget > optDistance + deadZone)
        {
            totalForce = totalForce + dirToTarget.Normalized() * 1.0f;
        }
        else if (distToTarget < optDistance - deadZone)
        {
            float const pushStrength = 1.5f * (1.0f - (distToTarget / optDistance));
            totalForce = totalForce - dirToTarget.Normalized() * pushStrength;
        }

        // 友军防挤压斥力（支持跨职业 Entry）
        float const personalSpace = 4.0f;
        std::list<Creature*> nearbyCreatures;
        bot->GetCreaturesWithEntryInRange(nearbyCreatures, personalSpace, 0); // 0 = 检索范围内所有生物

        for (Creature* ally : nearbyCreatures)
        {
            // 排除自身、死者、非友方，以及非战斗状态的背景 NPC（防止被城镇卫兵推搡）
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
            totalForce = totalForce + pushDir.Normalized() * (factor * 0.8f);
        }

        float const forceMagnitude = totalForce.Length();
        if (forceMagnitude < 0.2f)
            return false;

        FieldVector2D const moveDir = totalForce.Normalized();
        float const stepLength = 3.5f;

        outX = bot->GetPositionX() + moveDir.x * stepLength;
        outY = bot->GetPositionY() + moveDir.y * stepLength;
        outZ = bot->GetMap()->GetHeight(bot->GetPhaseMask(), outX, outY, bot->GetPositionZ());

        if (std::abs(outZ - bot->GetPositionZ()) > 3.0f)
        {
            outZ = bot->GetPositionZ();
        }

        return true;
    }
};