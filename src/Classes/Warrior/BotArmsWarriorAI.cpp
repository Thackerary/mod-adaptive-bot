/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "ArmsWarriorSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "SpellAuras.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "SpellAuraDefines.h"
#include "Chat.h"
#include <algorithm>
#include <cmath>
#include <vector>

class BotArmsWarriorAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位参数 (物理近战背后找背模型，铁律 16)
    // =========================================================================
    static constexpr float MELEE_REACH_DIST   = 4.0f;   // 近战判定区上限：超出必须重新贴背
    static constexpr float MELEE_COMFORT_DIST = 3.0f;   // 贴身阈值：进入后保持平滑贴背输出
    static constexpr float MELEE_FOLLOW_DIST  = 1.5f;   // 理想站位：目标正后方 1.5 码

    // MoveFollow 的 angle 为「相对目标朝向的偏移」，引擎内部已叠加目标朝向。
    // 严禁自行叠加 victim->GetOrientation()，否则站位会随目标转向持续漂移。
    // M_PI 即目标正后方：背身位，可规避正面顺劈、吐息以及被招架加速。
    static constexpr float BEHIND_ANGLE       = static_cast<float>(M_PI);

    // 多目标雷达
    static constexpr float  CLEAVE_RADIUS        = 8.0f;
    static constexpr uint32 SWEEPING_MIN_TARGETS = 2;

    // =========================================================================
    // 怒气池语义
    // -------------------------------------------------------------------------
    // 引擎内部怒气为 10 倍定点存储 (1000 == 100 怒气)，与基类 CanCast 中的
    // `if (PowerType == POWER_RAGE) cost *= 10;` 完全对齐。
    // =========================================================================
    static constexpr uint32 MAX_RAGE                    = 1000;
    static constexpr uint32 RAGE_LOW_THRESHOLD          = 200;   // 20 怒气：血性狂暴补怒线
    static constexpr uint32 RAGE_SHOUT_THRESHOLD        = 100;   // 10 怒气：战斗怒吼最低门槛
    static constexpr uint32 RAGE_CORE_STRIKE_THRESHOLD  = 300;   // 30 怒气：致死/猛击的投放线
    static constexpr uint32 RAGE_DUMP_THRESHOLD         = 650;   // 65 怒气：平砍队列泄怒线

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪 (HasSpellCooldown 恒 false)，
    // 凡无「持续光环保护」的 CD 技能必须由专精自行计时，
    // 否则会因判定恒真而在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_MORTAL_STRIKE          = 6000;
    static constexpr uint32 CD_SWEEPING_STRIKES       = 30000;
    static constexpr uint32 CD_SHATTERING_THROW       = 300000;
    static constexpr uint32 CD_RETALIATION            = 300000;
    static constexpr uint32 CD_BERSERKER_RAGE         = 30000;
    static constexpr uint32 CD_BLOODRAGE              = 60000;
    static constexpr uint32 CD_ENRAGED_REGENERATION   = 180000;
    static constexpr uint32 CD_CHARGE                 = 15000;
    static constexpr uint32 CD_BLADESTORM             = 90000;   // 利刃风暴本体 1.5 分钟 CD
    static constexpr uint32 CD_BLADESTORM_GLYPHED     = 75000;   // 利刃风暴雕文：冷却缩短 15s
    static constexpr uint32 CD_HEROIC_STRIKE_QUEUE    = 1200;    // 平砍队列节流 (on-next-swing)
    static constexpr uint32 BATTLE_SHOUT_RETRY_MS     = 5000;    // 战吼被同类 AP 增益覆盖时的重试节流

    // =========================================================================
    // 血线、阶段与窗口阈值
    // =========================================================================
    static constexpr float ENRAGED_REGENERATION_HP_PCT    = 30.0f;  // 狂暴回复自救血线
    static constexpr float RETALIATION_HP_PCT             = 40.0f;  // 反击风暴触发血线
    static constexpr uint32 RETALIATION_MIN_MELEE_ATTACKERS = 2;    // 近战围攻判定门槛
    static constexpr float EXECUTE_PHASE_HP_PCT           = 20.0f;  // 斩杀期血线
    static constexpr float CHARGE_MIN_DIST                = 8.0f;   // 冲锋下限：贴脸无需突进
    static constexpr float CHARGE_MAX_DIST                = 25.0f;  // 冲锋上限
    static constexpr int32 REND_REFRESH_WINDOW_MS         = 1500;   // 撕裂断档预判窗口

    // 目标免疫光环白名单：持有其一即必须用碎裂投掷破盾 (否则 5 分钟 CD 永不触发)
    static constexpr uint32 IMMUNITY_DIVINE_SHIELD        = 642;
    static constexpr uint32 IMMUNITY_HAND_OF_PROTECTION   = 1022;
    static constexpr uint32 IMMUNITY_ICE_BLOCK            = 45438;
    static constexpr uint32 IMMUNITY_DIVINE_INTERVENTION  = 19752;

