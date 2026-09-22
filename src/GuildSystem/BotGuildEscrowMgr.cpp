/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BotGuildEscrowMgr.h"
#include "AdaptiveBotAI.h"
#include "../CommandSystem/BotCommandMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <vector>

BotGuildEscrowMgr* BotGuildEscrowMgr::Instance()
{
    static BotGuildEscrowMgr instance;
    return &instance;
}

void BotGuildEscrowMgr::LoadBankruptcyFromDB()
{
    std::unordered_map<ObjectGuid, uint32> loaded;

    QueryResult result = CharacterDatabase.Query("SELECT guid, debt_copper FROM character_bot_escrow WHERE is_bankrupt = 1");
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
            loaded[guid] = fields[1].Get<uint32>();
        } while (result->NextRow());
    }

    std::lock_guard<std::mutex> lock(_lock);
    _bankruptDebts.swap(loaded);

    LOG_INFO("server.loading", ">> [AdaptiveBot] 已载入 {} 条佣兵失信黑名单记录。", _bankruptDebts.size());
}

bool BotGuildEscrowMgr::IsBankrupt(ObjectGuid const& playerGuid)
{
    std::lock_guard<std::mutex> lock(_lock);
    return _bankruptDebts.find(playerGuid) != _bankruptDebts.end();
}

void BotGuildEscrowMgr::ClearBankruptcy(Player* player)
{
    if (!player)
        return;

    ObjectGuid const guid = player->GetGUID();

    // 全程锁内只做账务读改，消息下发与扣款动作一律在锁外执行
    uint32 totalRepay = 0;
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto it = _bankruptDebts.find(guid);
        if (it == _bankruptDebts.end())
            return;

        totalRepay = it->second;
    }

    uint32 const gold = totalRepay / 10000;
    uint32 const silver = (totalRepay % 10000) / 100;
    uint32 const copper = totalRepay % 100;

    if (player->GetMoney() < totalRepay)
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【公会信托】您的金币不足以偿还拖欠的 {}金 {}银 {}铜（含滞纳金），劳务中介仍对您保持封禁。|r",
                gold, silver, copper);
        return;
    }

    player->ModifyMoney(-static_cast<int64>(totalRepay));

    {
        std::lock_guard<std::mutex> lock(_lock);
        _bankruptDebts.erase(guid);
    }

    CharacterDatabase.Execute("UPDATE character_bot_escrow SET is_bankrupt = 0, debt_copper = 0 WHERE guid = {}",
        guid.GetCounter());

    if (player->GetSession())
        ChatHandler(player->GetSession()).PSendSysMessage(
            "【公会信托】您已成功结清欠款（{}金 {}银 {}铜），公会信用恢复正常！", gold, silver, copper);
}

void BotGuildEscrowMgr::PersistUnpaidDebtOnLogout(Player* player)
{
    if (!player)
        return;

    ObjectGuid const guid = player->GetGUID();
    uint32 unpaidCopper = 0;

    {
        std::lock_guard<std::mutex> lock(_lock);

        auto it = _activeContracts.find(guid);
        if (it == _activeContracts.end() || it->second.pendingCopper == 0)
            return;

        // 离线欠费与宽限期超时同口径：加收 15% 滞纳金后直接转为失信欠款。
        // 内存契约不落盘，一旦服务器在玩家离线期间重启，这条在途账单就会
        // 随进程一起蒸发，等价于用「登出 + 重启」白嫖整场随从。故此处立即固化。
        unpaidCopper = static_cast<uint32>(static_cast<float>(it->second.pendingCopper) * BREACH_PENALTY_RATE);
        _bankruptDebts[guid] = unpaidCopper;
        _activeContracts.erase(it);
        _contractProbeTimers.erase(guid);
    }

    CharacterDatabase.Execute(
        "REPLACE INTO character_bot_escrow (guid, is_bankrupt, debt_copper, updated_time) VALUES ({}, 1, {}, {})",
        guid.GetCounter(), unpaidCopper, static_cast<uint64>(GameTime::GetGameTime().count()));

    LOG_INFO("scripts", ">> [AdaptiveBot] 玩家 [{}] 离线未结清佣金，已将 {} 铜欠款固化至失信数据库。",
        player->GetName(), unpaidCopper);
}

void BotGuildEscrowMgr::StartContract(Player* player)
{
    if (!player)
        return;

    ObjectGuid const guid = player->GetGUID();
    uint32 const currentMapId = player->GetMapId();

    std::lock_guard<std::mutex> lock(_lock);

    // 幂等保护：在途契约绝不允许被重置。否则宽限期内任何一次探针触发
    // 都会把 pendingCopper 与宽限倒计时清零，等价于自动免单洗白。
    if (_activeContracts.find(guid) != _activeContracts.end())
        return;

    BotHireContract& contract = _activeContracts[guid];
    contract.masterGuid = guid;
    contract.pendingCopper = 0;
    contract.totalKilledCount = 0;
    contract.periodicTimer = 0;
    contract.lastMapId = currentMapId; // 开户即锚定当前地图，避免下一帧误判为切图
    contract.inGracePeriod = false;
    contract.gracePeriodTimer = 0;
    contract.graceRecoveryCheckTimer = 0;
    contract.reminded30Min = false;
    contract.reminded10Min = false;

    _contractProbeTimers.erase(guid);
}

