/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

// =============================================================================
// 奥术法师 (Arcane Mage) 3.3.5a 法术常量表
// -----------------------------------------------------------------------------
// 全部 ID 取 WotLK 最高等级 Rank。
// 基础法术的低等级由 AdaptiveBotAI::GetAppropriateRank 沿 GetPrevRankSpell
// 链自动降阶；天赋法术 DBC SpellLevel 恒为 0，必须由专精按
// GetTalentSpellMinLevel 显式门禁。本文件严禁承载任何逻辑。
// =============================================================================
namespace ArcaneMageSpells
{
    // -------------------------------------------------------------------------
    // 核心奥术伤害法术
    // -------------------------------------------------------------------------
    constexpr uint32 ARCANE_BLAST        = 42897;  // 奥术冲击 Rank 4 (64 级核心叠层技，读条，叠加奥术冲击光环)
    constexpr uint32 ARCANE_MISSILES     = 42846;  // 奥术飞弹 Rank 13 (基础引导技，消耗飞弹速射免费快速打出)
    constexpr uint32 ARCANE_BARRAGE      = 44781;  // 奥术弹幕 Rank 3 (60 级天赋，瞬发无消耗消层/移动填充，3s CD)
    constexpr uint32 FROSTBOLT           = 42842;  // 寒冰箭 Rank 16 (64 级前未习得奥术冲击时的低等级读条填充)

    // -------------------------------------------------------------------------
    // 爆发、续航与自保
    // -------------------------------------------------------------------------
    constexpr uint32 ARCANE_POWER        = 12042;  // 奥术强化 (40 级天赋，伤害 +20%，耗蓝 +20%，15s，2m CD，Off-GCD)
    constexpr uint32 PRESENCE_OF_MIND    = 12043;  // 气定神闲 (30 级天赋，下次法术瞬发，2m CD，Off-GCD)
    constexpr uint32 EVOCATION           = 12051;  // 唤醒 (引导回蓝，8 秒恢复 60% 法力，4m CD)
    constexpr uint32 MANA_GEM_EFFECT     = 42987;  // 法力宝石恢复法力法术 (恢复 3330~3500 法力，2m 自管 CD)
    constexpr uint32 MAGE_ARMOR          = 43024;  // 法师护甲 Rank 6 (常驻护甲，施法保持 50% 精神回蓝)
    constexpr uint32 MIRROR_IMAGE        = 55342;  // 镜像 (80 级爆发起手，召唤 3 个分身并降低仇恨，3m CD)
    constexpr uint32 INVISIBILITY        = 66;     // 隐形术 (渐隐脱困/清仇恨，3m CD)
    constexpr uint32 ICE_BLOCK           = 45438;  // 寒冰屏障 (30 级保命冰箱，5m CD)
    constexpr uint32 MANA_SHIELD         = 43020;  // 法力护盾 Rank 7 (吸收伤害，应急自保)

    // -------------------------------------------------------------------------
    // 打断与控制
    // -------------------------------------------------------------------------
    constexpr uint32 COUNTERSPELL        = 2139;   // 法术反制 (打断读条，24s CD，Off-GCD)
    constexpr uint32 SLOW                = 31589;  // 减速 (50 级天赋，瞬发目标攻速 -30%、移速 -60%、施法 +30%)

    // -------------------------------------------------------------------------
    // 核心光环与被动补偿 (以光环形式注入，弥补 NPC 无天赋树缺陷)
    // -------------------------------------------------------------------------
    constexpr uint32 AURA_ARCANE_BLAST    = 36032;  // 奥术冲击自身叠层光环 (1~4 层，每层提升伤害与耗蓝)
    constexpr uint32 AURA_MISSILE_BARRAGE = 44401;  // 飞弹速射触发光环 (奥术飞弹引导减半且零耗蓝)
    constexpr uint32 ARCANE_MEDITATION    = 18464;  // 奥术冥想 Rank 3 (施法中保持 50% 回蓝)
    constexpr uint32 TORMENT_THE_WEAK     = 55340;  // 欺凌弱小 Rank 3 (对被减速目标伤害 +12%)
    constexpr uint32 SPELL_POWER          = 35589;  // 法术能量 Rank 2 (法术暴击伤害加成 +50%)
    constexpr uint32 ARCANE_EMPOWERMENT   = 31583;  // 奥术增效 Rank 3 (奥术冲击伤害 +9%，团队伤害 +3%)
    constexpr uint32 GLYPH_OF_ARCANE_BLAST    = 56366; // 奥术冲击雕文 (奥术冲击增伤效果提升)
    constexpr uint32 GLYPH_OF_ARCANE_MISSILES = 56364; // 奥术飞弹雕文 (奥术飞弹暴击伤害提升)
}
