/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef _FURY_WARRIOR_SPELLS_H_
#define _FURY_WARRIOR_SPELLS_H_

#include "Define.h"

// =============================================================================
// 狂暴战 (Fury Warrior) 3.3.5a 法术常量与天赋 ID
// -----------------------------------------------------------------------------
// 命名规范与其余随从专精保持一致（RetributionPaladinSpells / ArcaneMageSpells 等），
// 供 BotFuryWarriorAI.cpp 以 FuryWarriorSpells::XXX 形式引用。
//
// 等级门禁约定（配合 AdaptiveBotAI::GetTalentSpellMinLevel / GetAppropriateRank）：
//   - 纯天赋技能（血之狂热、重伤、乱舞、无尽怒气、泰坦之握、暴怒）DBC SpellLevel 恒为 0，
//     必须在 GetTalentSpellMinLevel 注册并由 GetTalentRank 解析，否则低等级随从会
//     持续尝试施放最高 Rank 而空转烧 GCD。
//   - 训练师基础技能（嗜血、旋风斩、斩杀、英勇打击、顺劈斩、鲁莽、死亡之愿、
//     血性狂暴、狂怒回复、战斗怒吼、拳击、拦截）交回 GetAppropriateRank 依据
//     DBC SpellLevel 自动降阶，严禁登记为天赋。
// =============================================================================
namespace FuryWarriorSpells
{
    // =========================================================================
    // 核心姿态
    // -------------------------------------------------------------------------
    // 狂暴战必须常驻狂暴姿态：命中 +3%、承受伤害 +5%，同时解锁嗜血/旋风斩/拦截。
    // 战斗姿态为低等级或特殊场景（需要压制/雷霆一击）的回退姿态。
    // =========================================================================
    constexpr uint32 BERSERKER_STANCE        = 2458;
    constexpr uint32 BATTLE_STANCE           = 2457;

    // =========================================================================
    // 核心打击技能
    // =========================================================================
    constexpr uint32 BLOODTHIRST             = 23894;   // 嗜血：核心伤害 + 自我治疗
    constexpr uint32 WHIRLWIND               = 1680;    // 旋风斩：多目标 AoE 主输出
    constexpr uint32 SLAM                    = 47475;   // 猛击：嗜血/旋风斩双 CD 时的填充
    constexpr uint32 EXECUTE                 = 47471;   // 斩杀：20% 血线以下泄怒斩杀
    constexpr uint32 HEROIC_STRIKE           = 47450;   // 英勇打击：副手平砍队列强化
    constexpr uint32 CLEAVE                  = 47520;   // 顺劈斩：多目标时的泄怒手段

    // =========================================================================
    // 爆发、怒气与自保
    // =========================================================================
    constexpr uint32 DEATH_WISH              = 12292;   // 死亡之愿：伤害 +20%，受疗 -5%
    constexpr uint32 RECKLESSNESS            = 1719;    // 鲁莽：100% 暴击率，承受伤害 +20%
    constexpr uint32 BLOODRAGE               = 2687;    // 血性狂暴：立即产怒 + 持续产怒
    constexpr uint32 BERSERKER_RAGE          = 18499;   // 狂暴之怒：解除恐惧/闷棍/瘫痪并免疫
    constexpr uint32 ENRAGED_REGENERATION    = 55694;   // 狂怒回复：消耗怒气持续回血
    constexpr uint32 BATTLE_SHOUT            = 47436;   // 战斗怒吼：全团 AP 增益

    // =========================================================================
    // 打断与突进
    // =========================================================================
    constexpr uint32 PUMMEL                  = 6552;    // 拳击：狂暴姿态近战瞬发打断
    constexpr uint32 INTERCEPT               = 20252;   // 拦截：战斗/狂暴姿态突进并短暂定身

    // =========================================================================
    // 被动、天赋与触发光环
    // -------------------------------------------------------------------------
    // 触发光环 (AURA_*) 由引擎在暴击/受击时自动施加，只用于 HasAura 判定，
    // 严禁作为主动施法目标下发给 ExecuteSpell。
    // =========================================================================
    constexpr uint32 AURA_BLOODSURGE         = 46916;   // 血涌触发光环：下一次猛击瞬发
    constexpr uint32 BLOODSURGE              = 46915;   // 血涌天赋 (Rank 3)：暴击触发的被动根源
    constexpr uint32 FLURRY                  = 16282;   // 乱舞：暴击后叠加 5 层攻速提升
    constexpr uint32 DEEP_WOUNDS             = 12867;   // 重伤 Rank 3：暴击附带 48% 武器伤害流血 DoT
    constexpr uint32 RAMPAGE                 = 29801;   // 暴怒：击杀后叠加 AP 增益
    constexpr uint32 UNENDING_FURY           = 29632;   // 无尽怒气：击杀后回怒
    constexpr uint32 TITANS_GRIP             = 46917;   // 泰坦之握：双持双手武器

    // =========================================================================
    // 主力雕文
    // -------------------------------------------------------------------------
    // 由 ApplyPassiveTalents 按等级注入并常驻维持 (HasAura 判定 + AddAura 补偿)。
    // =========================================================================
    constexpr uint32 GLYPH_OF_WHIRLWIND      = 58370;   // 旋风斩雕文：旋风斩伤害 +10%
    constexpr uint32 GLYPH_OF_HEROIC_STRIKE  = 58357;   // 英勇打击雕文：英勇打击暴击率 +5%
}

#endif // _FURY_WARRIOR_SPELLS_H_
