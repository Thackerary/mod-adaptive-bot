/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "FuryWarriorSpells.h"
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
#include <vector>

class BotFuryWarriorAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位参数 (物理近战背后找背模型)
    // =========================================================================
    static constexpr float MELEE_REACH_DIST   = 4.0f;   // 近战判定区上限：超出必须重新贴背
    static constexpr float MELEE_COMFORT_DIST = 3.0f;   // 贴身阈值：进入后保持平滑贴背输出
    static constexpr float MELEE_FOLLOW_DIST  = 1.5f;   // 理想站位：目标正后方 1.5 码

    // MoveFollow 的 angle 为「相对目标朝向的偏移」，引擎内部已叠加目标朝向。
    // 严禁自行叠加 victim->GetOrientation()，否则站位会随目标转向持续漂移。
    // M_PI 即目标正后方：背身位，可规避正面顺劈、吐息以及被招架加速。
    static constexpr float BEHIND_ANGLE       = static_cast<float>(M_PI);

    // 顺劈斩多目标判定
    static constexpr float  CLEAVE_RADIUS     = 8.0f;
    static constexpr uint32 CLEAVE_MIN_TARGETS = 2;

    // =========================================================================
    // 怒气池语义
    // -------------------------------------------------------------------------
    // 引擎内部怒气为 10 倍定点存储 (1000 == 100 怒气)，与 CanCast 中的
    // `if (PowerType == POWER_RAGE) cost *= 10;` 完全对齐。
    // =========================================================================
    static constexpr uint32 MAX_RAGE            = 1000;
    static constexpr uint32 RAGE_LOW_THRESHOLD  = 200;   // 20 怒气：血性狂暴补怒线
    static constexpr uint32 RAGE_DUMP_THRESHOLD = 500;   // 50 怒气：英勇打击/顺劈斩泄怒线

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪，凡无「持续光环保护」的 CD 技能必须由专精自行计时，
    // 否则 HasSpellCooldown 恒 false 会在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_BLOODTHIRST          = 4000;
    static constexpr uint32 CD_WHIRLWIND            = 5000;
    static constexpr uint32 CD_BLOODRAGE            = 60000;
    static constexpr uint32 CD_BERSERKER_RAGE       = 30000;
    static constexpr uint32 CD_DEATH_WISH           = 180000;
    static constexpr uint32 CD_RECKLESSNESS         = 300000;
    static constexpr uint32 CD_ENRAGED_REGENERATION = 180000;
    static constexpr uint32 CD_PUMMEL               = 10000;

    // =========================================================================
    // 血线与阶段阈值
    // =========================================================================
    static constexpr float ENRAGED_REGENERATION_HP_PCT = 30.0f;  // 狂暴回复自救血线
    static constexpr float BURST_TARGET_HP_PCT         = 50.0f;  // 目标高血量爆发窗口
    static constexpr float EXECUTE_PHASE_HP_PCT        = 20.0f;  // 斩杀期血线

    // 狂暴姿态解锁等级：低于该等级无狂暴姿态可用，回退战斗姿态
    static constexpr uint8 BERSERKER_STANCE_MIN_LEVEL = 30;

public:
    explicit BotFuryWarriorAI(Creature* creature) : AdaptiveBotAI(creature) {}

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
    // 注：训练师基础技能 (嗜血以外的打击、鲁莽、血性狂暴、狂怒回复、战斗怒吼、
    //     拳击、拦截等) 严禁登记于此，其等级门槛由 GetAppropriateRank 依据
    //     DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case FuryWarriorSpells::DEATH_WISH: return 30;   // 死亡之愿：纯天赋，DBC SpellLevel 恒为 0
            case FuryWarriorSpells::BLOODTHIRST: return 40;  // 嗜血：狂暴系 31 点天赋
            default:                            return 0;
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
        ResetWarriorTimers();
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
        UpdateWarriorTimers(diff);

        // 全局读条/通道双保险守卫：引导类法术在部分状态下并不置位 UNIT_STATE_CASTING，
        // 故追加 CURRENT_CHANNELED_SPELL 显式判定，杜绝读条与引导被跟随/走位指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护 (姿态 + 战斗怒吼 + 跟随)
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

        // 物理近战开怪：第二参数必须传 true 开启双持白字平砍循环
        // (随从怒气主要来源即为平砍命中，与远程专精的 me->Attack(victim, false) 严格区分)。
        if (me->GetVictim() != victim)
            me->Attack(victim, true);

        // ---- P0: 恐惧/瘫痪脱困 ----
        if (TryBreakIncapacitate())
            return;

        // ---- P0: 极限自保 (狂暴回复 + 激怒前置) ----
        if (TrySurvival())
            return;

        // ---- P1: 姿态与常驻增益 ----
        if (MaintainStance())
            return;

        if (MaintainBattleShout())
            return;

        // 战时血性狂暴补怒 (Off-GCD，顺下不 return)
        TryBloodrage();

        // ---- P2: 双爆发与拳击打断 (Off-GCD，顺下不 return) ----
        TryBurstAndInterrupt(victim);

        // ---- P3: 狂暴 FCFS 打击循环 (斩杀 > 嗜血 > 旋风斩 > 血涌瞬发猛击) ----
        TryFuryRotation(victim);

        // ---- P3.5: 平砍队列控怒 (50 怒气门禁，on-next-swing 不占 GCD) ----
        TryRageDumpQueue(victim);

        // ---- P4: 背后找背站位 ----
        MaintainMeleeBehindPositioning(victim);

        // ---- 双持白字平砍驱动 ----
        // ScriptedAI::UpdateAI 已被本类完整接管，引擎不会自动驱动平砍，
        // 必须在决策流末帧显式调用，否则双持白字伤害与平砍产怒永久缺失。
        DoMeleeAttackIfReady();
    }