void BotGuildEscrowMgr::RemoveContract(ObjectGuid const& playerGuid)
{
    std::lock_guard<std::mutex> lock(_lock);
    _activeContracts.erase(playerGuid);
    _contractProbeTimers.erase(playerGuid);
}

bool BotGuildEscrowMgr::HasActiveContract(ObjectGuid const& playerGuid)
{
    std::lock_guard<std::mutex> lock(_lock);
    return _activeContracts.find(playerGuid) != _activeContracts.end();
}

float BotGuildEscrowMgr::CalculateGuildDiscount(uint8 playerGuildId, uint8 botGuildId, bool isCrossFaction)
{
    // 同公会享受 7.5 折内部津贴
    if (playerGuildId != GUILD_NONE && playerGuildId == botGuildId)
        return 0.75f;

    // 跨阵营中介加收 20%
    if (isCrossFaction)
        return 1.20f;

    // 常规原价
    return 1.0f;
}

GuildPetConfig const* BotGuildEscrowMgr::GetGuildConfig(uint8 guildId)
{
    for (auto const& cfg : GUILD_CONFIGS)
    {
        if (cfg.guildId == guildId)
            return &cfg;
    }
    return nullptr;
}

GuildPetConfig const* BotGuildEscrowMgr::GetGuildConfigByCreature(uint32 creatureEntry)
{
    for (auto const& cfg : GUILD_CONFIGS)
    {
        if (cfg.creatureEntry == creatureEntry)
            return &cfg;
    }
    return nullptr;
}

uint8 BotGuildEscrowMgr::GetBotGuildIdFromEntry(uint32 entry)
{
    // Phase 1 模版 Entry 公式：71000 + gid * 31 + spec_id (gid: 1~10, spec_id: 1~31)
    // Entry 范围为 71032 ~ 71341
    if (entry >= 71032 && entry <= 71341)
    {
        uint32 const gid = (entry - 71001) / 31;
        if (gid >= 1 && gid <= 10)
            return static_cast<uint8>(gid);
    }
    return GUILD_NONE;
}

uint8 BotGuildEscrowMgr::GetPlayerGuildId(ObjectGuid const& playerGuid)
{
    std::lock_guard<std::mutex> lock(_lock);

    auto it = _guildMemberships.find(playerGuid);
    return (it != _guildMemberships.end()) ? it->second : static_cast<uint8>(GUILD_NONE);
}

bool BotGuildEscrowMgr::SetPlayerGuild(Player* player, uint8 guildId)
{
    if (!player)
        return false;

    GuildPetConfig const* cfg = GetGuildConfig(guildId);
    if (!cfg)
        return false;

    // 阵营准入审核：联盟专属公会拒绝部落指挥官，部落专属公会反之。
    if (cfg->isAllianceOnly && player->GetTeamId() != TEAM_ALLIANCE)
        return false;
    if (cfg->isHordeOnly && player->GetTeamId() != TEAM_HORDE)
        return false;

    ObjectGuid const guid = player->GetGUID();

    {
        std::lock_guard<std::mutex> lock(_lock);

        // 已有会籍者不得静默覆盖：转投必须显式走退会流程，否则旧公会的
        // 使魔道具与津贴状态会与新会籍并存，形成双重福利漏洞。
        if (_guildMemberships.find(guid) != _guildMemberships.end())
            return false;

        _guildMemberships[guid] = guildId;
        _lastSupplyClaimTime[guid] = 0;
    }

    CharacterDatabase.Execute(
        "REPLACE INTO character_bot_guild_member (guid, guild_id, join_time, last_supply_time) VALUES ({}, {}, {}, 0)",
        guid.GetCounter(), static_cast<uint32>(guildId), static_cast<uint64>(GameTime::GetGameTime().count()));

    return true;
}

