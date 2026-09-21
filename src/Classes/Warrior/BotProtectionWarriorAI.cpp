/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "ProtectionWarriorSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "SpellAuras.h"

class BotProtectionWarriorAI : public AdaptiveBotAI
{
public:
    explicit BotProtectionWarriorAI(Creature* creature) : AdaptiveBotAI(creature) {}

    bool IsTankBot() const override { return true; }

    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case ProtectionWarriorSpells::LAST_STAND:
                return 20;
            case ProtectionWarriorSpells::CONCUSSION_BLOW:
                return 30;
            case ProtectionWarriorSpells::SHIELD_SLAM:
                return 40;
            case ProtectionWarriorSpells::DEVASTATE:
                return 50;
            case ProtectionWarriorSpells::SHOCKWAVE:
                return 60;
            default:
                return 0;
        }
    }

    void Reset() override
    {
        AdaptiveBotAI::Reset();
        shoutCheckTimer = 0;
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 /*level*/) override
    {
        ApplyPassiveTalents();
    }

    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);
        UpdateWarriorTimers(diff);

        // 维护团队怒吼计时器 (战斗内外通用)
        if (shoutCheckTimer <= diff)
            shoutCheckTimer = 3000;
        else
            shoutCheckTimer -= diff;

        // 1. 脱战业务维护
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            UpdateFollowMaster(diff);

            if (shoutCheckTimer == 3000)
                MaintainTeamShout(); 

            if (me->GetLevel() >= 10 && !me->HasAura(ProtectionWarriorSpells::DEFENSIVE_STANCE)) 
            {
                if (CanCast(me, ProtectionWarriorSpells::DEFENSIVE_STANCE, true)) 
                    ExecuteSpell(me, ProtectionWarriorSpells::DEFENSIVE_STANCE); 
            }
            return;
        }

        // 2. 索敌仲裁
        Unit* victim = SelectAssistTarget(); 
        if (!victim || !victim->IsAlive()) 
            return;

        // 3. 近战平砍与追击状态机接管
        ManageMeleeCombat(victim); 

        // =====================================================================
        // Action Priority List (APL) 核心决策循环
        // =====================================================================

        // ---------------------------------------------------------------------
        // P0: 核心姿态维护与怒气唤醒
        // ---------------------------------------------------------------------
        if (me->GetLevel() >= 10 && !me->HasAura(ProtectionWarriorSpells::DEFENSIVE_STANCE)) 
        {
            if (CanCast(me, ProtectionWarriorSpells::DEFENSIVE_STANCE, true)) 
            {
                ExecuteSpell(me, ProtectionWarriorSpells::DEFENSIVE_STANCE); 
                return;
            }
        }

        if (me->GetPower(POWER_RAGE) < 150) 
        {
            if (CanCast(me, ProtectionWarriorSpells::BLOODRAGE, false)) 
                ExecuteSpell(me, ProtectionWarriorSpells::BLOODRAGE, false); 
        }

        // ---------------------------------------------------------------------
        // P1: 硬核减伤链与生死急救
        // ---------------------------------------------------------------------
        float const hpPct = me->GetHealthPct(); 

        // 生命 < 25%：破釜沉舟
        if (hpPct < 25.0f) 
        {
            uint32 const lastStand = GetAppropriateRank(ProtectionWarriorSpells::LAST_STAND); 
            if (lastStand && CanCast(me, lastStand, false)) 
            {
                if (ExecuteSpell(me, lastStand, false)) 
                    return;
            }
        }

        // 生命 < 35%：盾墙
        if (hpPct < 35.0f) 
        {
            uint32 const shieldWall = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_WALL); 
            if (shieldWall && CanCast(me, shieldWall, false)) 
            {
                if (ExecuteSpell(me, shieldWall, false)) 
                    return;
            }
        }

        // 生命 < 50%：狂暴回复 (前置激怒激活保护)
        if (hpPct < 50.0f && !me->HasAura(ProtectionWarriorSpells::ENRAGED_REGENERATION)) 
        {
            bool hasEnrage = me->HasAura(ProtectionWarriorSpells::BLOODRAGE) ||
                             me->HasAura(ProtectionWarriorSpells::BERSERKER_RAGE); 

            if (!hasEnrage) 
            {
                if (CanCast(me, ProtectionWarriorSpells::BLOODRAGE, false)) 
                    hasEnrage = ExecuteSpell(me, ProtectionWarriorSpells::BLOODRAGE, false); 
                else if (CanCast(me, ProtectionWarriorSpells::BERSERKER_RAGE, false)) 
                    hasEnrage = ExecuteSpell(me, ProtectionWarriorSpells::BERSERKER_RAGE, false); 
            }

            if (hasEnrage) 
            {
                uint32 const enrRegen = GetAppropriateRank(ProtectionWarriorSpells::ENRAGED_REGENERATION); 
                if (enrRegen && CanCast(me, enrRegen, false)) 
                {
                    if (ExecuteSpell(me, enrRegen, false)) 
                        return;
                }
            }
        }

        // 生命 < 80% 或正在挨打：盾牌格挡
        if (hpPct < 80.0f || !me->getAttackers().empty()) 
        {
            uint32 const shieldBlock = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_BLOCK); 
            if (shieldBlock && CanCast(me, shieldBlock, false)) 
                ExecuteSpell(me, shieldBlock, false); 
        }

        // ---------------------------------------------------------------------
        // P2: 机动突进、战术打断、援护与换防救火
        // ---------------------------------------------------------------------
        float const distToVictim = me->GetDistance(victim); 

        // 1. 常规起手突进接怪
        if (distToVictim >= 8.0f && distToVictim <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT)) 
        {
            if (chargeCooldown == 0)
            {
                uint32 const charge = GetAppropriateRank(ProtectionWarriorSpells::CHARGE); 
                if (charge && CanCast(victim, charge, false)) 
                {
                    if (ExecuteSpell(victim, charge, false)) 
                    {
                        chargeCooldown = 15000;
                        return;
                    }
                }
            }

            if (interceptCooldown == 0)
            {
                uint32 const intercept = GetAppropriateRank(ProtectionWarriorSpells::INTERCEPT); 
                if (intercept && CanCast(victim, intercept, false)) 
                {
                    if (ExecuteSpell(victim, intercept, false)) 
                    {
                        interceptCooldown = 30000;
                        return;
                    }
                }
            }
        }

        // 2. 战术打断：盾击接入阶段三基类记忆化压秒打断仲裁引擎。
        // 交由基类 ShouldInterruptTarget 统一裁决：引导类即刻抢断，
        // 读条类严格按 learnedInterruptDelays 学到的压秒余量出手，
        // 彻底取代原先「只要在读条就砍」的盲目秒断。
        if (shieldBashCooldown == 0)
        {
            uint32 const shieldBash = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_BASH);
            if (shieldBash && TryInterrupt(victim, shieldBash))
            {
                shieldBashCooldown = 12000;
                return;
            }
        }

        // 远距离突进打断：仅当目标确在施法且处于突进射程时才交冲锋/拦截，
        // 避免把机动技能浪费在无读条的常规拉怪上。
        if (victim->HasUnitState(UNIT_STATE_CASTING) && distToVictim >= 8.0f && distToVictim <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT))
        {
            if (chargeCooldown == 0)
            {
                uint32 const charge = GetAppropriateRank(ProtectionWarriorSpells::CHARGE); 
                if (charge && CanCast(victim, charge, false)) 
                {
                    if (ExecuteSpell(victim, charge, false)) 
                    {
                        chargeCooldown = 15000;
                        return;
                    }
                }
            }

            if (interceptCooldown == 0)
            {
                uint32 const intercept = GetAppropriateRank(ProtectionWarriorSpells::INTERCEPT); 
                if (intercept && CanCast(victim, intercept, false)) 
                {
                    if (ExecuteSpell(victim, intercept, false)) 
                    {
                        interceptCooldown = 30000;
                        return;
                    }
                }
            }
        }

        // 3. 战术机动：援护队友 (ID 3411)
        Player* master = GetMaster(); 
        if (master && master->IsAlive() && master->GetHealthPct() < 70.0f && !master->getAttackers().empty()) 
        {
            float const distToMaster = me->GetDistance(master); 
            if (distToMaster >= 8.0f && distToMaster <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT)) 
            {
                uint32 const intervene = GetAppropriateRank(ProtectionWarriorSpells::INTERVENE); 
                if (intervene && CanCast(master, intervene, true)) 
                {
                    if (ExecuteSpell(master, intervene, true)) 
                        return;
                }
            }
        }

        // 4. 仇恨失控剥离 (换防救火)
        if (Unit* urgentTarget = GetUrgentThreatTarget()) 
        {
            float const distToUrgent = me->GetDistance(urgentTarget); 

            // 战神远距离冲锋/拦截优先贴脸打断控怪
            if (chargeCooldown == 0 && distToUrgent >= 8.0f && distToUrgent <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT)) 
            {
                uint32 const charge = GetAppropriateRank(ProtectionWarriorSpells::CHARGE); 
                if (charge && CanCast(urgentTarget, charge, false)) 
                {
                    if (ExecuteSpell(urgentTarget, charge, false)) 
                    {
                        chargeCooldown = 15000;
                        return;
                    }
                }
            }

            // 远程嘲讽 (30 码, 强拉仇恨)
            uint32 const taunt = GetAppropriateRank(ProtectionWarriorSpells::TAUNT); 
            if (tauntCooldown == 0 && taunt && CanCast(urgentTarget, taunt, false)) 
            {
                if (ExecuteSpell(urgentTarget, taunt, false)) 
                {
                    tauntCooldown = 8000;
                    return;
                }
            }
        }

        // 5. 强力单体昏迷硬控：震荡猛击
        if (victim->HasUnitState(UNIT_STATE_CASTING) || victim->GetHealthPct() > 50.0f) 
        {
            uint32 const concBlow = GetAppropriateRank(ProtectionWarriorSpells::CONCUSSION_BLOW); 
            if (concBlow && CanCast(victim, concBlow, true)) 
            {
                if (ExecuteSpell(victim, concBlow, true)) 
                    return;
            }
        }

        // ---------------------------------------------------------------------
        // P3: 核心仇恨打击、减益维持与 AoE 控制
        // ---------------------------------------------------------------------
        uint8 meleeEnemies = 0; 
        for (Unit* attacker : me->getAttackers()) 
        {
            if (attacker && attacker->IsAlive() && me->IsWithinMeleeRange(attacker)) 
            {
                ++meleeEnemies; 
                if (meleeEnemies >= 3) 
                    break;
            }
        }

        // 1. 维持核心减速/减攻速 Debuff：雷霆一击
        if (me->IsWithinDist(victim, 8.0f) && !victim->HasAura(ProtectionWarriorSpells::THUNDER_CLAP)) 
        {
            uint32 const thunderClap = GetAppropriateRank(ProtectionWarriorSpells::THUNDER_CLAP); 
            if (thunderClap && CanCast(me, thunderClap, true)) 
            {
                if (ExecuteSpell(me, thunderClap, true)) 
                    return;
            }
        }

        // 2. 核心仇恨打击：盾牌猛击
        uint32 const shieldSlam = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_SLAM); 
        if (shieldSlam && CanCast(victim, shieldSlam, true)) 
        {
            if (ExecuteSpell(victim, shieldSlam, true)) 
                return;
        }

        // 3. 高效反击：复仇 (严格检查格挡防御触发态)
        if (me->HasAuraState(AURA_STATE_DEFENSE)) 
        {
            uint32 const revenge = GetAppropriateRank(ProtectionWarriorSpells::REVENGE); 
            if (revenge && CanCast(victim, revenge, true)) 
            {
                if (ExecuteSpell(victim, revenge, true)) 
                    return;
            }
        }

        // 4. 正面锥形群拉与群体昏迷：震荡波
        if (me->IsWithinDist(victim, 10.0f)) 
        {
            uint32 const shockwave = GetAppropriateRank(ProtectionWarriorSpells::SHOCKWAVE); 
            if (shockwave && CanCast(me, shockwave, true)) 
            {
                if (ExecuteSpell(me, shockwave, true, victim)) 
                    return;
            }
        }

        // 5. 战斗中团队怒吼维持 (消除战斗中长达数分钟的 Buff 断档)
        if (shoutCheckTimer == 3000 && me->GetPower(POWER_RAGE) >= 200) 
        {
            if (MaintainTeamShout()) 
                return;
        }

        // 6. 降低近战攻强 Debuff：挫志怒吼
        if (me->IsWithinDist(victim, 10.0f) &&
            !victim->HasAura(ProtectionWarriorSpells::DEMORALIZING_SHOUT) &&
            !victim->HasAura(ProtectionWarriorSpells::AURA_DEMO_ROAR)) 
        {
            uint32 const demoShout = GetAppropriateRank(ProtectionWarriorSpells::DEMORALIZING_SHOUT); 
            if (demoShout && CanCast(me, demoShout, true)) 
            {
                if (ExecuteSpell(me, demoShout, true)) 
                    return;
            }
        }

        // 7. 核心破甲维持与填充打击：毁灭打击 / 破甲攻击
        bool const hasOtherArmorDebuff = victim->HasAura(ProtectionWarriorSpells::AURA_EXPOSE_ARMOR) ||
                                         victim->HasAura(ProtectionWarriorSpells::AURA_ACID_SPIT); 

        uint32 const devastate = GetAppropriateRank(ProtectionWarriorSpells::DEVASTATE); 
        uint32 const sunderSpell = devastate ? devastate : GetAppropriateRank(ProtectionWarriorSpells::SUNDER_ARMOR); 

        if (sunderSpell && !hasOtherArmorDebuff) 
        {
            uint32 const activeSunderRank = GetAppropriateRank(ProtectionWarriorSpells::SUNDER_ARMOR); 
            Aura* sunderAura = activeSunderRank ? victim->GetAura(activeSunderRank) : nullptr; 
            bool const needSunder = !sunderAura || sunderAura->GetStackAmount() < 5 || sunderAura->GetDuration() < 3000; 

            if (needSunder && CanCast(victim, sunderSpell, true)) 
            {
                if (ExecuteSpell(victim, sunderSpell, true)) 
                    return;
            }
            else if (devastate && CanCast(victim, devastate, true)) 
            {
                if (ExecuteSpell(victim, devastate, true)) 
                    return;
            }
        }

        // ---------------------------------------------------------------------
        // P4: 高怒泄怒平砍强化 (排队去重)
        // ---------------------------------------------------------------------
        if (me->GetPower(POWER_RAGE) >= 450 && !me->GetCurrentSpell(CURRENT_MELEE_SPELL)) 
        {
            if (meleeEnemies >= 2) 
            {
                uint32 const cleave = GetAppropriateRank(ProtectionWarriorSpells::CLEAVE); 
                if (cleave && CanCast(victim, cleave, false)) 
                    ExecuteSpell(victim, cleave, false); 
            }
            else
            {
                uint32 const heroicStrike = GetAppropriateRank(ProtectionWarriorSpells::HEROIC_STRIKE); 
                if (heroicStrike && CanCast(victim, heroicStrike, false)) 
                    ExecuteSpell(victim, heroicStrike, false); 
            }
        }
        // 平砍驱动：ScriptedAI::UpdateAI 已被本类完整接管，引擎不会自动驱动平砍，
        // 必须在决策流末帧显式调用，否则白字伤害与平砍产怒永久缺失。
        DoMeleeAttackIfReady();
    }

