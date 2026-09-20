/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "Analytics/CombatAnalyzer.h"
#include <algorithm>

AttributionReport CombatAnalyzer::Analyze(
    BotCombatRingBuffer<256> const& ringBuffer,
    uint32 combatDurationMs,
    bool victory,
    uint32 bossEntry)
{
    AttributionReport report;
    report.bossEntry = bossEntry;
    report.totalCombatTimeMs = combatDurationMs;
    report.isWipe = !victory;

    if (ringBuffer.Empty())
        return report;

    uint32 maxDamageInRecent = 0;
    uint32 maxDamageSpell = 0;

    uint32 tankThreatSum = 0;
    uint32 tankSampleCount = 0;

    // 1. 逆序扫描：捕获致死伤害与尖刺爆发
    bool foundFatal = false;
    ringBuffer.ForEachReverse([&](BotCombatEvent const& ev) -> bool
    {
        if (!foundFatal && ev.eventType == BotCombatEventType::LETHAL_DAMAGE)
        {
            report.fatalSpellId = ev.spellId;
            report.fatalDamage = ev.amount;
            report.fatalSourceGuid = ev.sourceGuid;
            foundFatal = true;

            // 将致死落点逆向提炼为临时避险禁区 (半径 6 码)
            if (ev.x != 0.0f || ev.y != 0.0f)
            {
                DangerZone zone;
                zone.type = DangerZoneType::CIRCLE;
                zone.x = ev.x;
                zone.y = ev.y;
                zone.z = ev.z;
                zone.radius = 6.0f;
                zone.spellId = ev.spellId;
                report.derivedDangerZones.push_back(zone);
            }
        }

        if (ev.eventType == BotCombatEventType::DAMAGE_TAKEN || ev.eventType == BotCombatEventType::LETHAL_DAMAGE)
        {
            if (ev.amount > maxDamageInRecent)
            {
                maxDamageInRecent = ev.amount;
                maxDamageSpell = ev.spellId;
            }
        }
        return true;
    });

    report.peakDamage = maxDamageInRecent;
    report.peakDamageSpellId = maxDamageSpell;

    // 2. 正序扫描：漏打断、起手 OT 与主坦 TPS
    ringBuffer.ForEach([&](BotCombatEvent const& ev)
    {
        if (ev.eventType == BotCombatEventType::SPELL_HIT_TAKEN)
        {
            ++report.missedInterruptsCount;
            report.lastMissedSpellId = ev.spellId;
        }

        if (ev.eventType == BotCombatEventType::THREAT_OT_WARNING && ev.combatTimeMs <= 10000)
        {
            if (!report.earlyOtDetected)
            {
                report.earlyOtDetected = true;
                report.otTimeMs = ev.combatTimeMs;
            }
        }

        if (ev.eventType == BotCombatEventType::TANK_THREAT_SAMPLE && ev.combatTimeMs <= 10000)
        {
            tankThreatSum += ev.amount;
            ++tankSampleCount;
        }
    });

    float const seconds = std::min(combatDurationMs, 10000u) / 1000.0f;
    report.tankFirst10sTps = (seconds > 0.0f) ? (static_cast<float>(tankThreatSum) / seconds) : 0.0f;

    return report;
}