bool BotGuildEscrowMgr::LeavePlayerGuild(Player* player)
{
    if (!player)
        return false;

    ObjectGuid const guid = player->GetGUID();

    // 门禁 1：名下仍有随从时禁止退会，避免产生无主随从与失联契约。
    if (AdaptiveBotAI::HasMasterBots(guid))
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【公会前台】您当前仍带领着公会随从，必须先完全解散队伍方可办理退会！|r");
        return false;
    }

    // 门禁 2：存在在途佣金时必须当场结清，金币不足则拒绝退会（杜绝"退会逃单"）。
    if (HasActiveContract(guid))
    {
        if (!SettleCurrentBill(player, BILLING_REASON_DISBAND))
        {
            if (player->GetSession())
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff0000【公会前台】您当前存在未结清的佣金账单，金币不足无法办理退会！|r");
            return false;
        }

        RemoveContract(guid);
    }

    uint8 currentGuildId = GUILD_NONE;
    {
        std::lock_guard<std::mutex> lock(_lock);

        auto it = _guildMemberships.find(guid);
        if (it == _guildMemberships.end())
            return false;

        currentGuildId = it->second;
        _guildMemberships.erase(it);
        _lastSupplyClaimTime.erase(guid);
    }

    CharacterDatabase.Execute("DELETE FROM character_bot_guild_member WHERE guid = {}", guid.GetCounter());

    // 使魔彻底回收三连：解除召唤实体、驱散召唤光环、追缴实物道具、注销法术书技能。
    // 缺任何一环都留下绕过路径——只清实体则光环残留会在重登时自动再召唤；
    // 只清光环则实物道具仍在背包可无限次使用。
    // AzerothCore 的 Player 并未提供 GetMiniPet / RemoveMiniPet，
    // 伴侣句柄由 Unit::GetCritterGUID()（UNIT_FIELD_CRITTER）持有，
    // 遣散须自行取实体后 DespawnOrUnsummon。
    if (ObjectGuid const critterGuid = player->GetCritterGUID())
    {
        if (Creature* critter = ObjectAccessor::GetCreature(*player, critterGuid))
            critter->DespawnOrUnsummon();
    }

    if (GuildPetConfig const* cfg = GetGuildConfig(currentGuildId))
    {
        player->RemoveAurasDueToSpell(cfg->spellId);
        player->DestroyItemCount(cfg->itemId, 1, true);
        player->removeSpell(cfg->spellId, SPEC_MASK_ALL, false);
    }

    if (player->GetSession())
        ChatHandler(player->GetSession()).PSendSysMessage(
            "【公会前台】您已成功退出公会。专属通信使魔已被收回注销，7.5 折公会津贴同步失效。");

    return true;
}

void BotGuildEscrowMgr::LoadGuildMembershipsFromDB()
{
    std::unordered_map<ObjectGuid, uint8>  loadedGuilds;
    std::unordered_map<ObjectGuid, uint64> loadedSupplies;

    QueryResult result = CharacterDatabase.Query("SELECT guid, guild_id, last_supply_time FROM character_bot_guild_member");
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
            loadedGuilds[guid] = fields[1].Get<uint8>();
            loadedSupplies[guid] = fields[2].Get<uint64>();
        } while (result->NextRow());
    }

    std::lock_guard<std::mutex> lock(_lock);
    _guildMemberships.swap(loadedGuilds);
    _lastSupplyClaimTime.swap(loadedSupplies);

    LOG_INFO("server.loading", ">> [AdaptiveBot] 已载入 {} 条冒险者公会会籍记录。", _guildMemberships.size());
}

namespace
{
    /// @brief 对单条法术执行「召唤实体」重定向，并沿触发链向下透传。
    ///        3.3.5a 伴侣法术普遍是两层结构：外壳法术挂 SPELL_EFFECT_TRIGGER_SPELL，
    ///        真正的 SPELL_EFFECT_SUMMON（MiscValue = 实体 Entry）挂在内核法术上，
    ///        只改外壳的 MiscValue 完全无效，故必须沿链下沉；
    ///        depth 上限用于阻断环形/自触发的畸形法术链。
    ///        同时严格限定只改「召唤类」效果：伴侣法术上常并存光环、加属性等其它效果，
    ///        盲改首个 MiscValue > 0 的效果会连带破坏这些逻辑。
    /// @return 是否至少成功改写了一处召唤目标。
    bool RedirectSummonChain(SpellInfo const* spellInfo, uint32 creatureEntry, uint8 depth = 0)
    {
        if (!spellInfo || depth > 3)
            return false;

        bool patched = false;
        std::array<uint32, MAX_SPELL_EFFECTS> triggeredSpells{};

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& effect = spellInfo->Effects[i];

            if (effect.Effect == SPELL_EFFECT_SUMMON || effect.Effect == SPELL_EFFECT_SUMMON_PET)
            {
                if (effect.MiscValue > 0)
                {
                    const_cast<SpellEffectInfo&>(effect).MiscValue = static_cast<int32>(creatureEntry);
                    patched = true;
                }
                continue;
            }

            if (effect.Effect == SPELL_EFFECT_TRIGGER_SPELL && effect.TriggerSpell != 0)
                triggeredSpells[i] = effect.TriggerSpell;
        }

        for (uint32 triggerSpellId : triggeredSpells)
        {
            if (triggerSpellId != 0)
                patched |= RedirectSummonChain(sSpellMgr->GetSpellInfo(triggerSpellId), creatureEntry, depth + 1);
        }

        return patched;
    }
}

