/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef _BOT_BEAST_MASTERY_HUNTER_SPELLS_H
#define _BOT_BEAST_MASTERY_HUNTER_SPELLS_H

#include "Define.h"

// =============================================================================
// 野兽控制猎人 (兽王猎) 3.3.5a 法术常量表
// -----------------------------------------------------------------------------
// 约定:
//   * 所有 ID 均取 3.3.5a (WotLK) 满阶 Rank, 低等级由 GetAppropriateRank 自动降阶;
//   * 纯天赋技能 (BESTIAL_WRATH / INTIMIDATION) DBC SpellLevel 恒为 0,
//     必须在专精 GetTalentSpellMinLevel 显式登记最低解锁等级 (铁律 3);
//   * 被动天赋必须注入满阶 Rank 根源 ID, 严禁注入 Rank 1 根源造成数值缩水 (铁律 33)。
// =============================================================================
namespace BeastMasteryHunterSpells
{
    // =========================================================================
    // 一、守护、核心射击与战术大招
    // =========================================================================
    constexpr uint32 AUTO_SHOT                = 75;     // 自动射击 (远程普攻核心通道)
    constexpr uint32 SERPENT_STING            = 49001;  // 毒蛇钉刺 Rank 12 (自然 DoT, 激活稳固雕文增伤)
    constexpr uint32 ARCANE_SHOT              = 49045;  // 奥术射击 Rank 11 (6s CD, 瞬发核心奥术伤害)
    constexpr uint32 MULTI_SHOT               = 49048;  // 多重射击 Rank 6 (10s CD, 瞬发多目标/副单体填充)
    constexpr uint32 STEADY_SHOT              = 49052;  // 稳固射击 Rank 4 (读条核心填充射击)
    constexpr uint32 KILL_SHOT                = 61006;  // 杀戮射击 Rank 3 (目标<20%强力物理斩杀, 15s/9s CD)
    constexpr uint32 BESTIAL_WRATH            = 19574;  // 狂野怒火 (纯天赋, 40级解锁, 基础120s CD)
    constexpr uint32 RAPID_FIRE               = 3045;   // 急速射击 (3m CD, +40%远程急速, Off-GCD)
    constexpr uint32 KILL_COMMAND             = 34026;  // 杀戮命令 (1m CD, 战术增伤)
    constexpr uint32 INTIMIDATION             = 19577;  // 胁迫 (纯天赋, 30级解锁, 1m/42s CD, 战术昏迷/打断)
    constexpr uint32 MISDIRECTION             = 34477;  // 误导 (30s CD, 仇恨转移给主坦)
    constexpr uint32 FEIGN_DEATH              = 5384;   // 假死 (30s CD, 脱困清仇恨)
    constexpr uint32 DETERRENCE               = 19263;  // 威慑 (90s CD, 100%招架/偏斜硬减伤)
    constexpr uint32 DISENGAGE                = 781;    // 逃脱 (25s CD, 盲区后跳拉开距离)
    constexpr uint32 ASPECT_OF_THE_DRAGONHAWK = 61846;  // 龙鹰守护 Rank 2 (常驻 AP 与闪避增益)
    constexpr uint32 ASPECT_OF_THE_VIPER      = 34074;  // 蝰蛇守护 (回蓝守护)

    // =========================================================================
    // 二、触发光环、被动天赋与雕文补偿
    // =========================================================================
    constexpr uint32 AURA_THE_BEAST_WITHIN    = 34471;  // 野兽之心红人光环 (+20%伤害, -50%蓝耗, 免疫控制, 持续10s/18s)
    constexpr uint32 AURA_RAPID_FIRE          = 3045;   // 急速射击光环
    constexpr uint32 AURA_SERPENT_STING       = 49001;  // 毒蛇钉刺 Debuff

    constexpr uint32 FEROCIOUS_INSPIRATION    = 34460;  // 凶猛灵感 Rank 3 (稳固/奥术/普攻暴击使全队伤害+3%)
    constexpr uint32 SERPENTS_SWIFTNESS       = 34467;  // 毒蛇迅捷 Rank 5 (远程与近战物理攻速 +20%)
    constexpr uint32 LONGEVITY                = 53264;  // 长寿 Rank 3 (狂野怒火与胁迫冷却缩短 30%)
    constexpr uint32 COBRA_STRIKES            = 53260;  // 眼镜蛇打击 Rank 3 (射击暴击增益)
    constexpr uint32 ASPECT_MASTERY           = 53271;  // 守护掌握 (龙鹰 AP 提高, 蝰蛇回蓝提高)
    constexpr uint32 UNLEASHED_FURY           = 19620;  // 狂怒释放 Rank 5 (伤害加成)
    constexpr uint32 THE_BEAST_WITHIN         = 34692;  // 野兽之心天赋根源
    constexpr uint32 BEAST_MASTERY            = 53270;  // 野兽主宰 51点天赋根源
    constexpr uint32 CAREFUL_AIM              = 34483;  // 仔细瞄准 Rank 3 (智力 100% 转化为攻强)
    constexpr uint32 MORTAL_SHOTS             = 53248;  // 致死射击 Rank 5 (远程技能暴击伤害 +30%)
    constexpr uint32 THRILL_OF_THE_HUNT       = 34499;  // 狩猎刺激 Rank 3 (技能暴击返还 40% 法力)

    constexpr uint32 GLYPH_OF_BESTIAL_WRATH   = 56829;  // 狂野怒火雕文 (狂野怒火冷却缩短 20s)
    constexpr uint32 GLYPH_OF_KILL_SHOT       = 56844;  // 杀戮射击雕文 (杀戮射击冷却缩短 6s)
    constexpr uint32 GLYPH_OF_STEADY_SHOT     = 56833;  // 稳固射击雕文 (目标有毒蛇钉刺时稳固伤害 +10%)
    constexpr uint32 GLYPH_OF_SERPENT_STING   = 56828;  // 毒蛇钉刺雕文 (毒蛇钉刺持续时间延长 6s)
}

#endif // _BOT_BEAST_MASTERY_HUNTER_SPELLS_H
