/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"

namespace BloodDeathKnightSpells
{
    // =========================================================================
    // 1. 姿态与增益 (Presence & Buffs)
    // =========================================================================
    constexpr uint32 FROST_PRESENCE              = 48263; // 冰霜灵气 (3.3.5a 坦克核心姿态)
    constexpr uint32 HORN_OF_WINTER              = 57330; // 寒冬号角 (Rank 2)

    // =========================================================================
    // 2. 仇恨与开怪 (Threat & Opener)
    // =========================================================================
    constexpr uint32 DEATH_GRIP                  = 49576; // 死亡之握 (远程拉怪)
    constexpr uint32 DARK_COMMAND                = 56222; // 黑暗命令 (单体嘲讽)
    constexpr uint32 ICY_TOUCH                   = 49909; // 冰冷触摸 (Rank 5, 14 倍仇恨源)
    constexpr uint32 PLAGUE_STRIKE               = 49921; // 暗影打击 (Rank 6)
    constexpr uint32 PESTILENCE                  = 50842; // 传染 (疾病扩散)
    constexpr uint32 DEATH_AND_DECAY             = 49938; // 枯萎凋零 (Rank 4)
    constexpr uint32 BLOOD_BOIL                  = 49941; // 血液沸腾 (Rank 4)
    constexpr uint32 RUNE_STRIKE                 = 56815; // 符文打击 (下一次平砍强化)
    constexpr uint32 HEART_STRIKE                = 55050; // 心脏打击 (Rank 6)

    // =========================================================================
    // 2.1 打断 (Interrupt)
    // =========================================================================
    constexpr uint32 MIND_FREEZE                 = 47528; // 心灵冰冻 (基础技能, 10s CD, 压秒打断)

    // =========================================================================
    // 3. 自疗与减伤 (Self-Heal & Cooldowns)
    // =========================================================================
    constexpr uint32 DEATH_STRIKE                = 49998; // 灵界打击 (Rank 5)
    constexpr uint32 ANTI_MAGIC_SHELL            = 48707; // 反魔法护盾 (绿罩)
    constexpr uint32 ICEBOUND_FORTITUDE          = 48792; // 冰封之韧
    constexpr uint32 VAMPIRIC_BLOOD              = 55233; // 吸血鬼之血
    constexpr uint32 RUNE_TAP                    = 48982; // 符文分流 (Rank 2)

    // =========================================================================
    // 4. 疾病 DeBuff 与被动天赋 (Diseases & Passives)
    // =========================================================================
    constexpr uint32 AURA_FROST_FEVER            = 55095; // 冰霜疫病
    constexpr uint32 AURA_BLOOD_PLAGUE           = 55078; // 暗影疫病
    constexpr uint32 WILL_OF_THE_NECROPOLIS      = 50150; // 大墓地的意志 (35% 血以下硬减伤)
    constexpr uint32 VETERAN_OF_THE_THIRD_WAR    = 50034; // 三战老兵 (耐力/力量/护甲)
    constexpr uint32 TOUGHNESS                   = 49042; // 坚韧 (护甲提升)
    constexpr uint32 ANTICIPATION                = 55129; // 预知 (闪避提升)

    // =========================================================================
    // 5. 天赋依赖技能的最低等级契约
    // =========================================================================
    constexpr uint8  HEART_STRIKE_MIN_LEVEL      = 55;    // 心脏打击解锁等级
    constexpr uint8  DEATH_STRIKE_MIN_LEVEL      = 56;    // 灵界打击解锁等级
    constexpr uint8  VAMPIRIC_BLOOD_MIN_LEVEL    = 60;    // 吸血鬼之血 (血系天赋) 解锁等级
    constexpr uint8  RUNE_TAP_MIN_LEVEL          = 66;    // 符文分流解锁等级
    constexpr uint8  PASSIVE_TALENT_MIN_LEVEL    = 60;    // 被动天赋光环注入等级
}