void BotGuildEscrowMgr::RedirectGuildPetSpells()
{
    uint32 redirected = 0;

    for (auto const& cfg : GUILD_CONFIGS)
    {
        // 前置安全闸：独立高段模版必须已由 world 库导入。
        // 若 SQL 尚未执行就把伴侣法术重定向到并不存在的 Entry，召唤会当场失败、
        // 该公会的通信使魔将彻底无法召出。宁可保留原版召唤物，也绝不打坏玩家功能。
        if (!sObjectMgr->GetCreatureTemplate(cfg.creatureEntry))
        {
            LOG_ERROR("server.loading",
                ">> [AdaptiveBot] 公会 [{}] 的使魔模版 {} 不存在，已跳过法术重定向（请先执行 data/sql/db_world/base_bot_guild_pets.sql）。",
                cfg.name, cfg.creatureEntry);
            continue;
        }

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(cfg.spellId);
        if (!spellInfo)
        {
            LOG_ERROR("server.loading", ">> [AdaptiveBot] 公会 [{}] 的使魔召唤法术 {} 不存在，已跳过重定向。",
                cfg.name, cfg.spellId);
            continue;
        }

        if (RedirectSummonChain(spellInfo, cfg.creatureEntry))
        {
            ++redirected;
            LOG_DEBUG("scripts", ">> [AdaptiveBot] 法术 {} 的召唤目标已重定向至独立实体 {}（{}）。",
                cfg.spellId, cfg.creatureEntry, cfg.name);
        }
        else
        {
            LOG_ERROR("server.loading",
                ">> [AdaptiveBot] 公会 [{}] 的召唤法术 {} 未找到可重定向的召唤效果（含触发链），使魔将维持原版外观。",
                cfg.name, cfg.spellId);
        }
    }

    LOG_INFO("server.loading", ">> [AdaptiveBot] 已完成 {}/{} 种公会通信使魔的伴侣召唤法术重定向（Entry 70201-70210）。",
        redirected, GUILD_CONFIGS.size());
}

bool BotGuildEscrowMgr::CanClaimDailySupply(ObjectGuid const& playerGuid)
{
    uint64 const now = static_cast<uint64>(GameTime::GetGameTime().count());

    std::lock_guard<std::mutex> lock(_lock);

    // 非会员不享有公会的行军补给福利。
    if (_guildMemberships.find(playerGuid) == _guildMemberships.end())
        return false;

    auto it = _lastSupplyClaimTime.find(playerGuid);
    if (it == _lastSupplyClaimTime.end() || it->second == 0)
        return true;

    // 采用 20 小时冷却而非严格 24 小时：玩家若每天固定在相近时段上线，
    // 严格 24 小时会因日历漂移逐日挤出可领取窗口，最终永久错过一天。
    // 20 小时既守住"每日仅一次"的节奏，又保留 4 小时的容错余量。
    return now >= (it->second + DAILY_SUPPLY_COOLDOWN_SECONDS);
}

void BotGuildEscrowMgr::RecordDailySupplyClaim(ObjectGuid const& playerGuid)
{
    uint64 const now = static_cast<uint64>(GameTime::GetGameTime().count());

    {
        std::lock_guard<std::mutex> lock(_lock);
        _lastSupplyClaimTime[playerGuid] = now;
    }

    CharacterDatabase.Execute("UPDATE character_bot_guild_member SET last_supply_time = {} WHERE guid = {}",
        now, playerGuid.GetCounter());
}

bool BotGuildEscrowMgr::GetContractSnapshot(ObjectGuid const& playerGuid, uint32& pendingCopper, uint32& killedCount,
                                            bool& inGracePeriod, uint32& graceRemainingMs)
{
    std::lock_guard<std::mutex> lock(_lock);

    auto it = _activeContracts.find(playerGuid);
    if (it == _activeContracts.end())
        return false;

    pendingCopper = it->second.pendingCopper;
    killedCount = it->second.totalKilledCount;
    inGracePeriod = it->second.inGracePeriod;
    graceRemainingMs = it->second.gracePeriodTimer;
    return true;
}

