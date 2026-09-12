/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

namespace BearDruidSpells
{
    // =========================================================================
    // 1. 形态与常驻增益 (Forms & Self Buffs)
    // =========================================================================
    constexpr uint32 BEAR_FORM                       = 5487;  // 熊形态 (10 级解锁)
    constexpr uint32 DIRE_BEAR_FORM                  = 9634;  // 巨熊形态 (40 级解锁核心形态)
    constexpr uint8  BEAR_FORM_MIN_LEVEL             = 10;
    constexpr uint8  DIRE_BEAR_FORM_MIN_LEVEL        = 40;

    constexpr uint32 GIFT_OF_THE_WILD                = 48470; // 野性赐福 (Rank 3)
    constexpr uint32 ENRAGE                          = 5229;  // 激怒 (战时/起手生成怒气)
    constexpr uint8  ENRAGE_MIN_LEVEL                = 22;

    // =========================================================================
    // 2. 仇恨、打击与聚怪 (Threat, Strikes & Gathering)
    // =========================================================================
    constexpr uint32 FAERIE_FIRE_FERAL               = 27011; // 精灵之火 (野性, Rank 5 顶阶, 30 码破甲开怪)
    constexpr uint8  FAERIE_FIRE_MIN_LEVEL           = 18;
    constexpr uint32 FERAL_CHARGE_BEAR               = 16979; // 野性冲锋 - 巨熊 (8~25 码突进定身/断法)
    constexpr uint8  FERAL_CHARGE_MIN_LEVEL          = 25;
    constexpr uint32 GROWL                           = 6795;  // 低吼 (单体嘲讽)
    constexpr uint8  GROWL_MIN_LEVEL                 = 10;
    constexpr uint32 CHALLENGING_ROAR                = 5209;  // 挑战咆哮 (AoE 群体嘲讽紧急救急)
    constexpr uint8  CHALLENGING_ROAR_MIN_LEVEL      = 28;
    constexpr uint32 MANGLE_BEAR                     = 48564; // 裂伤 - 熊 (Rank 5, 6 秒 CD 单体核心仇恨)
    constexpr uint8  MANGLE_MIN_LEVEL                = 50;
    constexpr uint32 LACERATE                        = 48568; // 割伤 (Rank 3, 叠 5 层流血 DoT)
    constexpr uint8  LACERATE_MIN_LEVEL              = 40;
    constexpr uint32 SWIPE_BEAR                      = 48432; // 横扫 - 熊 (Rank 8, 核心近战 AoE 群拉)
    constexpr uint8  SWIPE_MIN_LEVEL                 = 16;
    constexpr uint32 MAUL                            = 48480; // 重殴 (Rank 10, 下一次平砍强化)
    constexpr uint8  MAUL_MIN_LEVEL                  = 10;

    // =========================================================================
    // 3. 生存与减伤 (Survivability & Cooldowns)
    // =========================================================================
    constexpr uint32 BARKSKIN                        = 22812; // 树皮术 (20% 减伤, 1 分钟 CD)
    constexpr uint8  BARKSKIN_MIN_LEVEL              = 44;
    constexpr uint32 SURVIVAL_INSTINCTS              = 61336; // 生存本能 (+30% 最大生命值)
    constexpr uint8  SURVIVAL_INSTINCTS_MIN_LEVEL    = 30;
    constexpr uint32 FRENZIED_REGENERATION           = 22842; // 狂暴回复 (怒气转生命持续自疗)
    constexpr uint8  FRENZIED_REGENERATION_MIN_LEVEL = 36;

    // =========================================================================
    // 4. 天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
    // =========================================================================
    constexpr uint32 SURVIVAL_OF_THE_FITTEST         = 33856; // 适者生存 Rank 3 (-6% 被暴击, 全属性 +6%)
    constexpr uint32 NATURAL_REACTION                = 49673; // 自然反应 Rank 3 (+6% 闪避, 闪避回怒)
    constexpr uint32 THICK_HIDE                      = 16931; // 厚皮 Rank 3 (+10% 护甲)
    constexpr uint32 PROTECTOR_OF_THE_PACK           = 57881; // 兽群守护者 Rank 3 (减伤 12%, AP +6%)
    constexpr uint8  PASSIVE_TALENT_MIN_LEVEL        = 40;    // 被动光环注入等级门槛
}
