/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "ShadowPriestSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "SpellAuras.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Chat.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

class BotShadowPriestAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位与射程参数 (严格遵循 CONTEXT.md 铁律 17 迟滞区间模型)
    // =========================================================================
    static constexpr float MELEE_BLIND_DIST   = 8.0f;   // 近战盲区阈值：进入即触发贴身避难后撤
    static constexpr float RETREAT_EXIT_DIST  = 15.0f;  // 撤退迟滞退出线：必须严格大于 MELEE_BLIND_DIST
    static constexpr float TANK_RETREAT_DIST  = 15.0f;  // 锚定坦克背身位时的跟随距离 (必须 >= 迟滞退出线)
    static constexpr float IDEAL_SHOT_DIST    = 20.0f;  // 理想施法站位：锚定精神鞭笞 20 码引导上限
    static constexpr float MAX_ENGAGE_DIST    = 30.0f;  // 脱节上限：与触/震爆/痛 30 码射程严格对齐
    static constexpr float MIND_FLAY_MAX_DIST = 20.0f;  // 精神鞭笞引导射程

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」，引擎内部已叠加目标朝向。
    // 严禁自行叠加 victim->GetOrientation()，否则站位会随目标转向持续漂移。
    static constexpr float BEHIND_ANGLE       = static_cast<float>(M_PI);

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪 (HasSpellCooldown 恒 false)，
    // 凡无「持续光环保护」的主动技能必须由专精自行计时，
    // 否则会因判定恒真而在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_MIND_BLAST        = 8000;    // 心灵震爆 8s 本体 CD
    static constexpr uint32 CD_SHADOW_WORD_DEATH = 12000;   // 暗言术：灭 12s 本体 CD
    static constexpr uint32 CD_SHADOWFIEND       = 180000;  // 暗影恶魔 3 分钟 CD
    static constexpr uint32 CD_DISPERSION        = 120000;  // 消散 2 分钟 CD
    static constexpr uint32 CD_DISPERSION_GLYPH  = 75000;   // 消散 (消散雕文) 75s CD

    // =========================================================================
    // 战术阈值
    // =========================================================================
    static constexpr int32  VT_REFRESH_WINDOW_MS  = 1500;  // 吸血鬼之触刷新窗口 (刚好覆盖 1.5s 读条)
    static constexpr int32  SWP_REFRESH_WINDOW_MS = 2000;  // 痛的手工续期窗口 (仅在未注入苦修与磨难时启用)
    static constexpr float  DISPERSION_HP_PCT     = 20.0f; // 消散自保血线
    static constexpr float  DISPERSION_MANA_PCT   = 10.0f; // 消散回蓝法力线
    static constexpr float  SHIELD_HP_PCT         = 35.0f; // 真言术：盾救急血线
    static constexpr float  FIEND_MANA_PCT        = 50.0f; // 暗影魔回蓝法力线
    static constexpr float  SWD_EXECUTE_HP_PCT    = 25.0f; // 暗言术：灭斩杀阈值
    static constexpr float  SWD_SAFE_HP_PCT       = 50.0f; // 灭的反噬自残安全血线
    static constexpr uint8  PASSIVE_MIN_LEVEL     = 20;    // 被动光环注入基准等级 (天赋前置点数的近似门槛)
    static constexpr uint8  PAIN_SUFFERING_LEVEL  = 35;    // 苦修与磨难 (深影第 4 层) 的近似前置门槛