void BotGuildEscrowMgr::AccumulateKillFee(Player* player, Creature* killed)
{
    if (!player || !killed)
        return;

    // 豁免图腾与环境小动物：图腾不构成战功，小动物是无收益装饰目标，
    // 照常计件只会凭空制造账单纠纷。
    if (killed->IsTotem() || killed->IsCritter())
        return;

    ObjectGuid const guid = player->GetGUID();

    // 契约存在性确认 + 去重判定合并为一次加锁：
    // 早期版本先 HasActiveContract() 再单独加锁去重，同一逻辑帧内要抢两遍 _lock。
    // 去重环归属契约本体后，同一次临界区即可完成「契约在否」与「是否重杀」双裁决，
    // 判定与写入原子完成，杜绝同帧双通道并发穿透。
    {
        std::lock_guard<std::mutex> lock(_lock);

        auto it = _activeContracts.find(guid);
        if (it == _activeContracts.end())
            return;

        if (it->second.IsDuplicateKill(killed->GetGUID()))
            return;
    }

    // 获取当前指挥官名下存活随从列表（此处不持有 _lock，规避锁序倒置）
    std::vector<AdaptiveBotAI*> const botGroup = BotCommandScript::CollectBotGroup(player);
    if (botGroup.empty())
        return;

    uint8 const mobLevel = killed->GetLevel();

    // 1. 基准单价 (铜): 1~80 级全程单调平滑递增。
    //    此前 60~79 级每级 +50 铜却以 1500 起步，导致 79 级算出 2450 铜
    //    反而高于 80 级的 1500 铜，出现等级倒挂；现改为 60 级 600 铜起、
    //    每级 +45，79 级 1455 铜，80 级 1500 铜，全程严格单调。
    uint32 baseRateCopper = 10;
    if (mobLevel < 60)
        baseRateCopper = std::max<uint32>(10, static_cast<uint32>(mobLevel) * 10);
    else if (mobLevel < 80)
        baseRateCopper = 600 + static_cast<uint32>(mobLevel - 60) * 45;
    else
        baseRateCopper = 1500; // 80 级基准 15 银

    // 2. 怪物类型权重：团本首领 60x，5 人本首领 20x，精英与团本杂兵 3x，普通怪 1x。
    //    此前把「处于团本地图」直接等同于「首领」，导致团本里每一只杂兵都按 60x
    //    结算，一趟小怪清场就能制造出天价账单。首领身份必须只看 isWorldBoss /
    //    IsDungeonBoss，团本地图仅作为杂兵的 3x 判据。
    float typeMultiplier = 1.0f;
    bool const isRaidMap = (killed->GetMap() && killed->GetMap()->IsRaid());

    if (killed->isWorldBoss() || (isRaidMap && killed->IsDungeonBoss()))
        typeMultiplier = 60.0f; // 团本首领: 60x (~9G)
    else if (killed->IsDungeonBoss())
        typeMultiplier = 20.0f; // 5 人本首领: 20x (~3G)
    else if (killed->isElite() || isRaidMap)
        typeMultiplier = 3.0f;  // 精英怪及团本普通杂兵: 3x (~45S)

    // 3. 累加随从全员佣金 (按各自真实原生会籍与阵营立场阶梯计价)
    //    阶段四落地：玩家会籍由公会前台正式登记（GetPlayerGuildId 实时查询），
    //    随从会籍则必须由实体 Entry 反解原生归属，而绝不能沿用指挥官会籍——
    //    否则任意公会都会退化为「自己雇自己」，0.75x 内部津贴变成永久常驻，
    //    不同公会/跨阵营的阶梯定价彻底失去意义。
    //    阵营判定同理：随从入队时 SetMaster 已把阵营刷为玩家阵营，
    //    运行时 GetFaction() 比对恒为同阵营，只能改看模版上的原生阵营属性。
    uint8 const playerGuildId = GetPlayerGuildId(guid);
    uint32 totalMobFee = 0;

    for (AdaptiveBotAI* bot : botGroup)
    {
        if (!bot || !bot->GetBotCreature() || !bot->GetBotCreature()->IsAlive())
            continue;

        // 1. 真实反解随从原生的公会会籍
        uint32 const botEntry = bot->GetBotCreature()->GetEntry();
        uint8 const botGuildId = GetBotGuildIdFromEntry(botEntry);

        // 2. 真实反解随从原生阵营立场（规避 SetMaster 带来的同阵营污染）
        bool isCrossFaction = false;
        if (GuildPetConfig const* botGuildCfg = GetGuildConfig(botGuildId))
        {
            if (player->GetTeamId() == TEAM_ALLIANCE && botGuildCfg->isHordeOnly)
                isCrossFaction = true;
            else if (player->GetTeamId() == TEAM_HORDE && botGuildCfg->isAllianceOnly)
                isCrossFaction = true;
        }

        // 3. 计算阶梯折扣：同公会 0.75x，跨阵营 1.20x，常规 1.00x
        float const discount = CalculateGuildDiscount(playerGuildId, botGuildId, isCrossFaction);

        totalMobFee += static_cast<uint32>(static_cast<float>(baseRateCopper) * typeMultiplier * discount);
    }

    std::lock_guard<std::mutex> lock(_lock);
    auto it = _activeContracts.find(guid);
    if (it != _activeContracts.end())
    {
        it->second.pendingCopper += totalMobFee;
        it->second.totalKilledCount += 1;
    }
}

