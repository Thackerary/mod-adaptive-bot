/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

namespace FeralCatDruidSpells
{
    // =========================================================================
    // 形态、爆发、终结技与核心打击
    // =========================================================================
    constexpr uint32 CAT_FORM                     = 768;    // 猎豹形态 (核心战斗形态)
    constexpr uint32 SAVAGE_ROAR                  = 52610;  // 野性咆哮 (基础终结技, 物理伤害 +30% / 雕文 33%, 必须全覆盖)
    constexpr uint32 RIP                          = 49800;  // 割裂 Rank 9 (5 星主力物理流血终结技)
    constexpr uint32 FEROCIOUS_BITE               = 48577;  // 凶猛撕咬 Rank 8 (5 星直接物理爆发泄能终结技)
    constexpr uint32 RAKE                         = 48574;  // 斜掠 Rank 7 (核心产星流血 DoT)
    constexpr uint32 SHRED                        = 48572;  // 撕碎 Rank 9 (核心背后高伤害产星重击, 需背后位)
    constexpr uint32 MANGLE_CAT                   = 48566;  // 裂伤-猎豹 Rank 5 (纯天赋, 50 级解锁, +30% 流血易伤 / 正面填充)
    constexpr uint32 TIGERS_FURY                  = 50213;  // 猛虎之怒 Rank 6 (Off-GCD, 30s CD, 回复 60 能量并增伤)
    constexpr uint32 BERSERK                      = 50334;  // 狂暴 (纯天赋, 60 级解锁, 3m CD, 猎豹技能能量消耗减少 50%)
    constexpr uint32 FAERIE_FIRE_FERAL            = 16857;  // 精灵之火 (野性) (纯天赋, 30 级解锁, 破甲与开怪)
    constexpr uint32 SURVIVAL_INSTINCTS           = 61336;  // 生存本能 (纯天赋, 20 级解锁, 3m CD, 提高 30% 生命上限)
    constexpr uint32 BARKSKIN                     = 22812;  // 树皮术 (1m CD, 20% 硬减伤, 允许形态内施放)

    // =========================================================================
    // 触发光环、Debuff 与被动天赋补偿
    // =========================================================================
    constexpr uint32 AURA_CAT_FORM                = 768;    // 猎豹形态光环
    constexpr uint32 AURA_SAVAGE_ROAR             = 52610;  // 野性咆哮增益光环
    constexpr uint32 AURA_CLEARCASTING            = 16870;  // 节能施法光环 (清晰预兆触发, 技能 0 消耗)
    constexpr uint32 AURA_TIGERS_FURY             = 50213;  // 猛虎之怒光环
    constexpr uint32 AURA_BERSERK                 = 50334;  // 狂暴光环
    constexpr uint32 AURA_MANGLE                  = 33876;  // 裂伤流血易伤 Debuff (+30% 流血伤害)
    constexpr uint32 AURA_TRAUMA                  = 46857;  // 武器战创伤 Debuff (同等流血易伤)
    constexpr uint32 AURA_RIP                     = 49800;  // 割裂 Debuff
    constexpr uint32 AURA_RAKE                    = 48574;  // 斜掠 Debuff
    constexpr uint32 AURA_FAERIE_FIRE             = 770;    // 精灵之火破甲 Debuff
    constexpr uint32 OMEN_OF_CLARITY              = 16864;  // 清晰预兆天赋根源 (近战平砍几率触发节能施法)
    constexpr uint32 KING_OF_THE_JUNGLE           = 48494;  // 丛林之王 Rank 3 (猛虎之怒立即回复 60 能量)
    constexpr uint32 REND_AND_TEAR                = 48434;  // 撕扯 Rank 5 (流血目标撕碎 +20%, 凶猛撕咬暴击 +25%)
    constexpr uint32 PRIMAL_GORE                  = 63504;  // 原始血腥 (割裂每跳均可暴击)
    constexpr uint32 FERAL_AGGRESSION             = 16862;  // 野性侵略 Rank 5 (凶猛撕咬伤害 +15%)
    constexpr uint32 PREDATORY_STRIKES            = 16975;  // 猛兽攻击 Rank 3 (近战攻击强度加成)
    constexpr uint32 SHREDDING_ATTACKS            = 16972;  // 撕碎攻击 Rank 2 (撕碎能量消耗 -18)
    constexpr uint32 FEROCITY                     = 16938;  // 凶暴 Rank 5 (裂伤 / 斜掠 / 撕咬能量消耗 -5)
    constexpr uint32 HEART_OF_THE_WILD            = 24894;  // 野性之心 Rank 5 (力量 / 智力属性加成)
    constexpr uint32 SURVIVAL_OF_THE_FITTEST      = 33856;  // 适者生存 Rank 3 (属性提高 6%, 免受暴击)
    constexpr uint32 NATURAL_REACTION             = 48485;  // 自然反应 Rank 2 (躲闪提高 6%)
    constexpr uint32 GLYPH_OF_SHRED               = 54815;  // 撕碎雕文 (每次撕碎延长割裂持续时间 2s, 最多 6s)
    constexpr uint32 GLYPH_OF_RIP                 = 54818;  // 割裂雕文 (割裂持续时间延长 4s)
    constexpr uint32 GLYPH_OF_SAVAGE_ROAR         = 54822;  // 野性咆哮雕文 (野性咆哮额外提高 3% 伤害)

    // =========================================================================
    // 纯天赋最低解锁等级契约
    // 3.3.5a 中天赋法术 DBC SpellLevel 恒为 0，GetAppropriateRank 永不降阶，
    // 必须由专精显式登记最低等级，否则低等级随从会误判为「已习得」而永久空转。
    // =========================================================================
    constexpr uint8 SURVIVAL_INSTINCTS_MIN_LEVEL  = 20;
    constexpr uint8 FAERIE_FIRE_MIN_LEVEL         = 30;
    constexpr uint8 MANGLE_MIN_LEVEL              = 50;
    constexpr uint8 BERSERK_MIN_LEVEL             = 60;

    // 被动天赋光环注入的最低等级 (随从无天赋树，达到该等级即注入满阶根源)
    constexpr uint8 PASSIVE_TALENT_MIN_LEVEL      = 10;
}