private:
    // 自管冷却登记
    uint32 bloodthirstCooldown{ 0 };
    uint32 whirlwindCooldown{ 0 };
    uint32 bloodrageCooldown{ 0 };
    uint32 berserkerRageCooldown{ 0 };
    uint32 deathWishCooldown{ 0 };
    uint32 recklessnessCooldown{ 0 };
    uint32 enragedRegenerationCooldown{ 0 };
    uint32 pummelCooldown{ 0 };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateWarriorTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(bloodthirstCooldown);
        Tick(whirlwindCooldown);
        Tick(bloodrageCooldown);
        Tick(berserkerRageCooldown);
        Tick(deathWishCooldown);
        Tick(recklessnessCooldown);
        Tick(enragedRegenerationCooldown);
        Tick(pummelCooldown);
    }

    void ResetWarriorTimers()
    {
        bloodthirstCooldown = 0;
        whirlwindCooldown = 0;
        bloodrageCooldown = 0;
        berserkerRageCooldown = 0;
        deathWishCooldown = 0;
        recklessnessCooldown = 0;
        enragedRegenerationCooldown = 0;
        pummelCooldown = 0;
    }

    // =========================================================================
    // 天赋契约等级门禁
    // -------------------------------------------------------------------------
    // 3.3.5a 中天赋法术的 DBC SpellLevel 恒为 0，GetAppropriateRank 不会因等级而降阶，
    // 低等级下依旧返回最高 Rank 的 ID。若不加门禁，低等级随从会持续尝试死搓嗜血/死亡之愿。
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
    bool IsInterruptibleTarget(Unit* target) const
    {
        if (!target || !target->IsAlive())
            return false;

        if (target->HasUnitState(UNIT_STATE_CASTING))
            return true;

        // 引导类法术不置位 UNIT_STATE_CASTING，需按通道中断标记单独判定可打断性
        if (Spell* channeled = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return channeled->GetSpellInfo()->ChannelInterruptFlags != 0;

        return false;
    }

    bool IsEliteOrBossTarget(Unit* target) const
    {
        if (!target)
            return false;

        // PvP 场景直接视同精英
        if (target->IsPlayer())
            return true;

        if (Creature* creature = target->ToCreature())
            return creature->GetCreatureTemplate()->rank >= CREATURE_ELITE_ELITE;

        return false;
    }

    // 多目标雷达：统计「自身 / 主坦 / 指挥官」三点仇恨链上贴身 8 码内的敌对单位数量，
    // 供顺劈斩与旋风斩的多目标收益判定使用 (含去重，避免同一怪物被重复计数)。
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

    // =========================================================================
    // P0: 恐惧/瘫痪脱困
    // -------------------------------------------------------------------------
    // 狂暴之怒可解除恐惧、闷棍与瘫痪。注意基类 CanCast 在受控状态下会主动阻断
    // (UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING 直接 return false)，
    // 故此处必须走底层 CastSpell 直放通道，否则脱困技能永远出不了手。
    // =========================================================================
    bool TryBreakIncapacitate()
    {
        if (berserkerRageCooldown > 0)
            return false;

        if (!me->HasUnitState(UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED))
            return false;

        uint32 const berserkerRage = GetAppropriateRank(FuryWarriorSpells::BERSERKER_RAGE, false);
        if (!berserkerRage)
            return false;

        if (me->CastSpell(me, berserkerRage, false) == SPELL_CAST_OK)
        {
            berserkerRageCooldown = CD_BERSERKER_RAGE;

            if (isDebugLogging)
                LOG_INFO("scripts", "[Bot: {}] 狂暴之怒脱困成功。", me->GetName());

            return true;
        }

        return false;
    }

    // =========================================================================
    // P0: 极限自保 (狂暴回复)
    // -------------------------------------------------------------------------
    // 狂暴回复为「需要激怒状态」的强力回血底牌。若底层因缺少激怒而拒绝施放，
    // 当帧立即补一道激怒前置 (狂暴之怒 > 血性狂暴)，绝不把 3 分钟 CD 空烧在等待上。
    // =========================================================================
    bool TrySurvival()
    {
        if (enragedRegenerationCooldown > 0 || me->GetHealthPct() >= ENRAGED_REGENERATION_HP_PCT)
            return false;

        uint32 const enragedRegeneration = GetAppropriateRank(FuryWarriorSpells::ENRAGED_REGENERATION, false);
        if (!enragedRegeneration || me->HasAura(enragedRegeneration) || !CanCast(me, enragedRegeneration, true))
            return false;

        if (ExecuteSpell(me, enragedRegeneration, true))
        {
            enragedRegenerationCooldown = CD_ENRAGED_REGENERATION;
            return true;
        }

        // 底层拒绝施法 (缺少激怒状态)：当帧立即补激怒前置，下一帧再吃狂暴回复
        return TryTriggerEnrage();
    }

    // 激怒前置源：狂暴之怒 (顺带解控、冷却更短) 优先，血性狂暴兜底
    bool TryTriggerEnrage()
    {
        if (berserkerRageCooldown == 0)
        {
            uint32 const berserkerRage = GetAppropriateRank(FuryWarriorSpells::BERSERKER_RAGE, false);
            if (berserkerRage && !me->HasAura(berserkerRage) &&
                CanCast(me, berserkerRage, true) && ExecuteSpell(me, berserkerRage, true))
            {
                berserkerRageCooldown = CD_BERSERKER_RAGE;
                return true;
            }
        }

        if (bloodrageCooldown == 0)
        {
            uint32 const bloodrage = GetAppropriateRank(FuryWarriorSpells::BLOODRAGE, false);
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
    // P1: 姿态维持
    // -------------------------------------------------------------------------
    // 30 级及以上常驻狂暴姿态 (解锁嗜血/旋风斩/拦截/死亡之愿并附带命中加成)，
    // 低于 30 级平滑回退战斗姿态，保证任何等级段的随从都有合法姿态基底。
    // =========================================================================
    bool MaintainStance()
    {
        uint32 const desiredStance = (me->GetLevel() >= BERSERKER_STANCE_MIN_LEVEL)
                                   ? FuryWarriorSpells::BERSERKER_STANCE
                                   : FuryWarriorSpells::BATTLE_STANCE;

        if (me->HasAura(desiredStance))
            return false;

        // 姿态光环共用 SPELL_SPECIFIC_PRESENCE 互斥组，新姿态会自动顶掉旧姿态，
        // 无需手动摘除，避免多一步 RemoveAurasDueToSpell 造成姿态空窗。
        if (!CanCast(me, desiredStance, true))
            return false;

        return ExecuteSpell(me, desiredStance, true);
    }

    // =========================================================================
    // P1: 战斗怒吼
    // =========================================================================
    bool MaintainBattleShout()
    {
        uint32 const battleShout = GetAppropriateRank(FuryWarriorSpells::BATTLE_SHOUT, false);
        if (!battleShout)
            return false;

        // 怒吼光环与施法法术共用同一 Rank ID，直接按该 Rank 判定存在性即可
        if (me->HasAura(battleShout))
            return false;

        if (!CanCast(me, battleShout, true))
            return false;

        return ExecuteSpell(me, battleShout, true);
    }

    // =========================================================================
    // P1: 战时血性狂暴补怒 (Off-GCD，顺下不 return)
    // -------------------------------------------------------------------------
    // 仅在怒气见底时补怒：怒气充裕时照旧交技能等于白烧 1 分钟 CD，
    // 真正需要续怒气打斩杀时反而无牌可打。
    // =========================================================================
    void TryBloodrage()
    {
        if (bloodrageCooldown > 0)
            return;

        if (me->GetPower(POWER_RAGE) >= RAGE_LOW_THRESHOLD)
            return;

        uint32 const bloodrage = GetAppropriateRank(FuryWarriorSpells::BLOODRAGE, false);
        if (bloodrage && !me->HasAura(bloodrage) &&
            CanCast(me, bloodrage, true) && ExecuteSpell(me, bloodrage, true))
            bloodrageCooldown = CD_BLOODRAGE;
    }

    // =========================================================================
    // P2: 双爆发与拳击打断 (Off-GCD，当帧顺下绝不 return)
    // =========================================================================
    void TryBurstAndInterrupt(Unit* victim)
    {
        if (!victim)
            return;

        // 爆发窗口：首领/精英且仍处于 50% 以上血量 (高血量窗口才值得交底牌)
        bool const burstWindow = IsEliteOrBossTarget(victim) && victim->GetHealthPct() > BURST_TARGET_HP_PCT;

        // ---- 死亡之愿：纯天赋 (30 级解锁)，20% 增伤换取 5% 受疗惩罚 ----
        if (burstWindow && deathWishCooldown == 0)
        {
            uint32 const deathWish = GetTalentRank(FuryWarriorSpells::DEATH_WISH);
            if (deathWish && !me->HasAura(deathWish) &&
                CanCast(me, deathWish, true) && ExecuteSpell(me, deathWish, true))
                deathWishCooldown = CD_DEATH_WISH;

            // Off-GCD 铁律：严禁在此 return，必须允许当帧决策流顺下，
            // 让嗜血/旋风斩立即吃满这 20% 增伤。
        }

        // ---- 鲁莽：100% 暴击率窗口，需狂暴姿态 ----
        if (burstWindow && recklessnessCooldown == 0)
        {
            uint32 const recklessness = GetAppropriateRank(FuryWarriorSpells::RECKLESSNESS, false);
            if (recklessness && !me->HasAura(recklessness) &&
                CanCast(me, recklessness, true) && ExecuteSpell(me, recklessness, true))
                recklessnessCooldown = CD_RECKLESSNESS;
        }

        // ---- 拳击：目标读条/引导时近战瞬发打断 (狂暴姿态) ----
        if (pummelCooldown == 0 && IsInterruptibleTarget(victim))
        {
            uint32 const pummel = GetAppropriateRank(FuryWarriorSpells::PUMMEL, false);
            if (pummel && CanCast(victim, pummel, true) && ExecuteSpell(victim, pummel, true))
                pummelCooldown = CD_PUMMEL;
        }
    }

    // =========================================================================
    // P3: 狂暴 FCFS 近战伤害循环 (First Come, First Served)
    // -------------------------------------------------------------------------
    // 全部技能均为瞬发，严禁下发 StopMoving，保障边跑边打的机动性；
    // 命中技能后仍需继续驱动平砍与站位，故本函数返回值不阻断主决策流。
    // =========================================================================
    bool TryFuryRotation(Unit* victim)
    {
        if (!victim)
            return false;

        // ---- 1. 斩杀：目标 20% 以下优先泄怒斩杀 ----
        if (TryExecute(victim))
            return true;

        // ---- 2. 嗜血：核心伤害 + 自我治疗 ----
        if (TryBloodthirst(victim))
            return true;

        // ---- 3. 旋风斩：多目标武器伤害 ----
        if (TryWhirlwind(victim))
            return true;

        // ---- 4. 血涌瞬发猛击：填充 ----
        return TryBloodsurgeSlam(victim);
    }

    bool TryExecute(Unit* victim)
    {
        if (!victim || victim->GetHealthPct() >= EXECUTE_PHASE_HP_PCT)
            return false;

        // 斩杀无自管冷却：每帧数量受 GCD 与 30 怒气消费天然约束，无需额外节流
        uint32 const execute = GetAppropriateRank(FuryWarriorSpells::EXECUTE, false);
        if (!execute || !CanCast(victim, execute, true))
            return false;

        return ExecuteSpell(victim, execute, true);
    }

    bool TryBloodthirst(Unit* victim)
    {
        if (bloodthirstCooldown > 0 || !victim)
            return false;

        uint32 const bloodthirst = GetTalentRank(FuryWarriorSpells::BLOODTHIRST);
        if (!bloodthirst || !CanCast(victim, bloodthirst, true))
            return false;

        if (ExecuteSpell(victim, bloodthirst, true))
        {
            bloodthirstCooldown = CD_BLOODTHIRST;
            return true;
        }

        return false;
    }

    bool TryWhirlwind(Unit* victim)
    {
        if (whirlwindCooldown > 0 || !victim)
            return false;

        uint32 const whirlwind = GetAppropriateRank(FuryWarriorSpells::WHIRLWIND, false);
        if (!whirlwind || !CanCast(victim, whirlwind, true))
            return false;

        if (ExecuteSpell(victim, whirlwind, true))
        {
            whirlwindCooldown = CD_CD_WHIRLWIND_PLACEHOLDER;
            return true;
        }

        return false;
    }

    bool TryBloodsurgeSlam(Unit* victim)
    {
        if (!victim)
            return false;

        // APL 契约：猛击只允许在【血涌】(46916) 触发时瞬发交出。
        // 无光环时若强行施放，底层会强制 1.5 秒站桩读条，直接掐断双持白字产怒与找背走位，
        // 并让随从在原位吃满顺劈与吐息，故无光环一律直接放弃，绝不硬读条。
        if (!me->HasAura(FuryWarriorSpells::AURA_BLOODSURGE))
            return false;

        uint32 const slam = GetAppropriateRank(FuryWarriorSpells::SLAM, false);
        if (!slam || !CanCast(victim, slam, true))
            return false;

        return ExecuteSpell(victim, slam, true);
    }

    // =========================================================================
    // P3.5: 平砍队列控怒 (英勇打击 / 顺劈斩)
    // -------------------------------------------------------------------------
    // 二者均为 on-next-swing 队列类技能，不占 GCD，因此可以在主循环之后独立下发，
    // 与 FCFS 打击天然并行而不互相挤压 GCD。
    // 50 怒气门禁：低于该线时严禁泄怒，必须把每一滴怒气留给嗜血/旋风斩/斩杀。
    // =========================================================================
    bool TryRageDumpQueue(Unit* victim)
    {
        if (!victim)
            return false;

        // 斩杀期把怒气全部让渡给斩杀，严禁用英勇打击偷跑怒气导致斩杀断档
        if (victim->GetHealthPct() < EXECUTE_PHASE_HP_PCT)
            return false;

        if (me->GetPower(POWER_RAGE) < RAGE_DUMP_THRESHOLD)
            return false;

        // 多目标 (>= 2) 走顺劈斩，否则用英勇打击
        if (CountNearbyEnemies(CLEAVE_RADIUS) >= CLEAVE_MIN_TARGETS)
        {
            uint32 const cleave = GetAppropriateRank(FuryWarriorSpells::CLEAVE, false);
            if (cleave && CanCast(victim, cleave, true) && ExecuteSpell(victim, cleave, true))
                return true;
        }

        uint32 const heroicStrike = GetAppropriateRank(FuryWarriorSpells::HEROIC_STRIKE, false);
        if (heroicStrike && CanCast(victim, heroicStrike, true) && ExecuteSpell(victim, heroicStrike, true))
            return true;

        return false;
    }

    // =========================================================================
    // P4: 物理近战背后找背站位模型
    // =========================================================================
    void MaintainMeleeBehindPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
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
    // 狂暴天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
    // -------------------------------------------------------------------------
    // 注入等级取各天赋在狂暴树中的近似前置门槛，保证任何等级段都有可用被动；
    // 其中 BLOODSURGE (46915) 是【血涌】触发光环 46916 的唯一生效前提，
    // 缺失该被动则瞬发猛击通道永久报废。
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

        SyncPassive(20, FuryWarriorSpells::GLYPH_OF_WHIRLWIND);     // 旋风斩雕文：旋风斩伤害 +10%
        SyncPassive(20, FuryWarriorSpells::GLYPH_OF_HEROIC_STRIKE); // 英勇打击雕文：英勇打击暴击率 +5%
        SyncPassive(30, FuryWarriorSpells::FLURRY);                 // 乱舞：暴击后叠加攻速
        SyncPassive(40, FuryWarriorSpells::BLOODSURGE);             // 血涌：保障 46916 触发 (瞬发猛击解锁通道)
        SyncPassive(45, FuryWarriorSpells::UNENDING_FURY);          // 无尽怒气：击杀后回怒
        SyncPassive(50, FuryWarriorSpells::RAMPAGE);                // 暴怒：击杀后叠加 AP 增益
        SyncPassive(60, FuryWarriorSpells::TITANS_GRIP);            // 泰坦之握：双持双手武器
    }
};

void AddSC_bot_fury_warrior()
{
    new AdaptiveBotScript<BotFuryWarriorAI>("bot_fury_warrior");
}