bool BotGuildEscrowMgr::SettleCurrentBill(Player* player, BillingReason reason)
{
    if (!player)
        return false;

    ObjectGuid const guid = player->GetGUID();

    uint32 dueCopper = 0;
    uint32 kills = 0;
    bool wasInGracePeriod = false;

    {
        std::lock_guard<std::mutex> lock(_lock);
        auto it = _activeContracts.find(guid);
        if (it == _activeContracts.end() || it->second.pendingCopper == 0)
            return true;

        dueCopper = it->second.pendingCopper;
        kills = it->second.totalKilledCount;
        wasInGracePeriod = it->second.inGracePeriod;
    }

    // 财务透明度：告警与回执统一以「金/银/铜」三段式直读呈现，
    // 不再让玩家面对一串原始铜数自行换算。
    uint32 const gold = dueCopper / 10000;
    uint32 const silver = (dueCopper % 10000) / 100;
    uint32 const copper = dueCopper % 100;

    // -------------------------------------------------------------------------
    // 扣费失败通道：进入 1 小时宽限期，并告知确切欠款（消息下发必须脱离锁作用域）
    // -------------------------------------------------------------------------
    if (player->GetMoney() < dueCopper)
    {
        bool newlyEntered = false;
        {
            std::lock_guard<std::mutex> lock(_lock);
            auto it = _activeContracts.find(guid);
            if (it != _activeContracts.end() && !it->second.inGracePeriod)
            {
                it->second.inGracePeriod = true;
                it->second.gracePeriodTimer = GRACE_PERIOD_MS;
                it->second.graceRecoveryCheckTimer = 0;
                it->second.reminded30Min = false;
                it->second.reminded10Min = false;
                newlyEntered = true;
            }
        }

        if (newlyEntered && player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【公会警告】您的背包金币不足以支付佣金（当前欠款: {}金 {}银 {}铜）！已进入 1 小时信托宽限期，请尽快筹集资金；超时随从将罢工遣散并记入公会失信黑名单。|r",
                gold, silver, copper);

        return false;
    }

    // -------------------------------------------------------------------------
    // 扣费成功通道
    // -------------------------------------------------------------------------
    player->ModifyMoney(-static_cast<int64>(dueCopper));

    {
        std::lock_guard<std::mutex> lock(_lock);
        auto it = _activeContracts.find(guid);
        if (it != _activeContracts.end())
        {
            it->second.pendingCopper = 0;
            it->second.totalKilledCount = 0;
            it->second.inGracePeriod = false;
            it->second.gracePeriodTimer = 0;
            it->second.graceRecoveryCheckTimer = 0;
            it->second.reminded30Min = false;
            it->second.reminded10Min = false;
        }
    }

    if (player->GetSession())
    {
        char const* reasonStr = "周期扣款";
        if (wasInGracePeriod || reason == BILLING_REASON_RECOVERY)
            reasonStr = "宽限期补缴";
        else if (reason == BILLING_REASON_MAP_CHANGE)
            reasonStr = "副本切图结算";
        else if (reason == BILLING_REASON_DISBAND)
            reasonStr = "队伍解散清算";
        else if (reason == BILLING_REASON_MANUAL)
            reasonStr = "手动结算";

        // AzerothCore 的 PSendSysMessage 为 fmt 语义，占位符必须是 {}，
        // 沿用 printf 的 %s/%u 会在运行期抛 fmt::format_error。
        ChatHandler(player->GetSession()).PSendSysMessage(
            "【公会账单】[{}] 随从协助击杀 {} 个目标，支付佣金: {}金 {}银 {}铜。{}",
            reasonStr, kills, gold, silver, copper,
            wasInGracePeriod ? " 您的公会信用已恢复正常！" : "");
    }

    return true;
}

