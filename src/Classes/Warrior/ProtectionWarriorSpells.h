/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

namespace ProtectionWarriorSpells
{
    // =========================================================================
    // 1. 姿态法术 (Stances)
    // =========================================================================
    constexpr uint32 DEFENSIVE_STANCE           = 71;    // 防御姿态 (核心常驻姿态)
    constexpr uint32 BATTLE_STANCE              = 2457;  // 战斗姿态
    constexpr uint32 BERSERKER_STANCE           = 2458;  // 狂暴姿态

    // =========================================================================
    // 2. 团队怒吼与自身常驻光环 (Shouts & Buffs)
    // =========================================================================
    constexpr uint32 COMMANDING_SHOUT           = 47440; // 命令怒吼 (Rank 3, 68级解锁)
    constexpr uint32 BATTLE_SHOUT               = 47436; // 战斗怒吼 (Rank 9, 1~67级主力团队Buff)
    constexpr uint32 DEMORALIZING_SHOUT         = 47437; // 挫志怒吼 (Rank 8)

    // =========================================================================
    // 3. 高机动突进、战术机动与打断 (Mobility, Interrupt & CC)
    // =========================================================================
    constexpr uint32 CHARGE                     = 11578; // 冲锋 (Rank 3)
    constexpr uint32 INTERCEPT                  = 20252; // 拦截 (Rank 5)
    constexpr uint32 INTERVENE                  = 3411;  // 援护 (3411 为战士正确援护)
    constexpr uint32 SHIELD_BASH                = 72;    // 盾击 (Rank 4 打断)
    constexpr uint32 CONCUSSION_BLOW            = 12809; // 震荡猛击 (5秒物理昏迷)
    constexpr uint32 SHOCKWAVE                  = 46968; // 震荡波 (正面锥形昏迷)

    // =========================================================================
    // 4. 生存硬减伤与破釜急救 (Hard Defensives & Power)
    // =========================================================================
    constexpr uint32 BLOODRAGE                  = 2687;  // 血性狂暴 (起手产怒/激怒效果)
    constexpr uint32 BERSERKER_RAGE             = 18499; // 狂暴之怒 (解恐/激怒触发前置)
    constexpr uint32 SHIELD_BLOCK               = 2565;  // 盾牌格挡 (格挡值/率翻倍)
    constexpr uint32 SHIELD_WALL                = 871;   // 盾墙 (60% 绝对物理/法术减伤)
    constexpr uint32 LAST_STAND                 = 12975; // 破釜沉舟 (生命上限提高 30%)
    constexpr uint32 ENRAGED_REGENERATION       = 55694; // 狂暴回复 (百分比自疗)

    // =========================================================================
    // 5. 核心单体与群体仇恨打击 (Core Attacks)
    // =========================================================================
    constexpr uint32 SHIELD_SLAM                = 47488; // 盾牌猛击 (Rank 8)
    constexpr uint32 REVENGE                    = 57823; // 复仇 (Rank 9)
    constexpr uint32 DEVASTATE                  = 47498; // 毁灭打击 (Rank 5)
    constexpr uint32 SUNDER_ARMOR               = 7386;  // 破甲攻击 (Rank 5 基础元数据)
    constexpr uint32 THUNDER_CLAP               = 47502; // 雷霆一击 (Rank 9)
    constexpr uint32 HEROIC_STRIKE              = 47450; // 英勇打击 (Rank 13)
    constexpr uint32 CLEAVE                     = 47520; // 顺劈斩 (Rank 8)

    // =========================================================================
    // 6. 强力仇恨控制 (Threat Control)
    // =========================================================================
    constexpr uint32 TAUNT                      = 355;   // 嘲讽 (30码单体强嘲)
    constexpr uint32 CHALLENGING_SHOUT          = 1161;  // 挑战怒吼 (紧急群嘲大招)

    // =========================================================================
    // 7. 防护战士常驻被动天赋光环补偿 (弥补 NPC 缺天赋树缺陷)
    // =========================================================================
    constexpr uint32 AURA_WARBRINGER            = 57499; // 战神 (解除定身，全姿态可在战斗中冲锋/拦截/援护)
    constexpr uint32 AURA_DAMAGE_SHIELD         = 58874; // 伤害盾 (格挡反弹 30% 格挡值仇恨)
    constexpr uint32 AURA_CRITICAL_BLOCK        = 47296; // 致命格挡 (格挡率提升，概率双倍格挡)
    constexpr uint32 AURA_VITALITY              = 29144; // 活力 (力/耐+6%, 精准+6)
    constexpr uint32 AURA_TOUGHNESS             = 12764; // 坚韧 (护甲提升 10%)
    constexpr uint32 AURA_ONE_HANDED_SPEC       = 16803; // 单手武器专精 (伤害提升 10%)

    // =========================================================================
    // 8. 团队同类 Debuff 互斥排查掩码
    // =========================================================================
    constexpr uint32 AURA_EXPOSE_ARMOR          = 8647;  // 潜行者破甲
    constexpr uint32 AURA_ACID_SPIT             = 55754; // 猎人酸液喷吐
    constexpr uint32 AURA_DEMO_ROAR             = 48560; // 德鲁伊挫志咆哮
}