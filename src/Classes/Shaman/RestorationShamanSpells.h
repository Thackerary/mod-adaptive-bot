/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

// 恢复萨满专精法术常量表 (3.3.5a 最高等级 ID)
namespace RestorationShamanSpells
{
    // ---------------- 核心治疗与护盾 ----------------
    constexpr uint32 RIPTIDE             = 61301; // 激流 Rank 4 (60 级终极天赋, 瞬发 HoT + 直接治疗, 6s CD)
    constexpr uint32 CHAIN_HEAL          = 55459; // 治疗链 Rank 7 (主力智能团刷, 2.5s 读条)
    constexpr uint32 LESSER_HEALING_WAVE = 49276; // 次级治疗波 Rank 9 (快速单体填充, 1.5s 读条)
    constexpr uint32 HEALING_WAVE        = 49273; // 治疗波 Rank 14 (高额大读条, 3.0s 读条)
    constexpr uint32 EARTH_SHIELD        = 49284; // 大地之盾 Rank 5 (50 级天赋, 绑定主坦, 9 层受击回血)
    constexpr uint32 WATER_SHIELD        = 52128; // 水之护盾 Rank 9 (自身常驻回蓝)

    // ---------------- 战略爆发与续航 ----------------
    constexpr uint32 NATURES_SWIFTNESS   = 16188; // 自然迅捷 (30 级天赋, Off-GCD, 使下个自然法术瞬发, 2m CD)
    constexpr uint32 TIDAL_FORCE         = 55198; // 潮汐之力 (50 级天赋, 下 3 次治疗暴击率 +60%, 3m CD)
    constexpr uint32 MANA_TIDE_TOTEM     = 16190; // 法力之潮图腾 (40 级天赋, 5m CD)

    // ---------------- 驱散体系 ----------------
    constexpr uint32 CLEANSE_SPIRIT      = 51886; // 净化灵魂 (40 级天赋, 解诅咒/中毒/疾病, 无法解魔法)
    constexpr uint32 CURE_TOXINS         = 526;   // 消毒术 (16 级基础法术, 仅解中毒/疾病, 无法解诅咒)

    // ---------------- 打断体系 ----------------
    // 风剪为唯一单 Rank 基础法术 (16 级可学, 无天赋门槛), 严禁登记进 GetTalentSpellMinLevel。
    // 等级门禁由 DBC SpellLevel 经 GetAppropriateRank(..., false) 自动降阶/拒绝处理。
    // 数值必须与 EnhancementShamanSpells::WIND_SHEAR 保持一致 (同职业共享法术 ID)。
    constexpr uint32 WIND_SHEAR          = 57994; // 风剪 (近战打断 + 减仇恨, 6s CD)

    // ---------------- 图腾矩阵 (四大元素) ----------------
    constexpr uint32 STRENGTH_OF_EARTH_TOTEM = 58643; // 大地之力图腾 (土)
    constexpr uint32 FLAMETONGUE_TOTEM       = 58656; // 火舌图腾 (火)
    constexpr uint32 MANA_SPRING_TOTEM       = 58774; // 法力之泉图腾 (水)
    constexpr uint32 WRATH_OF_AIR_TOTEM      = 3738;  // 空气之怒图腾 (风)

    // ---------------- 被动天赋补偿与雕文 ----------------
    constexpr uint32 TIDAL_WAVES_TALENT    = 51566; // 潮汐奔涌【天赋】(50 级)
    constexpr uint32 TIDAL_WAVES_PROC      = 53390; // 潮汐奔涌【Proc Buff】：激流/链子后叠 2 层
    constexpr uint32 ANCESTRAL_AWAKENING   = 51558; // 先祖复苏 Rank 3 (40 级, 暴击治疗瞬发折射)
    constexpr uint32 PURIFICATION          = 16213; // 净化 Rank 5 (治疗效果 +10%)
    constexpr uint32 HEALING_WAY           = 29206; // 治疗之道 Rank 3 (治疗波增效)
    constexpr uint32 MANA_SPRING           = 16206; // 强化法力之泉 Rank 2
    constexpr uint32 GLYPH_OF_CHAIN_HEAL   = 55437; // 治疗链雕文 (跳跃目标 +1, 共 4 目标)
    constexpr uint32 GLYPH_OF_EARTH_SHIELD = 63279; // 大地之盾雕文 (大地之盾治疗量 +20%)
}