private:
    uint32 shoutCheckTimer{ 0 };

    // =========================================================================
    // 自管冷却登记
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪 (Unit::HasSpellCooldown 恒 false)，
    // 故 CanCast 中的冷却校验对本随从形同虚设，凡无「持续光环保护」的 CD 技能
    // 必须由专精自行计时，否则会每一帧对同一技能空转重入、被底层反复拒放刷日志。
    // =========================================================================
    uint32 shieldBashCooldown{ 0 };
    uint32 chargeCooldown{ 0 };
    uint32 interceptCooldown{ 0 };
    uint32 tauntCooldown{ 0 };
    uint32 battleShoutRetryTimer{ 0 };

    void UpdateWarriorTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(shieldBashCooldown);
        Tick(chargeCooldown);
        Tick(interceptCooldown);
        Tick(tauntCooldown);
        Tick(battleShoutRetryTimer);
    }

    // =========================================================================
    // 坦克近战走位与 APF 势场避险 (铁律 21)
    // -------------------------------------------------------------------------
    // 本函数以同名成员刻意隐藏基类 AdaptiveBotAI::ManageMeleeCombat：
    // 基类版本只要「场上存在任一危险禁区」就无条件下发 APF 航点，坦克会被反复
    // 推离聚怪点位，造成仇恨丢失与站位漂移；坦克版本仅在自身真正踏入危险区
    // (IsUnderDangerThreat) 时才接管走位。因基类该函数非 virtual，此名称隐藏
    // 仅在坦克专精内部生效，不波及其他近战专精的既有行为。
    // =========================================================================
    void ManageMeleeCombat(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || victim->GetMap() != me->GetMap())
            return;

        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // ---- APF 势场紧急避险 (仅圆形火圈/毒池，正面顺劈由坦克本体硬接) ----
        // 势场单步外推为 3.0 码定长，若服务端每 50ms 心跳都重下 MovePoint，
        // 起跑动画会被无限掐断重置而表现为原地抽搐，故以 300ms 帧节流管控重算频率。
        if (IsUnderDangerThreat(2.0f) && !me->HasUnitState(UNIT_STATE_CHARGING))
        {
            if (apfMoveUpdateTimer == 0 || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float nextX = 0.0f, nextY = 0.0f, nextZ = 0.0f;

                // 第二参数 avoidFrontalCone = false：正面承伤聚怪是坦克本职，不规避锥形区；
                // 第三参数 isTank = true：由势场底层豁免正面顺劈与友军防挤压斥力。
                if (PotentialField::CalculateNextPosition(me, victim, 2.0f, false, true, activeDangerZones, nextX, nextY, nextZ))
                {
                    if (me->GetVictim() != victim || !me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                        me->Attack(victim, true);

                    me->GetMotionMaster()->MovePoint(1, nextX, nextY, nextZ);
                    apfMoveUpdateTimer = 300;
                    return;
                }
            }
            else
            {
                return; // 正在平滑执行上一个 APF 避险航点，不打断
            }

            // 势场未能给出有效落点但已进入近战范围：就地站桩挥砍，
            // 严禁向下击穿 MoveChase 把坦克拉回火圈中心造成溜溜球折返。
            if (me->IsWithinMeleeRange(victim))
            {
                if (me->GetVictim() != victim || !me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    me->Attack(victim, true);

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
                    me->GetMotionMaster()->Clear();

                return;
            }
        }

        // ---- 常规追击：坦克直线贴身硬刚，不追求背身位 ----
        if (me->GetVictim() != victim || !me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
            me->Attack(victim, true);

        // 冲锋/拦截的 EFFECT_MOTION 期间严禁下发任何走位指令，
        // 否则会当帧掐断突进路径，把坦克钉死在技能起手点原地空转。
        if (me->HasUnitState(UNIT_STATE_CHARGING))
            return;

        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
        if (moveType != CHASE_MOTION_TYPE && moveType != POINT_MOTION_TYPE)
            me->GetMotionMaster()->MoveChase(victim);
    }

    uint32 GetTeamShoutSpell() const
    {
        if (me->GetLevel() >= 68) 
            return GetAppropriateRank(ProtectionWarriorSpells::COMMANDING_SHOUT); 
        return GetAppropriateRank(ProtectionWarriorSpells::BATTLE_SHOUT); 
    }

    void ApplyPassiveTalents()
    {
        uint8 const level = me->GetLevel();

        auto SyncPassive = [this, level](uint8 minLvl, uint32 spellId)
        {
            if (level >= minLvl)
            {
                if (!me->HasAura(spellId))
                    me->AddAura(spellId, me);
            }
            else
            {
                me->RemoveAurasDueToSpell(spellId);
            }
        };

        SyncPassive(10, ProtectionWarriorSpells::AURA_TOUGHNESS); 
        SyncPassive(10, ProtectionWarriorSpells::AURA_WARBRINGER); 
        SyncPassive(20, ProtectionWarriorSpells::AURA_ONE_HANDED_SPEC); 
        SyncPassive(30, ProtectionWarriorSpells::AURA_CRITICAL_BLOCK); 
        SyncPassive(40, ProtectionWarriorSpells::AURA_VITALITY); 
        SyncPassive(50, ProtectionWarriorSpells::AURA_DAMAGE_SHIELD); 
    }

    // 全局通用的怒吼维持 (战斗内外皆可调用)
    bool MaintainTeamShout()
    {
        // 重试节流：同类 AP 增益 (圣骑士力量祝福等) 覆盖时底层会拒绝施放怒吼，
        // 无节流守卫会在战斗内的每次 3 秒巡检中重复尝试并被拒绝，空转烧掉决策流。
        if (battleShoutRetryTimer > 0)
            return false;

        Player* master = GetMaster(); 
        uint32 const shoutSpell = GetTeamShoutSpell(); 
        if (!shoutSpell) 
            return false;

        auto NeedsTeamShout = [](Unit* target, uint32 spellId) -> bool
        {
            if (!target || !target->IsAlive())
                return false;
            return !target->HasAura(spellId);
        };

        bool const selfNeedsShout   = NeedsTeamShout(me, shoutSpell); 
        bool const masterNeedsShout = master && NeedsTeamShout(master, shoutSpell); 

        if (selfNeedsShout || masterNeedsShout) 
        {
            if (CanCast(me, shoutSpell, true)) 
            {
                if (ExecuteSpell(me, shoutSpell, true)) 
                    return true;
            }
            else
            {
                // 底层拒放 (多为同类 AP 增益覆盖)：置位重试节流，避免每帧空转重入。
                battleShoutRetryTimer = 5000;
            }
        }
        return false;
    }
};

void AddSC_bot_protection_warrior()
{
    new AdaptiveBotScript<BotProtectionWarriorAI>("bot_protection_warrior");
}
