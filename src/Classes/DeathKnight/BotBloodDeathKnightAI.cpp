/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BloodDeathKnightSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "SpellAuras.h"
#include "Pet.h"
#include "Chat.h"
#include <algorithm>
#include <vector>

class BotBloodDeathKnightAI : public AdaptiveBotAI
{
public:
    explicit BotBloodDeathKnightAI(Creature* creature) : AdaptiveBotAI(creature) {}

    bool IsTankBot() const override { return true; }

    // 专精契约：血DK为纯坦克定位，不常驻伴随护卫
    bool ShouldHaveGuardian() const override { return false; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case BloodDeathKnightSpells::HEART_STRIKE:
                return BloodDeathKnightSpells::HEART_STRIKE_MIN_LEVEL;
            case BloodDeathKnightSpells::DEATH_STRIKE:
                return BloodDeathKnightSpells::DEATH_STRIKE_MIN_LEVEL;
            case BloodDeathKnightSpells::VAMPIRIC_BLOOD:
                return BloodDeathKnightSpells::VAMPIRIC_BLOOD_MIN_LEVEL;
            case BloodDeathKnightSpells::RUNE_TAP:
                return BloodDeathKnightSpells::RUNE_TAP_MIN_LEVEL;
            default:
                return 0;
        }
    }

    void Reset() override
    {
        // 符文能量通道必须先于基类重置完成配置，保证 SyncLevelWithMaster 走符文能量分支
        // 初始符能保底 30 点，确保接怪瞬间遭遇法系尖刺即可开启反魔法护盾
        me->setPowerType(POWER_RUNIC_POWER);
        me->SetMaxPower(POWER_RUNIC_POWER, 100);
        me->SetPower(POWER_RUNIC_POWER, MIN_RUNIC_POWER_RESERVE);

        AdaptiveBotAI::Reset();

        presenceCheckTimer = 0;
        pestilenceTimer = 0;
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 level) override
    {
        AdaptiveBotAI::OnLevelSynced(level);
        ApplyPassiveTalents();
    }

    void OnEngaged(Unit* /*who*/) override
    {
        // 进战符能保底：绿罩 / 冰封之韧 / 符文打击 各需 20 点符能，
        // 避免随从因 0 符能导致救命技能全部卡死。
        if (me->GetPower(POWER_RUNIC_POWER) < MIN_RUNIC_POWER_RESERVE)
            me->SetPower(POWER_RUNIC_POWER, MIN_RUNIC_POWER_RESERVE);
    }

    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);

        // 巡检计时器：每 3 秒复核一次常驻姿态与增益
        if (presenceCheckTimer <= diff)
            presenceCheckTimer = 3000;
        else
            presenceCheckTimer -= diff;

        // 传染本地限流计时器维护
        if (pestilenceTimer > diff)
            pestilenceTimer -= diff;
        else
            pestilenceTimer = 0;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            UpdateFollowMaster(diff);

            if (presenceCheckTimer == 3000)
            {
                MaintainFrostPresence();
                MaintainHornOfWinter();
            }
            return;
        }

        // =====================================================================
        // 2. 索敌仲裁
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (!victim || !victim->IsAlive())
            return;

        // =====================================================================
        // 3. 近战平砍与追击状态机接管
        // =====================================================================
        ManageMeleeCombat(victim);

        // =====================================================================
        // Action Priority List (APL) 核心决策循环
        // =====================================================================

        // P0: 姿态与开怪 (冰霜灵气 / 冰冷触摸 14 倍仇恨开怪 / 法系怪战术死握)
        if (MaintainFrostPresence())
            return;

        if (TryOpenWithIcyTouch(victim))
            return;

        // 战术聚怪：远程法系读条怪强行拉入近战位
        if (TryDeathGrip(victim, true))
            return;

        // 寒冬号角：2 分钟增益到期时补吹，放行 GCD 并退出当前帧
        if (MaintainHornOfWinter())
            return;

        // 符能保底：低于阈值时平滑补充，模拟平砍与受击获取符能 (不占用 GCD)
        SupplementRunicPower();

        // P1: 生存与减伤链 (绿罩 / 冰封之韧 / 吸血鬼之血 / 符文分流)
        if (MaintainDefensiveCooldowns(victim))
            return;

        // P2: 仇恨救急 (死握拉回 / 黑暗命令嘲讽)
        if (HandleTauntEmergency())
            return;

        // P3: 核心自疗与高仇恨泄能 (灵界打击 / 符文打击)
        if (MaintainDeathStrike(victim))
            return;

        // 符文打击为「下一次平砍强化」，不占用 GCD：无论是否施放成功均继续流向后续 GCD 技能
        TryRuneStrike(victim);

        // P4: 疾病链与群拉 (冰冷触摸 / 暗影打击 / 传染)
        if (MaintainDiseases(victim))
            return;

        // P5: AoE 与填充 (枯萎凋零 / 血液沸腾 / 心脏打击 / 灵界打击)
        if (PerformCombatFiller(victim))
            return;
    }