void BotGuildEscrowMgr::Update(Player* player, uint32 diff)
{
    if (!player)
        return;

    ObjectGuid const guid = player->GetGUID();

    // -------------------------------------------------------------------------
    // 契约自愈探针：招募入口不在本模块职责内，故由世界经济体自适应补建账户。
    // 每 1 秒低频巡检一次，指挥官已带兵但契约缺失时自动开户，杜绝漏单。
    // -------------------------------------------------------------------------
    bool hasContract = false;
    bool probeReady = false;
    {
        std::lock_guard<std::mutex> lock(_lock);
        hasContract = (_activeContracts.find(guid) != _activeContracts.end());

        if (!hasContract)
        {
            uint32& probe = _contractProbeTimers[guid];
            probe += diff;
            if (probe >= CONTRACT_PROBE_INTERVAL_MS)
            {
                probe = 0;
                probeReady = true;
            }
        }
    }

    if (!hasContract)
    {
        if (!probeReady)
            return;

        // 失信黑名单门禁：欠款未清者坚决不予开户，否则黑名单形同虚设。
        if (IsBankrupt(guid))
        {
            std::lock_guard<std::mutex> lock(_lock);
            _contractProbeTimers.erase(guid);
            return;
        }

        // 无契约且未带兵：立刻回收探针槽位，避免世界常驻期为所有历史 GUID 无界驻留。
        // 改用 O(1) 只读探针：CollectBotGroup 会拷贝整份快照，在每秒触发的
        // 开户判定路径上属于纯浪费的堆分配。
        if (!AdaptiveBotAI::HasMasterBots(guid))
        {
            std::lock_guard<std::mutex> lock(_lock);
            _contractProbeTimers.erase(guid);
            return;
        }

        StartContract(player);
    }

    // 随从存在性快照：必须在获取 _lock 之前完成随从总线的加锁，
    // 否则会在 _lock 内部嵌套 s_botRegistryMutex 形成锁序倒置。
    // 此处使用零堆分配的 HasMasterBots 而非 CollectBotGroup，
    // 既保留「锁外取样」的正确顺序，又彻底消除每秒心跳的 vector 抖动。
    bool const hasBots = AdaptiveBotAI::HasMasterBots(guid);

    bool shouldSettle = false;
    BillingReason settleReason = BILLING_REASON_PERIODIC;
    bool shouldForceDisband = false;
    bool send30MinWarning = false;
    bool send10MinWarning = false;
    uint32 breachDebt = 0;
    uint32 warningDueCopper = 0;

    {
        std::lock_guard<std::mutex> lock(_lock);

        auto it = _activeContracts.find(guid);
        if (it == _activeContracts.end())
            return;

        BotHireContract& contract = it->second;

        // 空契约自愈回收：既已无随从可供计件、又无未结佣金、且未处于追缴宽限期，
        // 说明该契约已彻底停摆（随从被遣散 / 团灭未归 / 解散流程中断）。
        // 若不摘除，_activeContracts 会随历史 GUID 无界驻留，构成常驻内存泄漏。
        if (!contract.inGracePeriod && contract.pendingCopper == 0 && !hasBots)
        {
            _activeContracts.erase(it);
            _contractProbeTimers.erase(guid);
            return;
        }

        // 双轨驱动之一：切图强制结算（进出副本 / 跨大陆传送）。
        // 以地图 ID 变化为判据而非依赖不存在的 OnMapChanged 钩子。
        uint32 const currentMapId = player->GetMapId();
        if (contract.lastMapId != currentMapId)
        {
            contract.lastMapId = currentMapId;
            shouldSettle = true;
            settleReason = BILLING_REASON_MAP_CHANGE;
        }

        // 双轨驱动之二：20 分钟周期轮询静默划扣。
        // 切图结算优先于周期结算，周期心跳不得覆盖已标记的触发源，
        // 否则宽限期补缴会被误标为「周期扣款」而丢失回执语义。
        contract.periodicTimer += diff;
        if (contract.periodicTimer >= PERIODIC_BILLING_INTERVAL_MS)
        {
            contract.periodicTimer = 0;
            if (!shouldSettle)
            {
                shouldSettle = true;
                settleReason = BILLING_REASON_PERIODIC;
            }
        }

        // 1 小时违约宽限期倒计时 + 阶梯催缴 + 资金自愈检测（离线期间不推进，等价于暂停追缴）
        if (contract.inGracePeriod)
        {
            // 资金自愈检测：每 2 秒主动侦测背包。玩家通过任意渠道补足金币
            // （拍卖行结算 / 邮件取款 / 队友交易）后无需干等下一轮 20 分钟周期，
            // 最多 2 秒即自动结清并脱出宽限期，不会因为「刚好卡在结算间隙」
            // 而被误判为恶意欠薪。
            contract.graceRecoveryCheckTimer += diff;
            if (contract.graceRecoveryCheckTimer >= GRACE_RECOVERY_PROBE_MS)
            {
                contract.graceRecoveryCheckTimer = 0;
                if (player->GetMoney() >= contract.pendingCopper)
                {
                    shouldSettle = true;
                    settleReason = BILLING_REASON_RECOVERY;
                }
            }

            if (contract.gracePeriodTimer > diff)
            {
                contract.gracePeriodTimer -= diff;

                // 阶梯催缴：先 30 分钟温和提醒，再 10 分钟红色紧急催缴。
                // 两个标记位保证每档只打扰玩家一次，不做高频刷屏。
                if (contract.gracePeriodTimer <= GRACE_WARN_30MIN_MS && !contract.reminded30Min)
                {
                    contract.reminded30Min = true;
                    send30MinWarning = true;
                    warningDueCopper = contract.pendingCopper;
                }

                if (contract.gracePeriodTimer <= GRACE_WARN_10MIN_MS && !contract.reminded10Min)
                {
                    contract.reminded10Min = true;
                    send10MinWarning = true;
                    warningDueCopper = contract.pendingCopper;
                }
            }
            else
            {
                // 绝杀前终局复核：若此刻资金已补足，立即放行结账；
                // 只有确认无钱才执行遣散与黑名单制裁，杜绝误杀。
                if (player->GetMoney() >= contract.pendingCopper)
                {
                    shouldSettle = true;
                    settleReason = BILLING_REASON_RECOVERY;
                }
                else
                {
                    shouldForceDisband = true;
                    breachDebt = static_cast<uint32>(static_cast<float>(contract.pendingCopper) * BREACH_PENALTY_RATE);
                    _bankruptDebts[guid] = breachDebt;
                    _activeContracts.erase(it);
                    _contractProbeTimers.erase(guid);
                    shouldSettle = false; // 已进入制裁通道，不得再重复划扣
                }
            }
        }
    }

    // 催缴消息一律在锁外下发：ChatHandler 会触发会话层副作用，
    // 持 _lock 期间回调外部代码会引入锁序倒置风险。
    if (send30MinWarning && player->GetSession())
    {
        uint32 const gold = warningDueCopper / 10000;
        uint32 const silver = (warningDueCopper % 10000) / 100;
        uint32 const copper = warningDueCopper % 100;
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffff8000【公会催缴】您的佣金宽限期仅剩 30 分钟，当前欠费: {}金 {}银 {}铜，请尽快筹集资金。|r",
            gold, silver, copper);
    }

    if (send10MinWarning && player->GetSession())
    {
        uint32 const gold = warningDueCopper / 10000;
        uint32 const silver = (warningDueCopper % 10000) / 100;
        uint32 const copper = warningDueCopper % 100;
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffff0000【紧急催缴】您的佣金宽限期仅剩 10 分钟！当前欠费: {}金 {}银 {}铜，超时全队将立即强制解散并执行信用破产制裁！|r",
            gold, silver, copper);
    }

    if (shouldForceDisband)
    {
        CharacterDatabase.Execute(
            "REPLACE INTO character_bot_escrow (guid, is_bankrupt, debt_copper, updated_time) VALUES ({}, 1, {}, {})",
            guid.GetCounter(), breachDebt, static_cast<uint64>(GameTime::GetGameTime().count()));

        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff0000【公会制裁】1 小时宽限期已过，由于恶意欠薪，您的冒险队伍已被强制解散，您已被列入艾泽拉斯公会失信人黑名单！|r");

        // 强制全队归巢：此时契约已被摘除，DoDisband 内部的尾款结清会自动跳过
        BotCommandScript::DoDisband(player);
        return;
    }

    if (shouldSettle)
        SettleCurrentBill(player, settleReason);
}