public:
    explicit BotShadowPriestAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 远程随从按远程单位接管移动逻辑，禁止迈入怪物近战范围
    bool IsRangedBot() const override { return true; }

    // 法系远程 (与猎人物理远程解耦)：伤害由法术强度与暗影乘数通道支撑
    bool IsRangedPhysicalBot() const override { return false; }

    // 按专精契约固定 1.0x：不继承基类为法系远程预留的 2.0x ~ 3.3x 装备装等放大通道
    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注：基础法术 (痛/瘟疫/震爆/灭/暗影魔/盾/心灵之火/祷言等) 严禁登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case ShadowPriestSpells::MIND_FLAY:        return 20;
            case ShadowPriestSpells::VAMPIRIC_EMBRACE: return 30;
            case ShadowPriestSpells::SHADOWFORM:       return 40;
            case ShadowPriestSpells::VAMPIRIC_TOUCH:   return 50;
            case ShadowPriestSpells::DISPERSION:       return 60;
            default:                                   return 0;
        }
    }

    // =========================================================================
    // 生命周期
    // =========================================================================
    void Reset() override
    {
        // 法力通道必须在基类 Reset 之前配置，以便等级同步时正确补满法力池
        me->setPowerType(POWER_MANA);
        AdaptiveBotAI::Reset();

        if (me->GetMaxPower(POWER_MANA) > 0)
            me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));

        ResetShadowTimers();
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 level) override
    {
        AdaptiveBotAI::OnLevelSynced(level);
        me->setPowerType(POWER_MANA);

        if (me->GetMaxPower(POWER_MANA) > 0)
            me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));

        ApplyPassiveTalents();
    }

    // =========================================================================
    // 核心决策循环
    // =========================================================================
    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);
        UpdateShadowTimers(diff);

        // =====================================================================
        // 引导死锁破除 (必须置于全局通道守卫之前)
        // ---------------------------------------------------------------------
        // 精神鞭笞单次引导 3 秒，期间全局通道守卫会锁定整个决策流。
        // 若引导途中吸血鬼之触步入刷新窗口 (余量 <= 1.5s)，剩余全部跳数都将在
        // 缺失触 DoT (及全团回蓝收益) 的情况下空抽，必须主动掐断通道立即重补。
        // =====================================================================
        if (Spell* const channeled = me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        {
            SpellInfo const* const channeledInfo = channeled->GetSpellInfo();
            SpellInfo const* const mindFlayInfo  = sSpellMgr->GetSpellInfo(ShadowPriestSpells::MIND_FLAY);

            // 以「同根 Rank」判定而非裸 ID 比较：低等级降阶施放的鞭笞同样是合法通道
            if (channeledInfo && mindFlayInfo && channeledInfo->IsRankOf(mindFlayInfo) &&
                GetTalentRank(ShadowPriestSpells::MIND_FLAY) > 0)
            {
                Unit* const channelTarget = me->GetVictim();

                bool const targetLost = (!channelTarget || !channelTarget->IsAlive() ||
                                         !channelTarget->IsInWorld() || channelTarget->GetMap() != me->GetMap());

                // 等级自适应门禁：未习得吸血鬼之触 (< 50 级) 时余量恒为 0，
                // 若不加天赋门禁，会在每一次引导的首帧就无脑秒断鞭笞。
                bool const vampiricTouchUrgent =
                    (GetTalentRank(ShadowPriestSpells::VAMPIRIC_TOUCH) > 0) && !targetLost &&
                    (GetOwnDotRemaining(channelTarget, ShadowPriestSpells::VAMPIRIC_TOUCH) <= VT_REFRESH_WINDOW_MS);

                if (targetLost || vampiricTouchUrgent)
                    me->InterruptSpell(CURRENT_CHANNELED_SPELL);
            }
        }

        // 全局读条/引导双保险守卫：消散与精神鞭笞均为引导通道，绝不被走位或填充指令掐断
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (MaintainBuffs())
                return;

            if (TryEngageCombat())
                return;

            UpdateFollowMaster(diff);
            return;
        }

        // =====================================================================
        // 2. 战斗内 APL
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() ||
            victim->GetMap() != me->GetMap() || !me->IsValidAttackTarget(victim))
        {
            MaintainBuffs();
            return;
        }

        // 远程随从仅锚定敌对目标维持进战姿态供施法链路使用，
        // 第二参数传 false 绝不开启近战追击 (CONTEXT.md 铁律)。
        if (me->GetVictim() != victim)
            me->Attack(victim, false);

        // ---- P0: 极限自保与回蓝 ----
        if (TrySurvival(victim))
            return;

        if (TryManaManagement(victim))
            return;

        // ---- P1: 形态与常驻增益 ----
        if (MaintainBuffs())
            return;

        // ---- P2/P3: 核心暗影循环 ----
        if (TryShadowRotation(victim))
            return;

        // ---- P4: 远程站位与贴身避难 ----
        MaintainRangedPositioning(victim);
    }