private:
    // 符能保底阈值：绿罩 / 冰封之韧 / 符文打击 均需 20 点符能
    static constexpr uint32 MIN_RUNIC_POWER_RESERVE = 30;

    // 符能战时续航参数
    static constexpr uint32 RUNIC_POWER_LOW_THRESHOLD   = 20; // 低于该值触发被动补能
    static constexpr uint32 RUNIC_POWER_REFILL_AMOUNT   = 10; // 单次被动补能量

    // 传染本地限流：3.3.5a 传染无技能 CD，需自行约束刷新节奏
    static constexpr uint32 PESTILENCE_COOLDOWN_MS      = 10000;

    uint32 presenceCheckTimer{ 0 };
    uint32 pestilenceTimer{ 0 };

    // =========================================================================
    // 常驻姿态与增益维护器
    // =========================================================================
    bool MaintainFrostPresence()
    {
        if (me->HasAura(BloodDeathKnightSpells::FROST_PRESENCE))
            return false;

        if (!CanCast(me, BloodDeathKnightSpells::FROST_PRESENCE, true))
            return false;

        return ExecuteSpell(me, BloodDeathKnightSpells::FROST_PRESENCE, true);
    }

    bool MaintainHornOfWinter()
    {
        // 寒冬号角在 3.3.5a 占用 1.0 秒 GCD，战时不再反复吹动；
        // 符能续航统一交由 SupplementRunicPower() 保底机制处理。
        if (me->HasAura(BloodDeathKnightSpells::HORN_OF_WINTER))
            return false;

        if (!CanCast(me, BloodDeathKnightSpells::HORN_OF_WINTER, true))
            return false;

        return ExecuteSpell(me, BloodDeathKnightSpells::HORN_OF_WINTER, true);
    }

    // =========================================================================
    // 符能被动补给：低于阈值时平滑注入，模拟平砍与受击获取符能
    // =========================================================================
    void SupplementRunicPower()
    {
        if (me->GetPower(POWER_RUNIC_POWER) >= RUNIC_POWER_LOW_THRESHOLD)
            return;

        uint32 const current = me->GetPower(POWER_RUNIC_POWER);
        uint32 const max = me->GetMaxPower(POWER_RUNIC_POWER);
        uint32 const refilled = std::min(max, current + RUNIC_POWER_REFILL_AMOUNT);

        me->SetPower(POWER_RUNIC_POWER, refilled);
    }

    // =========================================================================
    // 开怪 / 远距仇恨压制：冰冷触摸 (14 倍仇恨倍率)
    // 当目标尚未锁定自身 (仇恨不稳) 时，即使已有冰霜疫病也持续压制第一仇恨。
    // =========================================================================
    bool TryOpenWithIcyTouch(Unit* victim)
    {
        if (!victim || victim == me)
            return false;

        float const dist = me->GetDistance(victim);
        if (dist < 8.0f || dist > 30.0f)
            return false;

        bool const threatUnstable = (victim->GetVictim() != me);
        if (!threatUnstable && victim->HasAura(BloodDeathKnightSpells::AURA_FROST_FEVER))
            return false;

        uint32 const icyTouch = GetAppropriateRank(BloodDeathKnightSpells::ICY_TOUCH);
        if (!icyTouch || !CanCast(victim, icyTouch, true))
            return false;

        return ExecuteSpell(victim, icyTouch, true);
    }

    // =========================================================================
    // 死亡之握：8 ~ 30 码区间将目标拉回近战位 (严格作为战略救急手段)
    // =========================================================================
    bool TryDeathGrip(Unit* target, bool requireCaster = false)
    {
        if (!target || target == me)
            return false;

        float const dist = me->GetDistance(target);
        if (dist < 8.0f || dist > 30.0f)
            return false;

        // 战术模式：仅对远程法系读条怪 (或拥有法力池) 主动拉怪，避免对普通近战怪乱交战略技能；
        // 救急模式 (requireCaster = false) 不做限制，用于拉回失控目标。
        if (requireCaster)
        {
            bool const isCaster = target->IsNonMeleeSpellCast(false) ||
                                  (target->GetMaxPower(POWER_MANA) > 0);
            if (!isCaster)
                return false;
        }

        if (!CanCast(target, BloodDeathKnightSpells::DEATH_GRIP, true))
            return false;

        return ExecuteSpell(target, BloodDeathKnightSpells::DEATH_GRIP, true);
    }

    // =========================================================================
    // 生存减伤链 (反魔法护盾 / 冰封之韧 / 吸血鬼之血 / 符文分流)
    // =========================================================================
    bool MaintainDefensiveCooldowns(Unit* victim)
    {
        float const hpPct = me->GetHealthPct();

        // 反魔法护盾 (绿罩)：仅当目标正在施放非近战法术 (明确的法术尖刺前摇) 时开启
        bool const targetCasting = victim->IsNonMeleeSpellCast(false);

        if (targetCasting &&
            !me->HasAura(BloodDeathKnightSpells::ANTI_MAGIC_SHELL) &&
            CanCast(me, BloodDeathKnightSpells::ANTI_MAGIC_SHELL, true))
        {
            if (ExecuteSpell(me, BloodDeathKnightSpells::ANTI_MAGIC_SHELL, true))
                return true;
        }

        // 冰封之韧：自身血量 < 35%
        if (hpPct < 35.0f &&
            !me->HasAura(BloodDeathKnightSpells::ICEBOUND_FORTITUDE) &&
            CanCast(me, BloodDeathKnightSpells::ICEBOUND_FORTITUDE, true))
        {
            if (ExecuteSpell(me, BloodDeathKnightSpells::ICEBOUND_FORTITUDE, true))
                return true;
        }

        // 吸血鬼之血：自身血量 < 50%
        if (hpPct < 50.0f &&
            !me->HasAura(BloodDeathKnightSpells::VAMPIRIC_BLOOD) &&
            CanCast(me, BloodDeathKnightSpells::VAMPIRIC_BLOOD, true))
        {
            if (ExecuteSpell(me, BloodDeathKnightSpells::VAMPIRIC_BLOOD, true))
                return true;
        }

        // 符文分流：自身血量 < 60%
        uint32 const runeTap = GetAppropriateRank(BloodDeathKnightSpells::RUNE_TAP);
        if (runeTap && hpPct < 60.0f && CanCast(me, runeTap, true))
        {
            if (ExecuteSpell(me, runeTap, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // 仇恨救急：脱战目标死握拉回 + 近战位黑暗命令嘲讽
    // =========================================================================
    bool HandleTauntEmergency()
    {
        Unit* urgentTarget = GetUrgentThreatTarget();
        if (!urgentTarget)
            return false;

        if (TryDeathGrip(urgentTarget))
            return true;

        // 黑暗命令为 30 码远程强嘲；死亡之握冷却中时仍可隔距离直接拉回失控怪
        if (CanCast(urgentTarget, BloodDeathKnightSpells::DARK_COMMAND, true))
        {
            if (ExecuteSpell(urgentTarget, BloodDeathKnightSpells::DARK_COMMAND, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // 核心自疗：近战位且自身血量 < 85% 时以灵界打击汲取生命
    // 3.3.5a 机制约束：目标必须同时携带冰霜疫病与暗影疫病，灵界打击才产生有效自愈；
    // 双病未挂齐时直接返回 false，让路给 MaintainDiseases 补齐疾病链。
    // =========================================================================
    bool MaintainDeathStrike(Unit* victim)
    {
        if (!victim || me->GetHealthPct() >= 85.0f)
            return false;

        if (!me->IsWithinMeleeRange(victim))
            return false;

        if (!victim->HasAura(BloodDeathKnightSpells::AURA_FROST_FEVER) ||
            !victim->HasAura(BloodDeathKnightSpells::AURA_BLOOD_PLAGUE))
            return false;

        uint32 const deathStrike = GetAppropriateRank(BloodDeathKnightSpells::DEATH_STRIKE);
        if (!deathStrike || !CanCast(victim, deathStrike, true))
            return false;

        return ExecuteSpell(victim, deathStrike, true);
    }

    // =========================================================================
    // 高仇恨泄能：符文打击 (下一次平砍强化，不占用 GCD)
    // =========================================================================
    bool TryRuneStrike(Unit* victim)
    {
        if (!victim || !me->IsWithinMeleeRange(victim))
            return false;

        if (!CanCast(victim, BloodDeathKnightSpells::RUNE_STRIKE, true))
            return false;

        return ExecuteSpell(victim, BloodDeathKnightSpells::RUNE_STRIKE, true);
    }

    // =========================================================================
    // 疾病链：冰冷触摸 / 暗影打击 / 传染扩散
    // =========================================================================
    bool MaintainDiseases(Unit* victim)
    {
        if (!victim)
            return false;

        // 步骤一：冰冷触摸——仅在目标缺少冰霜疫病时补挂，
        // 严格顺序执行，避免无限冰触卡死暗影打击与传染。
        if (!victim->HasAura(BloodDeathKnightSpells::AURA_FROST_FEVER))
        {
            uint32 const icyTouch = GetAppropriateRank(BloodDeathKnightSpells::ICY_TOUCH);
            if (icyTouch && CanCast(victim, icyTouch, true))
            {
                if (ExecuteSpell(victim, icyTouch, true))
                    return true;
            }
        }

        // 步骤二：暗影打击——近战位补挂暗影疫病
        if (me->IsWithinMeleeRange(victim) &&
            !victim->HasAura(BloodDeathKnightSpells::AURA_BLOOD_PLAGUE))
        {
            uint32 const plagueStrike = GetAppropriateRank(BloodDeathKnightSpells::PLAGUE_STRIKE);
            if (plagueStrike && CanCast(victim, plagueStrike, true))
            {
                if (ExecuteSpell(victim, plagueStrike, true))
                    return true;
            }
        }

        // 步骤三：传染——多目标且主目标双病齐备时向周围扩散
        // 3.3.5a 传染无技能 CD，必须由本地计时器限流，
        // 否则会独占每个 GCD 无脑连发，卡死 P5 的枯萎凋零与血液沸腾。
        if (pestilenceTimer == 0 &&
            CountNearbyEnemies(10.0f) >= 2 &&
            victim->HasAura(BloodDeathKnightSpells::AURA_FROST_FEVER) &&
            victim->HasAura(BloodDeathKnightSpells::AURA_BLOOD_PLAGUE))
        {
            if (CanCast(victim, BloodDeathKnightSpells::PESTILENCE, true))
            {
                if (ExecuteSpell(victim, BloodDeathKnightSpells::PESTILENCE, true))
                {
                    pestilenceTimer = PESTILENCE_COOLDOWN_MS;
                    return true;
                }
            }
        }

        return false;
    }

    // =========================================================================
    // AoE 群拉与单体填充 (枯萎凋零 / 血液沸腾 / 心脏打击 / 灵界打击)
    // =========================================================================
    bool PerformCombatFiller(Unit* victim)
    {
        if (!victim)
            return false;

        bool const inMelee = me->IsWithinMeleeRange(victim);
        bool const needsAoE = (CountNearbyEnemies(10.0f) >= 2);

        // 枯萎凋零：多目标铺地仇恨
        if (needsAoE)
        {
            uint32 const deathAndDecay = GetAppropriateRank(BloodDeathKnightSpells::DEATH_AND_DECAY);
            if (deathAndDecay && CanCast(victim, deathAndDecay, true))
            {
                if (ExecuteSpell(victim, deathAndDecay, true, victim))
                    return true;
            }
        }

        // 血液沸腾：多目标自体 AoE 仇恨
        if (needsAoE && inMelee)
        {
            uint32 const bloodBoil = GetAppropriateRank(BloodDeathKnightSpells::BLOOD_BOIL);
            if (bloodBoil && CanCast(me, bloodBoil, true))
            {
                if (ExecuteSpell(me, bloodBoil, true, victim))
                    return true;
            }
        }

        // 心脏打击：单体核心填充
        if (inMelee)
        {
            uint32 const heartStrike = GetAppropriateRank(BloodDeathKnightSpells::HEART_STRIKE);
            if (heartStrike && CanCast(victim, heartStrike, true))
            {
                if (ExecuteSpell(victim, heartStrike, true))
                    return true;
            }
        }

        // 灵界打击：兜底填充 (同样要求双病齐备，避免无疾病空放)
        if (inMelee &&
            victim->HasAura(BloodDeathKnightSpells::AURA_FROST_FEVER) &&
            victim->HasAura(BloodDeathKnightSpells::AURA_BLOOD_PLAGUE))
        {
            uint32 const deathStrike = GetAppropriateRank(BloodDeathKnightSpells::DEATH_STRIKE);
            if (deathStrike && CanCast(victim, deathStrike, true))
            {
                if (ExecuteSpell(victim, deathStrike, true))
                    return true;
            }
        }

        return false;
    }

    // =========================================================================
    // 周围可攻击敌人计数器 (用于群体仇恨判定)
    // 跨来源去重采样：自身 + 主人/队友 + 随从集群的战斗攻击者，
    // 确保能识别正在攻击队友、而非仅攻击自身的怪物。
    // =========================================================================
    uint32 CountNearbyEnemies(float range)
    {
        std::vector<Unit*> enemies;

        auto Consider = [&](Unit* candidate)
        {
            if (!candidate || !candidate->IsAlive() || !candidate->IsInWorld())
                return;
            if (candidate == me || candidate->GetMap() != me->GetMap())
                return;
            if (!me->IsWithinDist(candidate, range) || !me->IsValidAttackTarget(candidate))
                return;
            if (std::find(enemies.begin(), enemies.end(), candidate) != enemies.end())
                return;

            enemies.push_back(candidate);
        };

        // 1. 自身战斗攻击者
        for (Unit* attacker : me->getAttackers())
            Consider(attacker);

        Player* master = GetMaster();

        if (master)
        {
            // 2. 指挥官与指挥官的宠物
            for (Unit* attacker : master->getAttackers())
                Consider(attacker);

            if (Unit* masterPet = master->GetPet())
            {
                for (Unit* attacker : masterPet->getAttackers())
                    Consider(attacker);
            }

            // 3. 小队成员的战斗攻击者
            if (Group* group = master->GetGroup())
            {
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->GetSource();
                    if (!member || member == master)
                        continue;

                    for (Unit* attacker : member->getAttackers())
                        Consider(attacker);

                    if (Unit* memberPet = member->GetPet())
                    {
                        for (Unit* attacker : memberPet->getAttackers())
                            Consider(attacker);
                    }
                }
            }

            // 4. 同指挥官随从集群
            {
                std::lock_guard<std::mutex> lock(s_botRegistryMutex);
                auto it = s_masterBotRegistry.find(master->GetGUID());
                if (it != s_masterBotRegistry.end())
                {
                    for (AdaptiveBotAI* allyBot : it->second)
                    {
                        if (allyBot && allyBot->me && allyBot->me != me)
                        {
                            for (Unit* attacker : allyBot->me->getAttackers())
                                Consider(attacker);
                        }
                    }
                }
            }
        }

        return static_cast<uint32>(enemies.size());
    }

    // =========================================================================
    // 鲜血天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
    // =========================================================================
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

        SyncPassive(BloodDeathKnightSpells::PASSIVE_TALENT_MIN_LEVEL, BloodDeathKnightSpells::WILL_OF_THE_NECROPOLIS);
        SyncPassive(BloodDeathKnightSpells::PASSIVE_TALENT_MIN_LEVEL, BloodDeathKnightSpells::VETERAN_OF_THE_THIRD_WAR);
        SyncPassive(BloodDeathKnightSpells::PASSIVE_TALENT_MIN_LEVEL, BloodDeathKnightSpells::TOUGHNESS);
        SyncPassive(BloodDeathKnightSpells::PASSIVE_TALENT_MIN_LEVEL, BloodDeathKnightSpells::ANTICIPATION);
    }
};

void AddSC_bot_blood_death_knight()
{
    new AdaptiveBotScript<BotBloodDeathKnightAI>("bot_blood_death_knight");
}