// -----------------------------------------------------------------------------
// PlayerScript 挂载钩子实现
// -----------------------------------------------------------------------------
BotGuildEscrowPlayerScript::BotGuildEscrowPlayerScript() : PlayerScript("BotGuildEscrowPlayerScript") {}

void BotGuildEscrowPlayerScript::OnPlayerLogin(Player* player)
{
    if (!player)
        return;

    if (!sBotGuildEscrowMgr->IsBankrupt(player->GetGUID()))
        return;

    // 登录即尝试自动清偿：金币不足时 ClearBankruptcy 会给出明确回执，
    // 避免失信黑名单沦为无法解除的永久死结。
    sBotGuildEscrowMgr->ClearBankruptcy(player);

    if (sBotGuildEscrowMgr->IsBankrupt(player->GetGUID()) && player->GetSession())
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffff0000【公会警告】您当前存在未结清的佣兵欠款，处于失信黑名单中，请尽快筹款偿还。|r");
}

void BotGuildEscrowPlayerScript::OnPlayerLogout(Player* player)
{
    if (!player)
        return;

    // 下线闭环：统一走 DoDisband，一次性完成「尾款清算 -> 随从遣返 ->
    // 护卫注销 -> 阵型/休息标记复位」，杜绝玩家离线后随从在副本内变成孤儿实体
    // （既不再跟随、也不再被指挥，还会持续占用实体配额）。
    // 注意：DoDisband 内部是「清算成功才注销契约」，此处不得再无条件 RemoveContract，
    // 否则金币不足的玩家只要登出一次即可赖掉全部欠款。
    BotCommandScript::DoDisband(player);

    // 离线防蒸发兜底：若因金币不足未能当场结清，内存契约此刻仍然存在，
    // 必须立即把欠款固化进数据库。否则服务器在玩家离线期间重启时，
    // 这条仅存于内存的在途账单会随进程一起消失，变相成为「登出逃单」通道。
    if (sBotGuildEscrowMgr->HasActiveContract(player->GetGUID()))
        sBotGuildEscrowMgr->PersistUnpaidDebtOnLogout(player);
}

void BotGuildEscrowPlayerScript::OnPlayerCreatureKill(Player* killer, Creature* killed)
{
    if (!killer || !killed)
        return;

    // 仅在带领随从时计费
    if (sBotGuildEscrowMgr->HasActiveContract(killer->GetGUID()))
        sBotGuildEscrowMgr->AccumulateKillFee(killer, killed);
}

void BotGuildEscrowPlayerScript::OnPlayerUpdate(Player* player, uint32 p_time)
{
    if (!player)
        return;

    sBotGuildEscrowMgr->Update(player, p_time);
}

// -----------------------------------------------------------------------------
// WorldScript：数据库连通后再载入黑名单
// -----------------------------------------------------------------------------
BotGuildEscrowWorldScript::BotGuildEscrowWorldScript() : WorldScript("BotGuildEscrowWorldScript") {}

void BotGuildEscrowWorldScript::OnStartup()
{
    sBotGuildEscrowMgr->LoadBankruptcyFromDB();
    sBotGuildEscrowMgr->LoadGuildMembershipsFromDB();
    sBotGuildEscrowMgr->RedirectGuildPetSpells(); // 动态重定向使魔伴侣法术至独立高段 Entry
}

void AddSC_BotGuildEscrowMgr()
{
    new BotGuildEscrowPlayerScript();
    new BotGuildEscrowWorldScript();
}