public:
    explicit BotArmsWarriorAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 物理近战专精：站位与移动交由本专精的背后找背状态机接管
    bool IsRangedBot() const override { return false; }

    // 与猎人/法系远程彻底解耦：不参与任何远程站位与法伤放大通道
    bool IsRangedPhysicalBot() const override { return false; }

    // 物理近战按真实装备模型结算：严禁继承法系远程的 2.0x ~ 3.3x 法伤放大乘数
    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注：训练师基础技能 (致死打击以外的打击、撕裂、压制、斩杀、猛击、英勇打击、
    //     顺劈斩、冲锋、碎裂投掷、战斗怒吼、血性狂暴、狂暴之怒、狂暴回复、反击风暴
    //     以及战斗姿态) 严禁登记于此，其等级门槛由 GetAppropriateRank 依据
    //     DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case ArmsWarriorSpells::SWEEPING_STRIKES: return 30;  // 横扫攻击：武器系天赋，DBC SpellLevel 恒为 0
            case ArmsWarriorSpells::MORTAL_STRIKE:    return 40;  // 致死打击：武器系 31 点核心天赋
            case ArmsWarriorSpells::BLADESTORM:       return 60;  // 利刃风暴：武器系 51 点终极天赋
            default:                                  return 0;
        }
    }

    // =========================================================================
    // 生命周期
    // =========================================================================
    void Reset() override
    {
        // 怒气通道必须在基类 Reset 之前配置，避免等级同步时按法力/能量语义错误处理资源池
        me->setPowerType(POWER_RAGE);
        me->SetMaxPower(POWER_RAGE, MAX_RAGE);
        me->SetPower(POWER_RAGE, 0);

        AdaptiveBotAI::Reset();
        ResetArmsTimers();
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 level) override
    {
        AdaptiveBotAI::OnLevelSynced(level);
        me->setPowerType(POWER_RAGE);

        // 怒气绝不因升级而回满：只校正上限，保持战场收益节奏
        if (me->GetMaxPower(POWER_RAGE) != MAX_RAGE)
            me->SetMaxPower(POWER_RAGE, MAX_RAGE);

        ApplyPassiveTalents();
    }

    // =========================================================================
    // 核心决策循环
    // =========================================================================
    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);
        UpdateArmsTimers(diff);

        // =====================================================================
        // 0. 利刃风暴保护期 (必须置于全局通道守卫之前)
        // ---------------------------------------------------------------------
        // 利刃风暴为 6 秒的自转旋风通道，期间严禁下发任何打击与读条
        // (下发会被底层直接拒绝并白烧决策流与 GCD)，但必须持续贴身跟随目标，
        // 否则目标一旦脱离 8 码旋风范围，整段大招的伤害将全部落空。
        // =====================================================================
        if (me->HasAura(ArmsWarriorSpells::BLADESTORM))
        {
            Unit* spinTarget = me->GetVictim();

            // 转火纠正：若原目标在自转期内死亡/离场，必须当帧检索新目标切入，
            // 否则 6 秒大招会全程对着尸体空转，整段爆发伤害归零。
            if (!spinTarget || !spinTarget->IsAlive() || !spinTarget->IsInWorld() || spinTarget->GetMap() != me->GetMap())
            {
                spinTarget = SelectAssistTarget();
                if (spinTarget && me->IsValidAttackTarget(spinTarget))
                    me->Attack(spinTarget, true);
            }

            if (spinTarget && spinTarget->IsAlive() &&
                me->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
            {
                me->GetMotionMaster()->MoveChase(spinTarget, MELEE_FOLLOW_DIST);
            }
            return;
        }

        // 全局读条守卫：猛击等读条法术会置位 UNIT_STATE_CASTING，
        // 期间严禁被跟随/走位指令掐断 (铁律 1)。
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        // =====================================================================
        // 1. 脱战业务维护 (姿态 + 战吼 + 跟随)
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (MaintainStance())
                return;

            if (MaintainBattleShout())
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
            MaintainStance();
            MaintainBattleShout();
            return;
        }

        // 物理近战开怪：第二参数必须传 true 开启双手白字平砍循环
        // (随从怒气主要来源即为平砍命中，与远程专精的 me->Attack(victim, false) 严格区分)。
        if (me->GetVictim() != victim)
            me->Attack(victim, true);

        // ---- P0: 濒死急救 (狂暴回复同帧顺下) 与近战被围攻自保 ----
        if (TrySurvival())
            return;

        if (TryRetaliation())
            return;

        // ---- P1: 姿态与常驻增益 ----
        if (MaintainStance())
            return;

        if (MaintainBattleShout())
            return;

        // 战时血性狂暴补怒 (Off-GCD，顺下不 return)
        TryBloodrage();

        // ---- P2: 战术突进与破甲爆发 ----
        if (TryCharge(victim))
            return;

        if (TryShatteringThrow(victim))
            return;

        if (TrySweepingStrikes(victim))
            return;

        if (TryBladestorm(victim))
            return;

        // ---- P3: 武器战核心打击 FCFS 循环 ----
        // 猛击为 0.5s 读条，命中后必须当帧交还决策流 (return)，
        // 防止后续平砍队列与走位指令在同帧掐断读条。
        if (TryArmsRotation(victim))
        {
            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
        }

        // ---- P3.5: 平砍队列控怒 (65 怒气门禁，on-next-swing 不占 GCD) ----
        TryRageDumpQueue(victim);

        // ---- P4: 背后找背站位 ----
        MaintainMeleeBehindPositioning(victim);

        // ---- 双手白字平砍驱动 ----
        // ScriptedAI::UpdateAI 已被本类完整接管，引擎不会自动驱动平砍，
        // 必须在决策流末帧显式调用，否则白字伤害与平砍产怒永久缺失。
        // 利刃风暴自转期内严禁驱动白字：自转通道会吞并平砍队列，
        // 强行挥砍会被底层拒放并扰乱通道状态，大招结束后自然恢复。
        if (!me->HasAura(ArmsWarriorSpells::BLADESTORM))
            DoMeleeAttackIfReady();
    }

