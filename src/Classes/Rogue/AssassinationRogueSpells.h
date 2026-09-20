/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

// =============================================================================
// 刺杀潜行者 (Assassination Rogue) 3.3.5a 法术常量表
// -----------------------------------------------------------------------------
// 全部 ID 取 WotLK 最高等级 Rank；低等级由 AdaptiveBotAI::GetAppropriateRank
// 沿 GetPrevRankSpell 链自动降阶，本文件严禁承载任何逻辑。
// =============================================================================
namespace AssassinationRogueSpells
{
    // -------------------------------------------------------------------------
    // 核心近战输出与终结技
    // -------------------------------------------------------------------------
    constexpr uint32 MUTILATE            = 48666;  // 毁伤 Rank 6 (40 级天赋，双持匕首主力产星，+2 星)
    constexpr uint32 ENVENOM             = 57993;  // 毒伤 Rank 9 (62 级核心终结技，需致命毒药，刷新切割)
    constexpr uint32 SLICE_AND_DICE      = 6774;   // 切割 Rank 2 (攻速 +40%，起手/断档补挂)
    constexpr uint32 HUNGER_FOR_BLOOD    = 53819;  // 血之饥渴 (50 级终极天赋，需目标流血，伤害 +5%，持续 1 分钟)
    constexpr uint32 RUPTURE             = 48672;  // 割裂 Rank 9 (终结流血技，为血之饥渴铺垫)
    constexpr uint32 GARROTE             = 48676;  // 绞喉 Rank 10 (潜行背后起手流血技，+1 星)
    constexpr uint32 SINISTER_STRIKE     = 48638;  // 邪恶攻击 Rank 12 (40 级前未习得毁伤时的产星填充)
    constexpr uint32 EVISCERATE          = 48668;  // 剔骨 Rank 12 (62 级前未习得毒伤时的物理终结技)

    // -------------------------------------------------------------------------
    // 潜行、爆发与控场
    // -------------------------------------------------------------------------
    constexpr uint32 STEALTH             = 1784;   // 潜行 Rank 4 (脱战常驻)
    constexpr uint32 VANISH              = 26889;  // 消失 Rank 3 (强行进潜，清仇恨并触发灭绝回能，3 分钟 CD)
    constexpr uint32 COLD_BLOOD          = 14177;  // 冷血 (20 级天赋，下次攻击必暴，Off-GCD，2 分钟 CD)
    constexpr uint32 KICK                = 38768;  // 脚踢 Rank 5 (Off-GCD 打断, 接入基类压秒仲裁)
    constexpr uint32 TRICKS_OF_THE_TRADE = 57934;  // 嫁祸诀窍 (75 级仇恨转移与 15% 增伤，30s CD)

    // -------------------------------------------------------------------------
    // 减伤自保与佯攻
    // -------------------------------------------------------------------------
    constexpr uint32 FEINT               = 48659;  // 佯攻 Rank 8 (降低范围承伤 50% 并降低仇恨，10s CD)
    constexpr uint32 EVASION             = 26669;  // 闪避 Rank 2 (闪避率 +50%，3 分钟 CD)
    constexpr uint32 CLOAK_OF_SHADOWS    = 31224;  // 暗影斗篷 (免疫魔法并清除有害魔法/疾病，90s CD)

    // -------------------------------------------------------------------------
    // 毒药与被动补偿光环 (以光环形式注入，弥补 NPC 无天赋树缺陷)
    // -------------------------------------------------------------------------
    constexpr uint32 OVERKILL            = 58426;  // 灭绝 (30 级天赋，潜行中及破潜后 20 秒回能速度 +30%)
    constexpr uint32 CUT_TO_THE_CHASE    = 51667;  // 名天堑 Rank 5 (50 级天赋，毒伤 100% 将切割刷新至 5 星时长)
    constexpr uint32 MASTER_POISONER     = 58410;  // 毒药大师 Rank 3 (中毒目标受暴击率 +3%)
    constexpr uint32 GLYPH_OF_MUTILATE   = 56807;  // 毁伤雕文 (毁伤能量消耗 -5 点)
    constexpr uint32 GLYPH_OF_HUNGER_FOR_BLOOD = 63249; // 血之饥渴雕文 (增伤提升至 8%)
    constexpr uint32 GLYPH_OF_TRICKS_OF_TRADE  = 63420; // 嫁祸诀窍雕文 (嫁祸增益持续时间延长)

    // -------------------------------------------------------------------------
    // 武器毒药 (随从无武器涂毒对象，由 ProcPoisons 以 triggered 方式模拟注入)
    // -------------------------------------------------------------------------
    constexpr uint32 DEADLY_POISON       = 57970;  // 致命毒药 Rank 9 (DoT 光环，可叠 5 层，毒伤硬性前置)
    constexpr uint32 INSTANT_POISON      = 57968;  // 速效毒药 Rank 9 (直接自然伤害)
}
