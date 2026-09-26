/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "Analytics/CombatAnalyzer.h"
#include <algorithm>

AttributionReport CombatAnalyzer::Analyze(
    BotCombatRingBuffer<256> const& ringBuffer,
    uint32 combatDurationMs,
    bool victory,
    uint32 bossEntry,
	float earlyMaxTankThreat,
    bool earlyOtDetected,
    uint32 earlyOtTimeMs,
	std::unordered_map<uint32, uint32> const& streamMissedSpells)
{
    AttributionReport report;
    report.bossEntry = bossEntry;
    report.totalCombatTimeMs = combatDurationMs;
    report.isWipe = !victory;
	report.earlyOtDetected = earlyOtDetected;
    report.otTimeMs = earlyOtTimeMs;
	// 直接采纳流式累加的漏打断字典（整场战斗无遗漏）
    report.missedSpellCounts = streamMissedSpells;
	
	float const seconds = std::min(combatDurationMs, 10000u) / 1000.0f;
    report.tankFirst10sTps = (seconds > 0.0f) ? (earlyMaxTankThreat / seconds) : 0.0f;
	
    if (ringBuffer.Empty())
        return report;

    uint32 maxDamageInRecent = 0;
    uint32 maxDamageSpell = 0;

    // 1. 逆序扫描：仅捕获致死伤害、尖刺爆发与地面避险禁区
    bool foundFatal = false;
    ringBuffer.ForEachReverse([&](BotCombatEvent const& ev) -> bool
    {
        if (!foundFatal && ev.eventType == BotCombatEventType::LETHAL_DAMAGE)
        {
            report.fatalSpellId = ev.spellId;
            report.fatalDamage = ev.amount;
            report.fatalSourceGuid = ev.sourceGuid;
            foundFatal = true;

            // 仅当致死来源为具体法术技能（非平砍白字 SpellID == 0）时，
            // 才把落点逆向提炼为地面避险禁区；否则随从被普攻打死会在原地
            // 画出一个 6 码假火圈，导致后续战斗出现无意义的绕行抖动。
            if (ev.spellId != 0 && (ev.x != 0.0f || ev.y != 0.0f))
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

    return report;
}