private:
    // =========================================================================
    // 自管冷却与节流登记
    // =========================================================================
    uint32 mortalStrikeCooldown{ 0 };
    uint32 sweepingStrikesCooldown{ 0 };
    uint32 shatteringThrowCooldown{ 0 };
    uint32 retaliationCooldown{ 0 };
    uint32 bladestormCooldown{ 0 };
    uint32 berserkerRageCooldown{ 0 };
    uint32 bloodrageCooldown{ 0 };
    uint32 enragedRegenerationCooldown{ 0 };
    uint32 chargeCooldown{ 0 };
    uint32 battleShoutRetryTimer{ 0 };
    uint32 heroicStrikeCooldown{ 0 };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateArmsTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(mortalStrikeCooldown);
        Tick(sweepingStrikesCooldown);
        Tick(shatteringThrowCooldown);
        Tick(retaliationCooldown);
        Tick(bladestormCooldown);
        Tick(berserkerRageCooldown);
        Tick(bloodrageCooldown);
        Tick(enragedRegenerationCooldown);
        Tick(chargeCooldown);
        Tick(battleShoutRetryTimer);
        Tick(heroicStrikeCooldown);
    }

    void ResetArmsTimers()
    {
        mortalStrikeCooldown = 0;
        sweepingStrikesCooldown = 0;
        shatteringThrowCooldown = 0;
        retaliationCooldown = 0;
        bladestormCooldown = 0;
        berserkerRageCooldown = 0;
        bloodrageCooldown = 0;
        enragedRegenerationCooldown = 0;
        chargeCooldown = 0;
        battleShoutRetryTimer = 0;
        heroicStrikeCooldown = 0;
    }

    // =========================================================================
    // 天赋契约等级门禁
    // -------------------------------------------------------------------------
    // 3.3.5a 中天赋法术的 DBC SpellLevel 恒为 0，GetAppropriateRank 不会因等级而降阶，
    // 低等级下依旧返回最高 Rank 的 ID。若不加门禁，低等级随从会持续尝试死搓
    // 致死打击 / 利刃风暴 / 横扫攻击。故所有在 GetTalentSpellMinLevel 登记的天赋
    // 必须经此函数解析。
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
    bool IsBossTarget(Unit* target) const
    {
        if (!target)
            return false;

        // PvP 场景直接视同首领
        if (target->IsPlayer())
            return true;

        if (Creature* creature = target->ToCreature())
            return creature->isWorldBoss() || creature->GetCreatureTemplate()->rank >= CREATURE_ELITE_ELITE;

        return false;
    }

    // 目标是否处于「无敌/保护」类免疫光环之下 (碎裂投掷的唯一破盾触发条件)
    bool HasImmunityAura(Unit* target) const
    {
        if (!target)
            return false;

        return target->HasAura(IMMUNITY_DIVINE_SHIELD) ||
               target->HasAura(IMMUNITY_HAND_OF_PROTECTION) ||
               target->HasAura(IMMUNITY_ICE_BLOCK) ||
               target->HasAura(IMMUNITY_DIVINE_INTERVENTION);
    }

    // 近战围攻计数：统计当前贴身 4 码内正在攻击随从的敌对单位数量
    uint32 CountMeleeAttackers() const
    {
        uint32 count = 0;
        for (Unit* attacker : me->getAttackers())
        {
            if (attacker && attacker->IsAlive() && attacker->IsInWorld() &&
                attacker->GetMap() == me->GetMap() && attacker->IsWithinMeleeRange(me))
                ++count;
        }
        return count;
    }

    // 多目标雷达：统计「自身 / 主坦 / 指挥官」三点仇恨链上 8 码内的敌对单位数量，
    // 供顺劈斩、横扫攻击与利刃风暴的多目标收益判定使用 (含去重，防止同一怪物重复计数)。
    uint32 CountNearbyEnemies(float radius)
    {
        std::vector<Unit*> enemies;

        auto Gather = [&enemies](Unit* source)
        {
            if (!source)
                return;

            for (Unit* attacker : source->getAttackers())
            {
                if (attacker && std::find(enemies.begin(), enemies.end(), attacker) == enemies.end())
                    enemies.push_back(attacker);
            }
        };

        Gather(me);
        Gather(GetGroupTank());
        Gather(GetMaster());

        uint32 count = 0;
        for (Unit* enemy : enemies)
        {
            if (!enemy || !enemy->IsAlive() || !enemy->IsInWorld() || enemy->GetMap() != me->GetMap())
                continue;

            if (!me->IsValidAttackTarget(enemy))
                continue;

            if (me->IsWithinDist(enemy, radius))
                ++count;
        }

        return count;
    }

    // 取自身施加的 DoT 剩余时间 (毫秒)；不存在则返回 0。
    // 严禁直接用满级 ID 做 HasAura —— 低等级降阶施放时判定必然失败，会陷入每帧空转重刷。
    int32 GetOwnDotRemaining(Unit* victim, uint32 rankedSpellId) const
    {
        if (!victim)
            return 0;

        Aura* const aura = victim->GetAuraOfRankedSpell(rankedSpellId, me->GetGUID());
        return aura ? aura->GetDuration() : 0;
    }

    // =========================================================================
    // P0: 濒死急救 (狂暴回复 + 激怒同帧顺下，铁律 27)
    // -------------------------------------------------------------------------
    // 狂暴回复底层硬性要求「激怒状态」且消耗 15 点怒气。若随从身上无激怒，
    // 当帧立即以狂暴之怒 / 血性狂暴补齐前置，随后在同一帧内完成施放 ——
    // 绝不允许 return false 把决策权让给 P3 打击循环：那会让致死/斩杀在同帧
    // 把怒气压到 15 点以下，下一帧狂暴回复因怒气不足被底层拒绝，
    // 随从当场被钉死在濒死血线上。
    // =========================================================================
    bool TrySurvival()
    {
        if (me->GetHealthPct() >= ENRAGED_REGENERATION_HP_PCT)
            return false;

        if (enragedRegenerationCooldown > 0)
            return false;

        bool const hasEnrage = me->HasAura(ArmsWarriorSpells::BERSERKER_RAGE) ||
                               me->HasAura(ArmsWarriorSpells::BLOODRAGE);
        if (!hasEnrage && !TryTriggerEnrage())
            return false;

        uint32 const enragedRegeneration = GetAppropriateRank(ArmsWarriorSpells::ENRAGED_REGENERATION, false);
        if (enragedRegeneration && !me->HasAura(enragedRegeneration) &&
            CanCast(me, enragedRegeneration, true))
        {
            if (ExecuteSpell(me, enragedRegeneration, true))
            {
                enragedRegenerationCooldown = CD_ENRAGED_REGENERATION;

                if (isDebugLogging)
                    LOG_INFO("scripts", "[Bot: {}] 狂暴回复自救 (生命 {}%)。", me->GetName(), me->GetHealthPct());

                return true;
            }
        }

        return false;
    }

    // 激怒前置源：狂暴之怒 (顺带解控、冷却更短) 优先，血性狂暴兜底
    bool TryTriggerEnrage()
    {
        if (berserkerRageCooldown == 0)
        {
            uint32 const berserkerRage = GetAppropriateRank(ArmsWarriorSpells::BERSERKER_RAGE, false);
            if (berserkerRage && !me->HasAura(berserkerRage) &&
                CanCast(me, berserkerRage, true) && ExecuteSpell(me, berserkerRage, true))
            {
                berserkerRageCooldown = CD_BERSERKER_RAGE;
                return true;
            }
        }

        if (bloodrageCooldown == 0)
        {
            uint32 const bloodrage = GetAppropriateRank(ArmsWarriorSpells::BLOODRAGE, false);
            if (bloodrage && !me->HasAura(bloodrage) &&
                CanCast(me, bloodrage, true) && ExecuteSpell(me, bloodrage, true))
            {
                bloodrageCooldown = CD_BLOODRAGE;
                return true;
            }
        }

        return false;
    }

    // =========================================================================
    // P0: 近战被围攻自保 (反击风暴)
    // =========================================================================
    bool TryRetaliation()
    {
        if (retaliationCooldown > 0)
            return false;

        if (me->GetHealthPct() >= RETALIATION_HP_PCT)
            return false;

        if (CountMeleeAttackers() < RETALIATION_MIN_MELEE_ATTACKERS)
            return false;

        // 反击风暴为战斗姿态专属技能，姿态不匹配时底层会直接拒绝施放
        if (!me->HasAura(ArmsWarriorSpells::BATTLE_STANCE))
            return false;

        uint32 const retaliation = GetAppropriateRank(ArmsWarriorSpells::RETALIATION, false);
        if (!retaliation || !CanCast(me, retaliation, true))
            return false;

        if (ExecuteSpell(me, retaliation, true))
        {
            retaliationCooldown = CD_RETALIATION;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P2: 冲锋突进
    // -------------------------------------------------------------------------
    // 目标脱离近战盲区 (8 码) 且仍在突进射程 (25 码) 内时瞬间贴身并昏迷，
    // 根除武器战在换目标 / Boss 位移 / 转阶段后长时间追不上导致的平砍产怒断供。
    // =========================================================================
    bool TryCharge(Unit* victim)
    {
        if (chargeCooldown > 0 || !victim)
            return false;

        if (!victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return false;

        if (!me->IsValidAttackTarget(victim))
            return false;

        float const dist = me->GetDistance(victim);
        if (dist < CHARGE_MIN_DIST || dist > CHARGE_MAX_DIST)
            return false;

        // 冲锋为战斗姿态专属技能
        if (!me->HasAura(ArmsWarriorSpells::BATTLE_STANCE))
            return false;

        // 原生冲锋要求脱战；仅当注入【主宰】(64976) 后才允许战时冲锋。
        // 未注入主宰的等级段严禁在战斗中空放冲锋 —— 底层会直接拒绝并白烧决策流。
        if (me->IsInCombat() && !me->HasAura(ArmsWarriorSpells::JUGGERNAUT))
            return false;

        uint32 const charge = GetAppropriateRank(ArmsWarriorSpells::CHARGE, false);
        if (!charge || !CanCast(victim, charge, true))
            return false;

        if (ExecuteSpell(victim, charge, true))
        {
            chargeCooldown = CD_CHARGE;

            if (isDebugLogging)
                LOG_INFO("scripts", "[Bot: {}] 冲锋突进贴身 -> [{}] (距离 {} 码)", me->GetName(), victim->GetName(), dist);

            return true;
        }

        return false;
    }

    // =========================================================================
    // P2: 碎裂投掷 (破无敌 + 破甲爆发)
    // -------------------------------------------------------------------------
    // 触发条件有二：
    //   a) 目标持有圣盾术 / 保护祝福 / 冰箱等免疫光环 (不破盾则全家物理职业零伤害)；
    //   b) 首领目标尚未被我方护甲削弱易伤覆盖 (以其尚未携带任何 MOD_RESISTANCE_PCT
    //      护甲 Debuff 判定)，此时投掷可提供 20% 破甲爆发窗口。
    // =========================================================================
    bool TryShatteringThrow(Unit* victim)
    {
        if (shatteringThrowCooldown > 0 || !victim)
            return false;

        bool const breakImmunity = HasImmunityAura(victim);
        bool const bossNotVulnerable = IsBossTarget(victim) &&
                                       !victim->HasAuraTypeWithMiscvalue(SPELL_AURA_MOD_RESISTANCE_PCT, SPELL_SCHOOL_NORMAL);

        if (!breakImmunity && !bossNotVulnerable)
            return false;

        // 碎裂投掷为战斗姿态专属技能
        if (!me->HasAura(ArmsWarriorSpells::BATTLE_STANCE))
            return false;

        uint32 const shatteringThrow = GetAppropriateRank(ArmsWarriorSpells::SHATTERING_THROW, false);
        if (!shatteringThrow || !CanCast(victim, shatteringThrow, true))
            return false;

        // 非瞬发读条：CanCast 通过后才刹停立定 (铁律 7)，
        // 严禁在 CanCast 之前 StopMoving，否则走位重构期会被每帧拉扯成原地抽搐。
        if (me->isMoving())
            me->StopMoving();

        if (!ExecuteSpell(victim, shatteringThrow, true))
            return false;

        shatteringThrowCooldown = CD_SHATTERING_THROW;
        return true;
    }

    // =========================================================================
    // P2: 横扫攻击 (多目标溅射增益)
    // =========================================================================
    bool TrySweepingStrikes(Unit* victim)
    {
        if (sweepingStrikesCooldown > 0 || !victim)
            return false;

        if (CountNearbyEnemies(CLEAVE_RADIUS) < SWEEPING_MIN_TARGETS)
            return false;

        if (!me->HasAura(ArmsWarriorSpells::BATTLE_STANCE))
            return false;

        uint32 const sweepingStrikes = GetTalentRank(ArmsWarriorSpells::SWEEPING_STRIKES);
        if (!sweepingStrikes || me->HasAura(sweepingStrikes) || !CanCast(me, sweepingStrikes, true))
            return false;

        if (!ExecuteSpell(me, sweepingStrikes, true))
            return false;

        // 横扫攻击为瞬发增益技能，占 1.5s GCD：施放成功必须当帧交还决策流 (return true)，
        // 交由 gcdTimer 阻断后续施法，严禁同帧顺下与读条指令冲突导致动作丢帧。
        sweepingStrikesCooldown = CD_SWEEPING_STRIKES;
        return true;
    }

    // =========================================================================
    // P2: 利刃风暴 (多目标爆发大招)
    // -------------------------------------------------------------------------
    // 仅在致死打击处于 CD 期间开启：利刃风暴的自转期内严禁下发任何打击，
    // 若在致死就绪时开启会白吞一整发主力打击，得不偿失。
    // =========================================================================
    bool TryBladestorm(Unit* victim)
    {
        if (bladestormCooldown > 0 || !victim)
            return false;

        if (me->HasAura(ArmsWarriorSpells::BLADESTORM))
            return false;

        bool const multiTarget = IsBossTarget(victim) || CountNearbyEnemies(CLEAVE_RADIUS) >= SWEEPING_MIN_TARGETS;
        if (!multiTarget)
            return false;

        if (mortalStrikeCooldown == 0)
            return false;

        uint32 const bladestorm = GetTalentRank(ArmsWarriorSpells::BLADESTORM);
        if (!bladestorm || !CanCast(victim, bladestorm, true))
            return false;

        if (!ExecuteSpell(victim, bladestorm, true))
            return false;

        // 利刃风暴雕文将冷却由 1 分 30 秒缩短至 1 分 15 秒，必须随雕文动态结算，
        // 否则固定 90s 会平白吞掉 15 秒的可用窗口。
        bladestormCooldown = me->HasAura(ArmsWarriorSpells::GLYPH_OF_BLADESTORM)
                           ? CD_BLADESTORM_GLYPHED
                           : CD_BLADESTORM;

        if (isDebugLogging)
            LOG_INFO("scripts", "[Bot: {}] 开启利刃风暴爆发。", me->GetName());

        return true;
    }

    // =========================================================================
    // P3: 武器战核心打击 FCFS 循环 (First Come, First Served)
    // =========================================================================
    bool TryArmsRotation(Unit* victim)
    {
        if (!victim)
            return false;

        // ---- 1. 撕裂：维持血之气息底座 (100% 触发压制可用) ----
        if (TryRend(victim))
            return true;

        // ---- 2. 致死打击：主力打击 + 创伤联动 (-50% 受疗) ----
        if (TryMortalStrike(victim))
            return true;

        // ---- 3. 压制：血之气息触发时立即打出 (极高暴击，低怒耗) ----
        if (TryOverpower(victim))
            return true;

        // ---- 4. 斩杀：猝死触发或目标低血线的核弹填充 ----
        if (TryExecute(victim))
            return true;

        // ---- 5. 猛击：核心打击双 CD 期间的读条填充 ----
        return TrySlam(victim);
    }

    bool TryRend(Unit* victim)
    {
        if (!victim)
            return false;

        // 撕裂为分阶 DoT：以「分阶查询 + 自身施加」双重判定存在性，
        // 目标缺失自身撕裂或剩余 <= 1.5s 时立即补挂，维持血之气息的触发密度。
        if (GetOwnDotRemaining(victim, ArmsWarriorSpells::REND) > REND_REFRESH_WINDOW_MS)
            return false;

        uint32 const rend = GetAppropriateRank(ArmsWarriorSpells::REND, false);
        if (!rend || !CanCast(victim, rend, true))
            return false;

        return ExecuteSpell(victim, rend, true);
    }

    bool TryMortalStrike(Unit* victim)
    {
        if (mortalStrikeCooldown > 0 || !victim)
            return false;

        // 怒气门禁：致死打击消耗 30 怒，不足时严禁空放，必须留给压制/斩杀
        if (me->GetPower(POWER_RAGE) < RAGE_CORE_STRIKE_THRESHOLD)
            return false;

        uint32 const mortalStrike = GetTalentRank(ArmsWarriorSpells::MORTAL_STRIKE);
        if (!mortalStrike || !CanCast(victim, mortalStrike, true))
            return false;

        if (ExecuteSpell(victim, mortalStrike, true))
        {
            mortalStrikeCooldown = CD_MORTAL_STRIKE;
            return true;
        }

        return false;
    }

    bool TryOverpower(Unit* victim)
    {
        if (!victim)
            return false;

        // 压制仅在【血之气息】触发光环存在时可用 (目标招架/闪避亦可，但脚本站桩无此收益)
        if (!me->HasAura(ArmsWarriorSpells::AURA_TASTE_FOR_BLOOD))
            return false;

        uint32 const overpower = GetAppropriateRank(ArmsWarriorSpells::OVERPOWER, false);
        if (!overpower || !CanCast(victim, overpower, true))
            return false;

        return ExecuteSpell(victim, overpower, true);
    }

    bool TryExecute(Unit* victim)
    {
        if (!victim)
            return false;

        // 触发通道：【猝死】光环 (保留 10 怒的免费斩杀) 或目标进入 20% 斩杀期。
        // 二者任一成立即交出，猝死窗口极短，绝不能等血线。
        bool const suddenDeath = me->HasAura(ArmsWarriorSpells::AURA_SUDDEN_DEATH);
        if (!suddenDeath && victim->GetHealthPct() >= EXECUTE_PHASE_HP_PCT)
            return false;

        uint32 const execute = GetAppropriateRank(ArmsWarriorSpells::EXECUTE, false);
        if (!execute || !CanCast(victim, execute, true))
            return false;

        return ExecuteSpell(victim, execute, true);
    }

    bool TrySlam(Unit* victim)
    {
        if (!victim)
            return false;

        // 强化猛击 (12862) 将猛击读条压至 0.5s。未注入该被动时基础 1.5s 全额站桩
        // 会挤掉白字平砍产怒与找背走位 (还会被移动指令随时掐断)，故直接放弃填充。
        if (!me->HasAura(ArmsWarriorSpells::IMPROVED_SLAM))
            return false;

        // 怒气门禁：猛击消耗 15 怒，不足 30 怒时严禁抢占致死/压制的怒气预算
        if (me->GetPower(POWER_RAGE) < RAGE_CORE_STRIKE_THRESHOLD)
            return false;

        // 仅当致死打击与压制双双不可用时才作为填充
        if (mortalStrikeCooldown == 0 || me->HasAura(ArmsWarriorSpells::AURA_TASTE_FOR_BLOOD))
            return false;

        uint32 const slam = GetAppropriateRank(ArmsWarriorSpells::SLAM, false);
        if (!slam || !CanCast(victim, slam, true))
            return false;

        // 非瞬发读条：CanCast 通过后才刹停立定 (铁律 7)，
        // 严禁在 CanCast 之前 StopMoving，否则走位重构期会被每帧拉扯成原地抽搐。
        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, slam, true);
    }

    // =========================================================================
    // P3.5: 平砍队列控怒 (英勇打击 / 顺劈斩，铁律 26)
    // -------------------------------------------------------------------------
    // 二者均为 on-next-swing 队列类技能，不占 GCD，可以在主循环之后独立下发，
    // 与 FCFS 打击天然并行而不互相挤压 GCD。
    // 65 怒气门禁：低于该线时严禁泄怒，必须把每一滴怒气留给致死打击与斩杀。
    // =========================================================================
    bool TryRageDumpQueue(Unit* victim)
    {
        if (!victim)
            return false;

        // 斩杀期把怒气全部让渡给致死打击与斩杀，严禁用英勇打击偷跑怒气导致斩杀断档
        if (victim->GetHealthPct() < EXECUTE_PHASE_HP_PCT)
            return false;

        // 猝死触发时，必须将所有怒气让渡给这发免费斩杀，
        // 严禁此时执行泄怒排队把斩杀所需怒气预先烧掉。
        if (me->HasAura(ArmsWarriorSpells::AURA_SUDDEN_DEATH))
            return false;

        if (me->GetPower(POWER_RAGE) < RAGE_DUMP_THRESHOLD)
            return false;

        // 队列节流：on-next-swing 技能不占 GCD，但引擎仍会吞并重复下发，
        // 必须按平砍周期节流，否则每帧重入等于把怒气全部烧在一个等待队列上。
        if (heroicStrikeCooldown > 0)
            return false;

        // 多目标 (>= 2) 走顺劈斩，否则用英勇打击
        if (CountNearbyEnemies(CLEAVE_RADIUS) >= SWEEPING_MIN_TARGETS)
        {
            uint32 const cleave = GetAppropriateRank(ArmsWarriorSpells::CLEAVE, false);
            if (cleave && CanCast(victim, cleave, true) && ExecuteSpell(victim, cleave, true))
            {
                heroicStrikeCooldown = CD_HEROIC_STRIKE_QUEUE;
                return true;
            }
        }

        uint32 const heroicStrike = GetAppropriateRank(ArmsWarriorSpells::HEROIC_STRIKE, false);
        if (heroicStrike && CanCast(victim, heroicStrike, true) && ExecuteSpell(victim, heroicStrike, true))
        {
            heroicStrikeCooldown = CD_HEROIC_STRIKE_QUEUE;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P1: 姿态维持
    // -------------------------------------------------------------------------
    // 武器战全部核心打击 (致死/压制/撕裂/利刃风暴/碎裂投掷/反击风暴) 均要求战斗姿态。
    // 严禁沿用狂暴战的狂暴姿态：那会让整条致死打击链路被底层直接拒绝。
    // =========================================================================
    bool MaintainStance()
    {
        if (me->HasAura(ArmsWarriorSpells::BATTLE_STANCE))
            return false;

        // 姿态光环共用 SPELL_SPECIFIC_PRESENCE 互斥组，新姿态会自动顶掉旧姿态，
        // 无需手动摘除，避免多一步 RemoveAurasDueToSpell 造成姿态空窗。
        if (!CanCast(me, ArmsWarriorSpells::BATTLE_STANCE, true))
            return false;

        return ExecuteSpell(me, ArmsWarriorSpells::BATTLE_STANCE, true);
    }

    // =========================================================================
    // P1: 战斗怒吼 (攻强增益)
    // =========================================================================
    bool MaintainBattleShout()
    {
        uint32 const battleShout = GetAppropriateRank(ArmsWarriorSpells::BATTLE_SHOUT, false);
        if (!battleShout)
            return false;

        // 分阶光环判定：等级同步或团队中其他战士已挂低阶怒吼时，
        // 若只按当前 Rank ID 精确匹配会判定为缺失并重复顶替，白白烧掉一次 GCD，
        // 故按法术链全阶查询，任意 Rank 的同名怒吼均视为已覆盖。
        if (me->GetAuraOfRankedSpell(ArmsWarriorSpells::BATTLE_SHOUT))
            return false;

        // 战吼消耗 10 怒，怒气不足时严禁补吼，必须把怒气留给致死打击的伤害预算。
        // 注：脱战期怒气自然衰减为 0，故战吼实际在进战怒气充盈后才完成补挂。
        if (me->GetPower(POWER_RAGE) < RAGE_SHOUT_THRESHOLD)
            return false;

        // 重试节流：圣骑士力量祝福 (BoM) 等同类 AP 增益覆盖时底层会拒绝施放怒吼，
        // 无节流守卫会在脱战分支每一帧重新尝试并被拒绝，空转烧掉决策流与 GCD。
        if (battleShoutRetryTimer > 0)
            return false;

        battleShoutRetryTimer = BATTLE_SHOUT_RETRY_MS;

        if (!CanCast(me, battleShout, true))
            return false;

        return ExecuteSpell(me, battleShout, true);
    }

    // =========================================================================
    // P1: 战时血性狂暴补怒 (Off-GCD，顺下不 return)
    // -------------------------------------------------------------------------
    // 仅在怒气见底时补怒：怒气充裕时照旧交技能等于白烧 1 分钟 CD，
    // 真正需要续怒气打致死/斩杀时反而无牌可打。
    // =========================================================================
    void TryBloodrage()
    {
        if (bloodrageCooldown > 0)
            return;

        if (me->GetPower(POWER_RAGE) >= RAGE_LOW_THRESHOLD)
            return;

        uint32 const bloodrage = GetAppropriateRank(ArmsWarriorSpells::BLOODRAGE, false);
        if (bloodrage && !me->HasAura(bloodrage) &&
            CanCast(me, bloodrage, true) && ExecuteSpell(me, bloodrage, true))
            bloodrageCooldown = CD_BLOODRAGE;
    }

    // =========================================================================
    // P4: 物理近战背后找背站位模型 (铁律 16)
    // =========================================================================
    void MaintainMeleeBehindPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        // 读条守卫 (铁律 1)：猛击/碎裂投掷等读条期间严禁下发任何走位指令，
        // 否则同帧的 MoveFollow/MoveChase 会把刚起手的读条当帧掐断，
        // 表现为技能反复起手却永不落地、白烧 GCD 与怒气。
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        me->SetFacingToObject(victim);

        float const dist = me->GetDistance(victim);
        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
        bool const isFollowing = (moveType == FOLLOW_MOTION_TYPE);
        bool const isChasing   = (moveType == CHASE_MOTION_TYPE);

        // 怪物仇恨锚定随从本人：此时严禁绕后找背身位。
        // 因为怪会随随从的移动实时转向，绕后指令会让两者围绕同一圆心无限对转，
        // 形成「旋转木马」同心圆死锁，随从全程贴不上背且一发技能打不出。
        bool const mobTargetingMe = (victim->GetVictim() == me);

        if (mobTargetingMe)
        {
            // 怪看随从时只求贴身：用 MoveChase 直线贴上去，不追求背后位
            if (!isChasing || dist > MELEE_REACH_DIST)
                me->GetMotionMaster()->MoveChase(victim, MELEE_FOLLOW_DIST);

            return;
        }

        // ---- 已平滑贴身：保持贴背输出，不打断平砍节奏 ----
        if (isFollowing && dist <= MELEE_COMFORT_DIST)
            return;

        // ---- 怪物盯防主坦：严格占住目标背身位，规避正面顺劈/吐息与被招架加速 ----
        // angle 必须传正后方 BEHIND_ANGLE (M_PI)：引擎内部已按目标当前朝向结算偏移，
        // 严禁自行叠加 victim->GetOrientation()，否则站位会随目标转向持续漂移；
        // 传 0.0f 会贴在 Boss 脸前吃顺劈与正面吐息，物理近战必须始终占住背后位。
        if (dist > MELEE_REACH_DIST || !isFollowing)
        {
            me->GetMotionMaster()->MoveFollow(victim, MELEE_FOLLOW_DIST, BEHIND_ANGLE);
        }
    }

    // =========================================================================
    // 武器天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷，铁律 33 满阶 Rank 注入)
    // -------------------------------------------------------------------------
    // 注入等级取各天赋在武器树中的近似前置门槛；其中：
    //   * TASTE_FOR_BLOOD (56638, Rank 3) 是【血之气息】触发光环 60503 的唯一生效前提，
    //     缺失则压制通道永久报废 (撕裂跳数不会刷新压制可用性)；
    //   * SUDDEN_DEATH (29725, Rank 3) 是【猝死】触发光环 52437 的唯一生效前提；
    //   * JUGGERNAUT (64976) 解锁战时冲锋与冲锋后 25% 暴击加成。
    // 三者必须在低等级段即注入，否则整条核心打击链会静默退化。
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

        SyncPassive(20, ArmsWarriorSpells::TASTE_FOR_BLOOD);        // 血之气息 Rank 3：撕裂跳数 100% 触发压制可用
        SyncPassive(20, ArmsWarriorSpells::SUDDEN_DEATH);           // 猝死 Rank 3：近战命中 9% 触发免费斩杀
        SyncPassive(20, ArmsWarriorSpells::IMPROVED_SLAM);          // 强化猛击 Rank 2：猛击读条压至 0.5s
        SyncPassive(20, ArmsWarriorSpells::DEEP_WOUNDS);            // 重伤 Rank 3：暴击附带 48% 武器伤害流血
        SyncPassive(20, ArmsWarriorSpells::TRAUMA);                 // 创伤 Rank 2：暴击提升目标所受流血伤害 30%
        SyncPassive(20, ArmsWarriorSpells::UNRELENTING_ASSAULT);    // 无坚不摧 Rank 2：压制冷却 -4s 并压制目标法强/治疗
        SyncPassive(20, ArmsWarriorSpells::TWO_HANDED_WEAPON_SPEC); // 双手武器专精 Rank 5：双手武器物理伤害 +10%
        SyncPassive(30, ArmsWarriorSpells::JUGGERNAUT);             // 主宰 Rank 1：解锁战时冲锋与冲锋后暴击加成
        SyncPassive(20, ArmsWarriorSpells::GLYPH_OF_MORTAL_STRIKE); // 致死打击雕文：致死打击伤害 +10%
        SyncPassive(20, ArmsWarriorSpells::GLYPH_OF_REND);          // 撕裂雕文：撕裂持续时间延长 6s
        SyncPassive(20, ArmsWarriorSpells::GLYPH_OF_BLADESTORM);    // 利刃风暴雕文：冷却缩短 15s
        SyncPassive(20, ArmsWarriorSpells::GLYPH_OF_EXECUTION);     // 斩杀雕文：斩杀按额外 10 怒结算伤害
    }
};

void AddSC_bot_arms_warrior()
{
    new AdaptiveBotScript<BotArmsWarriorAI>("bot_arms_warrior");
}