private:
    // 自管冷却登记
    uint32 mindBlastCooldown{ 0 };
    uint32 shadowWordDeathCooldown{ 0 };
    uint32 shadowfiendCooldown{ 0 };
    uint32 dispersionCooldown{ 0 };

    // 贴身撤离姿态标记：处于该姿态时必须凭此标记主动重发走位指令，
    // 否则会永久粘在坦克身后而无法恢复 20 码稳定施法窗口。
    bool isRetreatingToTank{ false };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateShadowTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(mindBlastCooldown);
        Tick(shadowWordDeathCooldown);
        Tick(shadowfiendCooldown);
        Tick(dispersionCooldown);
    }

    void ResetShadowTimers()
    {
        mindBlastCooldown = 0;
        shadowWordDeathCooldown = 0;
        shadowfiendCooldown = 0;
        dispersionCooldown = 0;

        isRetreatingToTank = false;
    }

    // =========================================================================
    // 天赋契约等级门禁
    // -------------------------------------------------------------------------
    // 3.3.5a 中天赋法术的 DBC SpellLevel 恒为 0，GetAppropriateRank 不会因等级而降阶，
    // 低等级下依旧返回最高 Rank 的 ID。若不加门禁，40 级牧师会直接搓出吸血鬼之触。
    // 故所有在 GetTalentSpellMinLevel 登记的天赋必须经此函数解析。
    // =========================================================================
    uint32 GetTalentRank(uint32 spellId) const
    {
        if (me->GetLevel() < GetTalentSpellMinLevel(spellId))
            return 0;

        return GetAppropriateRank(spellId, true);
    }

    // =========================================================================
    // 战场态势判定器
    // =========================================================================
    // 首领/精英判定：消散与暗影魔等长 CD 底牌只对高价值目标开放，杜绝小怪战空转
    static bool IsHighValueTarget(Unit* unit)
    {
        if (!unit || !unit->IsAlive() || !unit->IsInWorld())
            return false;

        Creature* const creature = unit->ToCreature();
        if (!creature)
            return false;

        CreatureTemplate const* const proto = creature->GetCreatureTemplate();
        if (!proto)
            return false;

        return creature->isWorldBoss() ||
               proto->rank == CREATURE_ELITE_ELITE ||
               proto->rank == CREATURE_ELITE_RAREELITE ||
               proto->rank == CREATURE_ELITE_WORLDBOSS;
    }

    // 取自身施加的 DoT 剩余时间 (毫秒)；不存在则返回 0
    int32 GetOwnDotRemaining(Unit* victim, uint32 rankedSpellId) const
    {
        if (!victim)
            return 0;

        Aura* const aura = victim->GetAuraOfRankedSpell(rankedSpellId, me->GetGUID());
        return aura ? aura->GetDuration() : 0;
    }

    // =========================================================================
    // 通用施法通道 (立定判定与瞬发解耦)
    // =========================================================================
    // 非瞬发读条：CanCast 通过后立即刹停并当帧起手。
    // 严禁写成「移动中直接 return false」——随从在风筝跑位期间会判定读条永久失败，
    // 决策流被 P4 站位分支反复抢占，DoT 无限期缺席，形成「越跑越不打、越不打越要跑」的死锁。
    // 瞬发技能 (allowMoving = true) 则完整保留机动性，严禁刹停。
    bool TryCastSpell(Unit* victim, uint32 spellId, bool allowMoving)
    {
        if (!spellId || !victim)
            return false;

        // 施法资格必须先通过校验再刹停：CanCast 失败时提前立定，
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(victim, spellId, true))
            return false;

        if (!allowMoving && me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, spellId, true);
    }

    // =========================================================================
    // P0: 极限自保与回蓝
    // =========================================================================
    bool TrySurvival(Unit* victim)
    {
        // ---- 消散：濒死或 (首领/精英战) 法力枯竭时引导，90% 减伤 + 回蓝 ----
        if (dispersionCooldown == 0)
        {
            bool const lifeThreat = (me->GetHealthPct() < DISPERSION_HP_PCT);
            bool const manaThreat = (me->getPowerType() == POWER_MANA) &&
                                    (me->GetPowerPct(POWER_MANA) < DISPERSION_MANA_PCT) &&
                                    IsHighValueTarget(victim);

            if (lifeThreat || manaThreat)
            {
                uint32 const dispersion = GetTalentRank(ShadowPriestSpells::DISPERSION);
                if (dispersion && TryCastSpell(me, dispersion, false))
                {
                    // 雕文动态判定：消散雕文将冷却由 2 分钟压缩至 75s
                    dispersionCooldown = me->HasAura(ShadowPriestSpells::GLYPH_OF_DISPERSION) ? CD_DISPERSION_GLYPH : CD_DISPERSION;
                    return true;
                }
            }
        }

        // ---- 真言术：盾：濒死瞬发套盾救急 (虚弱灵魂门禁防互顶空转) ----
        if (me->GetHealthPct() < SHIELD_HP_PCT && !me->HasAura(ShadowPriestSpells::AURA_WEAKENED_SOUL))
        {
            uint32 const shield = GetAppropriateRank(ShadowPriestSpells::POWER_WORD_SHIELD, false);
            if (shield && TryCastSpell(me, shield, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // P0: 暗影魔回蓝通道
    // =========================================================================
    bool TryManaManagement(Unit* victim)
    {
        if (me->getPowerType() != POWER_MANA)
            return false;

        if (shadowfiendCooldown > 0)
            return false;

        // 法力充盈或目标无战略价值时严禁浪费 3 分钟 CD 的底牌
        if (me->GetPowerPct(POWER_MANA) >= FIEND_MANA_PCT)
            return false;

        if (!IsHighValueTarget(victim))
            return false;

        if (gcdTimer > 0)
            return false;

        uint32 const shadowfiend = GetAppropriateRank(ShadowPriestSpells::SHADOWFIEND, false);
        if (!shadowfiend || !CanCast(victim, shadowfiend, true))
            return false;

        if (!ExecuteSpell(victim, shadowfiend, true))
            return false;

        shadowfiendCooldown = CD_SHADOWFIEND;
        return true;
    }

    // =========================================================================
    // P1: 姿态与常驻增益维护
    // =========================================================================
    bool MaintainBuffs()
    {
        uint32 const shadowform   = GetTalentRank(ShadowPriestSpells::SHADOWFORM);
        bool const inShadowform   = me->HasAura(ShadowPriestSpells::SHADOWFORM);
        bool const outOfCombat    = !me->IsInCombat();

        bool const needInnerFire  = !me->GetAuraOfRankedSpell(ShadowPriestSpells::INNER_FIRE);
        bool const needFortitude  = outOfCombat && !me->GetAuraOfRankedSpell(ShadowPriestSpells::PRAYER_OF_FORTITUDE);
        bool const needShadowProt = outOfCombat && !me->GetAuraOfRankedSpell(ShadowPriestSpells::SHADOW_PROTECTION);
        bool const needHolyBuffs  = needInnerFire || needFortitude || needShadowProt;

        if (!inShadowform)
        {
            // 形态空缺期：必须先把圣光系增益补齐，最后才重新压回暗影形态。
            // 若把形态维持排在增益之前，会与下面的「脱战拔形态」分支形成
            // 「拔形态 -> 立刻变回形态 -> 再拔」的每帧死循环，增益永远挂不上。
            if (needInnerFire)
            {
                uint32 const innerFire = GetAppropriateRank(ShadowPriestSpells::INNER_FIRE, false);
                if (TryCastSpell(me, innerFire, true))
                    return true;
            }

            if (needFortitude)
            {
                uint32 const fortitude = GetAppropriateRank(ShadowPriestSpells::PRAYER_OF_FORTITUDE, false);
                if (TryCastSpell(me, fortitude, true))
                    return true;
            }

            if (needShadowProt)
            {
                uint32 const shadowProtection = GetAppropriateRank(ShadowPriestSpells::SHADOW_PROTECTION, false);
                if (TryCastSpell(me, shadowProtection, true))
                    return true;
            }
        }
        else if (needHolyBuffs)
        {
            // 暗影形态为 MOD_SHAPESHIFT 形态：其下所有圣光系法术都会被
            // SpellInfo::CheckShapeshift 硬性拦截。脱战按 CONTEXT.md 铁律 13
            // 的「形态锁定破除」思路瞬拔形态补挂 (无 GCD)，增益齐备后自动变回。
            if (outOfCombat && shadowform)
            {
                me->RemoveAurasDueToSpell(ShadowPriestSpells::SHADOWFORM);
                return true;
            }

            // 战时不拔形态：保留 15% 承伤减免，圣光系增益让路给输出
        }

        // ---- 暗影形态：绝对优先维持 (必须在圣光系增益齐备后才压回) ----
        if (shadowform && !me->HasAura(ShadowPriestSpells::SHADOWFORM) && !needHolyBuffs)
        {
            if (TryCastSpell(me, shadowform, true))
                return true;
        }

        // ---- 吸血鬼之拥：30 分钟自身光环，暗影伤害转化为全团治疗 ----
        uint32 const vampiricEmbrace = GetTalentRank(ShadowPriestSpells::VAMPIRIC_EMBRACE);
        if (vampiricEmbrace && !me->HasAura(ShadowPriestSpells::VAMPIRIC_EMBRACE))
        {
            if (TryCastSpell(me, vampiricEmbrace, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // P2/P3: 核心暗影循环
    // =========================================================================
    bool TryShadowRotation(Unit* victim)
    {
        if (!victim)
            return false;

        float const dist = me->GetDistance(victim);

        // 近战盲区与超远脱节一律交由 P0 / P4 接管，绝不在此硬读条
        if (dist < MELEE_BLIND_DIST || dist > MAX_ENGAGE_DIST)
            return false;

        // ---- P2: DoT 滚动 (触 -> 瘟疫 -> 痛) ----
        if (TryVampiricTouch(victim))
            return true;

        if (TryDevouringPlague(victim))
            return true;

        if (TryShadowWordPain(victim))
            return true;

        // ---- P3: 直伤与引导填充 ----
        if (TryMindBlast(victim))
            return true;

        if (TryShadowWordDeath(victim))
            return true;

        return TryChannelMindFlay(victim);
    }

    // 吸血鬼之触：全团回蓝的乘数底座，剩余 <= 1.5s 立即读条刷新 (刚好覆盖读条时长)
    bool TryVampiricTouch(Unit* victim)
    {
        uint32 const vampiricTouch = GetTalentRank(ShadowPriestSpells::VAMPIRIC_TOUCH);
        if (!vampiricTouch)
            return false;

        if (GetOwnDotRemaining(victim, ShadowPriestSpells::VAMPIRIC_TOUCH) > VT_REFRESH_WINDOW_MS)
            return false;

        return TryCastSpell(victim, vampiricTouch, false);
    }

    // 噬灵瘟疫：瞬发暗影疾病 DoT，仅在完全缺失自身瘟疫时补齐
    bool TryDevouringPlague(Unit* victim)
    {
        uint32 const devouringPlague = GetAppropriateRank(ShadowPriestSpells::DEVOURING_PLAGUE, false);
        if (!devouringPlague)
            return false;

        if (GetOwnDotRemaining(victim, ShadowPriestSpells::DEVOURING_PLAGUE) > 0)
            return false;

        return TryCastSpell(victim, devouringPlague, true);
    }

    // 暗言术：痛：瞬发核心 DoT
    bool TryShadowWordPain(Unit* victim)
    {
        uint32 const shadowWordPain = GetAppropriateRank(ShadowPriestSpells::SHADOW_WORD_PAIN, false);
        if (!shadowWordPain)
            return false;

        int32 const remaining = GetOwnDotRemaining(victim, ShadowPriestSpells::SHADOW_WORD_PAIN);

        if (remaining > 0)
        {
            // 目标已有自身施加的痛：
            //  - 已注入【苦修与磨难】时，精神鞭笞会 100% 无损续订，严禁手动重刷打乱节奏；
            //  - 未注入时 (低等级段或天赋门槛未达) 才允许在断档前手工续期，
            //    否则斩杀/转火期会出现整段 DoT 空窗。
            if (me->HasAura(ShadowPriestSpells::PAIN_AND_SUFFERING))
                return false;

            if (remaining > SWP_REFRESH_WINDOW_MS)
                return false;
        }

        return TryCastSpell(victim, shadowWordPain, true);
    }

    // 心灵震爆：CD 就绪即读条，提供直伤爆发与暗影交织叠层
    bool TryMindBlast(Unit* victim)
    {
        if (mindBlastCooldown > 0)
            return false;

        uint32 const mindBlast = GetAppropriateRank(ShadowPriestSpells::MIND_BLAST, false);
        if (!mindBlast)
            return false;

        if (!TryCastSpell(victim, mindBlast, false))
            return false;

        mindBlastCooldown = CD_MIND_BLAST;
        return true;
    }

    // 暗言术：灭：斩杀期 (目标 < 25%) 或跑位移动中的瞬发填充
    bool TryShadowWordDeath(Unit* victim)
    {
        if (shadowWordDeathCooldown > 0)
            return false;

        // 反噬自残保护：自身血线不安全时严禁使用，杜绝自杀式补刀
        if (me->GetHealthPct() <= SWD_SAFE_HP_PCT)
            return false;

        bool const executeWindow = (victim->GetHealthPct() < SWD_EXECUTE_HP_PCT);
        bool const movingFiller  = me->isMoving();

        if (!executeWindow && !movingFiller)
            return false;

        uint32 const shadowWordDeath = GetAppropriateRank(ShadowPriestSpells::SHADOW_WORD_DEATH, false);
        if (!shadowWordDeath)
            return false;

        if (!TryCastSpell(victim, shadowWordDeath, true))
            return false;

        shadowWordDeathCooldown = CD_SHADOW_WORD_DEATH;
        return true;
    }

    // 精神鞭笞：全 DoT 齐备后的主力站桩引导填充 (刷新痛并维持暗影交织)
    bool TryChannelMindFlay(Unit* victim)
    {
        uint32 const mindFlay = GetTalentRank(ShadowPriestSpells::MIND_FLAY);
        if (!mindFlay || !victim)
            return false;

        // 引导射程独立收敛：鞭笞仅 20 码，超出即让路给 P4 压进
        if (me->GetDistance(victim) > MIND_FLAY_MAX_DIST)
            return false;

        // DoT 门禁：核心 DoT 缺席时先补 DoT，绝不空抽无乘数加成的引导
        if (GetOwnDotRemaining(victim, ShadowPriestSpells::SHADOW_WORD_PAIN) == 0)
            return false;

        if (!CanCast(victim, mindFlay, true))
            return false;

        // 引导类法术必须立定后引导，否则会被走位指令掐断整段通道
        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, mindFlay, true);
    }

    // =========================================================================
    // P4: 远程站位与贴身避难 (风筝与拉开状态机，维持 20 码施法站位)
    // =========================================================================
    void MaintainRangedPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        me->SetFacingToObject(victim);

        float const dist = me->GetDistance(victim);
        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();

        // ---- A. 脱节过远 (> 30 码)：主动压进至理想施法站位 ----
        if (dist > MAX_ENGAGE_DIST)
        {
            isRetreatingToTank = false;

            if (moveType != CHASE_MOTION_TYPE)
                me->GetMotionMaster()->MoveChase(victim, IDEAL_SHOT_DIST);
            return;
        }

        // ---- 撤退迟滞门禁 ----
        // 进入撤退沿用 MELEE_BLIND_DIST (8 码)，退出撤退必须恢复到 RETREAT_EXIT_DIST (15 码)。
        // 严禁拆除 isRetreatingToTank 这一自我粘滞条件：它是迟滞状态的载体，
        // 一旦在脱离 8 码的瞬间就被清空，撤退姿态会与立定分支每帧交替触发，
        // 表现为随从在原地反复起步/刹停的抽搐，且永远无法进入稳定施法窗口。
        bool const needsRetreat = (dist < MELEE_BLIND_DIST) || (isRetreatingToTank && dist < RETREAT_EXIT_DIST);

        // ---- B. 贴身盲区 / 尚未拉足安全距离：向坦克背身位撤离，借坦克 AoE 仇恨把小怪拉走 ----
        if (needsRetreat)
        {
            Unit* tank = GetGroupTank();
            bool const canAnchorTank = (tank && tank != me && tank != victim && tank->IsAlive() &&
                                        tank->IsInWorld() && tank->GetMap() == me->GetMap());

            if (canAnchorTank)
            {
                if (!isRetreatingToTank || moveType != FOLLOW_MOTION_TYPE)
                {
                    isRetreatingToTank = true;
                    // angle 必须传正后方：传 0.0f 会贴在坦克脸前，被顺劈斩与正面吐息一并打死。
                    me->GetMotionMaster()->MoveFollow(tank, TANK_RETREAT_DIST, BEHIND_ANGLE);
                }
                return;
            }

            // 无坦克可依：严禁对 victim 调用 MoveChase —— ChaseMovementGenerator 在
            // 施法者已位于指定距离「以内」时会原地立定，而随从此刻恰处于 8 码盲区内，
            // 结果是发出指令却纹丝不动，形成粘脸卡死。必须改用矢量外推硬位移：
            // 以 victim 为原点，沿 victim -> me 的当前向量外推至 IDEAL_SHOT_DIST 落点。
            if (!isRetreatingToTank || moveType != POINT_MOTION_TYPE)
            {
                isRetreatingToTank = true;

                float const angle = victim->GetAngle(me);
                float const retreatX = victim->GetPositionX() + IDEAL_SHOT_DIST * std::cos(angle);
                float const retreatY = victim->GetPositionY() + IDEAL_SHOT_DIST * std::sin(angle);

                me->GetMotionMaster()->MovePoint(0, retreatX, retreatY, me->GetPositionZ());
            }
            return;
        }

        // ---- C. 已回到有效射程：立定施法，清空遗留走位发生器 ----
        // 严禁放任 chase / follow / point 发生器常驻：残余走位会持续拉扯随从，
        // 使读条与站位反复互相打断，表现为原地抽搐。
        if (moveType == CHASE_MOTION_TYPE || moveType == FOLLOW_MOTION_TYPE || isRetreatingToTank)
        {
            isRetreatingToTank = false;
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveIdle();
            me->StopMoving();
        }
    }

    // =========================================================================
    // 暗影天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
    // -------------------------------------------------------------------------
    // 注入等级取各天赋前置点数的近似门槛；其中【苦修与磨难】是痛的自动续订硬依赖，
    // 【暗影交织】是暗影增伤叠层来源，必须在低等级段即注入，否则核心循环会静默退化。
    // 满阶 Rank 注入可规避 Rank 1 被动触发概率与加成数值的隐形削弱 (铁律 33)。
    // =========================================================================
    void ApplyPassiveTalents()
    {
        uint8 const level = me->GetLevel();

        auto SyncPassive = [this, level](uint8 minLevel, uint32 spellId)
        {
            if (level >= minLevel)
            {
                if (!me->HasAura(spellId))
                    me->AddAura(spellId, me);
            }
            else
            {
                me->RemoveAurasDueToSpell(spellId);
            }
        };

        SyncPassive(PASSIVE_MIN_LEVEL,    ShadowPriestSpells::SHADOW_WEAVING);      // 暗影交织：暗影增伤叠层来源
        SyncPassive(PAIN_SUFFERING_LEVEL, ShadowPriestSpells::PAIN_AND_SUFFERING);  // 苦修与磨难：鞭笞 100% 续订痛
        SyncPassive(PASSIVE_MIN_LEVEL,    ShadowPriestSpells::DARKNESS);            // 黑暗：暗影法术伤害提升
        SyncPassive(PASSIVE_MIN_LEVEL,    ShadowPriestSpells::SHADOW_POWER);        // 暗影能量：暗影暴击伤害提升
        SyncPassive(PASSIVE_MIN_LEVEL,    ShadowPriestSpells::MISERY);              // 悲惨：目标受法术命中提升
        SyncPassive(PASSIVE_MIN_LEVEL,    ShadowPriestSpells::GLYPH_OF_SHADOW);     // 暗影雕文
        SyncPassive(PASSIVE_MIN_LEVEL,    ShadowPriestSpells::GLYPH_OF_MIND_FLAY);  // 精神鞭笞雕文
        SyncPassive(PASSIVE_MIN_LEVEL,    ShadowPriestSpells::GLYPH_OF_DISPERSION); // 消散雕文：消散 CD 缩短 45s
    }
};

void AddSC_bot_shadow_priest()
{
    new AdaptiveBotScript<BotShadowPriestAI>("bot_shadow_priest");
}
