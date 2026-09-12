/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

namespace ProtectionPaladinSpells
{
    // =========================================================================
    // 1. 核心姿态与自身常驻增益 (Stances & Self Buffs)
    // =========================================================================
    constexpr uint32 RIGHTEOUS_FURY                 = 25780; // 正义之怒 (核心常驻仇恨增益)
    constexpr uint32 BLESSING_OF_SANCTUARY          = 20911; // 庇护祝福 (单体)
    constexpr uint32 GREATER_BLESSING_OF_SANCTUARY  = 25899; // 强效庇护祝福 (团队)

    // =========================================================================
    // 2. 圣印 (Judgement 施放前置)
    // =========================================================================
    constexpr uint32 SEAL_OF_VENGEANCE              = 31801; // 复仇圣印 (联盟向)
    constexpr uint32 SEAL_OF_CORRUPTION             = 53736; // 腐化圣印 (部落向)

    // =========================================================================
    // 3. 核心仇恨打击 (Core Threat Abilities)
    // =========================================================================
    constexpr uint32 HOLY_SHIELD                    = 48952; // 神圣之盾 (Rank 6)
    constexpr uint32 CONSECRATION                   = 48819; // 奉献 (Rank 8)
    constexpr uint32 HAMMER_OF_THE_RIGHTEOUS        = 53595; // 正义之锤 (Rank 5)
    constexpr uint32 SHIELD_OF_RIGHTEOUSNESS        = 61411; // 正义之盾 (Rank 2)
    constexpr uint32 JUDGEMENT_OF_LIGHT             = 20271; // 圣光审判

    // =========================================================================
    // 4. 仇恨控制与救急 (Threat Control & Emergency)
    // =========================================================================
    constexpr uint32 HAND_OF_RECKONING              = 62124; // 清算之手 (单体嘲讽)
    constexpr uint32 RIGHTEOUS_DEFENSE              = 31789; // 正义防御 (对友方施放的多目标嘲讽)

    // =========================================================================
    // 5. 生存与减伤 (Survivability & Cooldowns)
    // =========================================================================
    constexpr uint32 DIVINE_SACRIFICE               = 64205; // 神圣牺牲 (团队转伤减伤)
    constexpr uint32 DIVINE_PROTECTION              = 498;   // 圣佑术 (50% 减伤)
    constexpr uint32 DIVINE_SHIELD                  = 642;   // 圣盾术 (绝对免疫)
    constexpr uint32 FORBEARANCE                    = 25771; // 自律 (封印圣佑/圣盾)

    // =========================================================================
    // 6. 防护天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
    // =========================================================================
    constexpr uint32 AURA_DIVINE_STRENGTH           = 20262; // 神圣之力 (力量提升)
    constexpr uint32 AURA_TOUGHNESS                 = 20143; // 坚韧 (护甲提升)
    constexpr uint32 AURA_ANTICIPATION              = 20102; // 预知 (闪避提升)
    constexpr uint32 AURA_REDOUBT                   = 20127; // 盾牌壁垒 (格挡提升)
}
