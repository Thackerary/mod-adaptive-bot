/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef MOD_ADAPTIVE_BOT_ARMS_WARRIOR_SPELLS_H
#define MOD_ADAPTIVE_BOT_ARMS_WARRIOR_SPELLS_H

#include "Define.h"

namespace ArmsWarriorSpells
{
    enum : uint32
    {
        // =====================================================================
        // 姿态、战吼与核心打击
        // =====================================================================
        BATTLE_STANCE            = 2457,   // 战斗姿态 (武器战全部核心打击的姿态底座)
        MORTAL_STRIKE            = 47486,  // 致死打击 Rank 8 (40 级纯天赋, 6s CD, -50% 受疗)
        BLADESTORM               = 46924,  // 利刃风暴 (60 级纯天赋, 6s 自转旋风, 1.5m CD)
        SWEEPING_STRIKES         = 12292,  // 横扫攻击 (30 级纯天赋, 下 5 次近战伤害溅射, 30s CD)
        OVERPOWER                = 7384,   // 压制 (血之气息触发 / 目标招架闪避后可用, 5 怒气)
        REND                     = 47465,  // 撕裂 Rank 10 (核心物理流血 DoT, 维持血之气息底座)
        EXECUTE                  = 47471,  // 斩杀 Rank 9 (猝死触发或目标 < 20% 的核弹填充)
        SLAM                     = 47475,  // 猛击 Rank 8 (强化猛击后 0.5s 读条填充)
        HEROIC_STRIKE            = 47450,  // 英勇打击 Rank 13 (高怒气单体泄怒队列)
        CLEAVE                   = 47520,  // 顺劈斩 Rank 8 (多目标泄怒队列)
        CHARGE                   = 11578,  // 冲锋 Rank 3 (8~25 码突进, 主宰天赋解锁战时冲锋)
        SHATTERING_THROW         = 64382,  // 碎裂投掷 (破除无敌并降 20% 护甲, 5m CD)
        BATTLE_SHOUT             = 47436,  // 战斗怒吼 Rank 9 (攻强增益)
        BLOODRAGE                = 2687,   // 血性狂暴 (瞬间产怒 + 激怒状态)
        BERSERKER_RAGE           = 18499,  // 狂暴之怒 (解恐 + 激怒状态, 30s CD)
        ENRAGED_REGENERATION     = 55694,  // 狂暴回复 (激怒自保回血, 3m CD)
        RETALIATION              = 20230,  // 反击风暴 (近战被围攻反弹, 5m CD)

        // =====================================================================
        // 触发光环、被动天赋与雕文补偿
        // =====================================================================
        AURA_TASTE_FOR_BLOOD     = 60503,  // 血之气息触发光环 (使压制可用)
        TASTE_FOR_BLOOD          = 56638,  // 血之气息 Rank 3 天赋根源 (撕裂跳数 100% 触发)
        AURA_SUDDEN_DEATH        = 52437,  // 猝死触发光环 (使斩杀可用且保留 10 怒)
        SUDDEN_DEATH             = 29725,  // 猝死 Rank 3 天赋根源 (近战命中 9% 触发)
        JUGGERNAUT               = 64976,  // 主宰 Rank 1 (战时可用冲锋, 冲锋后致死/猛击暴击 +25%)
        IMPROVED_SLAM            = 12862,  // 强化猛击 Rank 2 (猛击读条时间减少 1.0s)
        DEEP_WOUNDS              = 12867,  // 重伤 Rank 3 (暴击触发 48% 武器伤害流血)
        TRAUMA                   = 46855,  // 创伤 Rank 2 (暴击提升目标所受流血伤害 30%)
        UNRELENTING_ASSAULT      = 46860,  // 无坚不摧 Rank 2 (压制冷却降低 4s, 并降低目标法强/治疗)
        TWO_HANDED_WEAPON_SPEC   = 12714,  // 双手武器专精 Rank 5 (双手武器物理伤害提升 10%)
        GLYPH_OF_MORTAL_STRIKE   = 58368,  // 致死打击雕文 (致死打击伤害 +10%)
        GLYPH_OF_REND            = 58386,  // 撕裂雕文 (撕裂持续时间延长 6s)
        GLYPH_OF_BLADESTORM      = 63324,  // 利刃风暴雕文 (利刃风暴冷却时间缩短 15s)
        GLYPH_OF_EXECUTION       = 58367,  // 斩杀雕文 (斩杀视为额外消耗 10 点怒气结算伤害)
    };
}

#endif // MOD_ADAPTIVE_BOT_ARMS_WARRIOR_SPELLS_H
