/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "ScriptMgr.h"
#include "Common.h"
#include "ObjectGuid.h"
#include <array>
#include <mutex>
#include <unordered_map>

class Player;
class Creature;

// 十大冒险者公会枚举
enum AdventureGuildType : uint8
{
    GUILD_NONE                 = 0,
    GUILD_EXPLORERS_LEAGUE     = 1, // 铁炉堡探险者协会 (联盟)
    GUILD_SI7_MERCENARIES      = 2, // 暴风城军情七处 (联盟)
    GUILD_SILVER_COVENANT      = 3, // 达拉然银色盟约 (联盟)
    GUILD_WARSONG_OFFENSIVE    = 4, // 战歌远征突击队 (部落)
    GUILD_SUNREAVERS           = 5, // 夺日者议会 (部落)
    GUILD_DEATHSTALKERS        = 6, // 幽暗城死亡猎手狂怒社 (部落)
    GUILD_ARGENT_CRUSADE       = 7, // 银色北伐军先锋营 (中立)
    GUILD_UNDERBELLY_SYNDICATE = 8, // 达拉然下水道黑市行会 (中立)
    GUILD_CENARION_EXPEDITION  = 9, // 塞纳里奥议会/远征队 (中立)
    GUILD_STEAMWHEEDLE_CARTEL  = 10 // 热砂财阀雇佣行 (中立跨阵营)
};

// 结算触发源枚举
enum BillingReason : uint8
{
    BILLING_REASON_PERIODIC,    // 20 分钟周期轮询静默划扣
    BILLING_REASON_MAP_CHANGE,  // 进出副本切图强制结算
    BILLING_REASON_DISBAND,     // 队伍主动解散最终清算
    BILLING_REASON_RECOVERY,    // 宽限期资金自愈补缴清算
    BILLING_REASON_MANUAL       // 玩家手动结算
};

// 公会使魔配置条目：itemId / spellId / creatureEntry 三元组取自 3.3.5a 真实数据链
struct GuildPetConfig
{
    uint8  guildId;        // 公会 ID (AdventureGuildType)
    uint32 itemId;         // 背包实物道具 ID (item_template)
    uint32 spellId;        // 伴侣召唤法术 ID (Spell.dbc / 法术书)
    uint32 creatureEntry;  // 使魔实体 Entry (creature_template)
    char const* name;      // 公会全称（界面与提示文案）
    bool isAllianceOnly;   // 是否仅招募联盟
    bool isHordeOnly;      // 是否仅招募部落
};

// 单指挥官的内存实时契约记账单
struct BotHireContract
{
    ObjectGuid masterGuid;
    uint32 pendingCopper{ 0 };          // 累积未结清佣金 (铜)
    uint32 totalKilledCount{ 0 };       // 本轮周期击杀统计
    uint32 periodicTimer{ 0 };          // 20 分钟心跳累加器 (ms)
    uint32 lastMapId{ 0xFFFFFFFF };     // 切图结算锚点：上一次观测到的地图 ID
    bool inGracePeriod{ false };        // 是否处于扣费失败宽限期
    uint32 gracePeriodTimer{ 0 };       // 宽限期倒计时 (ms，默认 1 小时)
    uint32 graceRecoveryCheckTimer{ 0 };// 宽限期资金自愈检测节流器 (2000ms)
    bool reminded30Min{ false };        // 30 分钟催缴预警已发送标记
    bool reminded10Min{ false };        // 10 分钟紧急催缴已发送标记

    // 契约独立击杀去重环：PlayerScript 侧与随从 AI 侧是两条独立上报通道，
    // 同一次击杀会被回调两次，必须设闸防双重计费。
    // 关键：该环绝不能做成跨指挥官的全局环——组队时同一只怪的击杀会同时
    // 上报给队内每一位带队玩家，全局环会把第二位及之后的指挥官判为「重复」
    // 而整场免单；下沉到契约后，每位指挥官各自独立判定，互不干扰。
    // 定长环形缓冲零堆分配，32 槽位足以覆盖同帧双路回调的错位窗口。
    std::array<ObjectGuid, 32> recentKilledGuids{};
    uint8 recentKilledIdx{ 0 };

    bool IsDuplicateKill(ObjectGuid const& guid)
    {
        for (auto const& recent : recentKilledGuids)
        {
            if (recent == guid)
                return true;
        }

        recentKilledGuids[recentKilledIdx % recentKilledGuids.size()] = guid;
        ++recentKilledIdx;
        return false;
    }
};

class BotGuildEscrowMgr
{
public:
    static constexpr uint32 PERIODIC_BILLING_INTERVAL_MS = 20 * 60 * 1000; // 20 分钟周期轮询
    static constexpr uint32 GRACE_PERIOD_MS              = 60 * 60 * 1000; // 1 小时欠费宽限 (60分钟)
    static constexpr uint32 CONTRACT_PROBE_INTERVAL_MS   = 1000;           // 契约自愈探针周期
    static constexpr uint32 GRACE_RECOVERY_PROBE_MS      = 2000;           // 宽限期资金自愈检测周期
    static constexpr uint32 GRACE_WARN_30MIN_MS          = 30 * 60 * 1000; // 30 分钟催缴预警阶梯
    static constexpr uint32 GRACE_WARN_10MIN_MS          = 10 * 60 * 1000; // 10 分钟紧急催缴阶梯
    static constexpr float  BREACH_PENALTY_RATE          = 1.15f;          // 违约滞纳金 15%
    static constexpr uint64 DAILY_SUPPLY_COOLDOWN_SECONDS = 20 * 60 * 60;  // 每日补给冷却 (20 小时宽限)

    /// @brief 十大冒险者公会专属使魔配置表（creatureEntry 采用全新独立高段 70201-70210，
    ///        与暴雪原生伴侣宠物 Entry 彻底解耦，杜绝任何形式的数据污染）。
    static constexpr std::array<GuildPetConfig, 10> GUILD_CONFIGS =
    {{
        { GUILD_EXPLORERS_LEAGUE,     7560,  8496,  70201, "铁炉堡探险者协会",     true,  false },
        { GUILD_SI7_MERCENARIES,      8491,  10674, 70202, "暴风城军情七处",       true,  false },
        { GUILD_SILVER_COVENANT,      8485,  10673, 70203, "达拉然银色盟约",       true,  false },
        { GUILD_WARSONG_OFFENSIVE,    10393, 12643, 70204, "战歌远征突击队",       false, true  },
        { GUILD_SUNREAVERS,           27445, 33050, 70205, "夺日者议会",           false, true  },
        { GUILD_DEATHSTALKERS,        10392, 12642, 70206, "幽暗城死亡猎手狂怒社", false, true  },
        { GUILD_ARGENT_CRUSADE,       44982, 63317, 70207, "银色北伐军先锋营",     false, false },
        { GUILD_UNDERBELLY_SYNDICATE, 43698, 59250, 70208, "达拉然下水道黑市行会", false, false },
        { GUILD_CENARION_EXPEDITION,  44794, 61773, 70209, "塞纳里奥议会/远征队",  false, false },
        { GUILD_STEAMWHEEDLE_CARTEL,  11026, 13548, 70210, "热砂财阀雇佣行",       false, false }
    }};

    static GuildPetConfig const* GetGuildConfig(uint8 guildId);
    static GuildPetConfig const* GetGuildConfigByCreature(uint32 creatureEntry);

    static BotGuildEscrowMgr* Instance();

    // 内存契约生命周期
    void StartContract(Player* player);
    void RemoveContract(ObjectGuid const& playerGuid);
    bool HasActiveContract(ObjectGuid const& playerGuid);

    // 核心算费与结算
    void AccumulateKillFee(Player* player, Creature* killed);
    bool SettleCurrentBill(Player* player, BillingReason reason);
    void Update(Player* player, uint32 diff);

    // 信用黑名单审计
    bool IsBankrupt(ObjectGuid const& playerGuid);
    void ClearBankruptcy(Player* player);
    void LoadBankruptcyFromDB();
    void PersistUnpaidDebtOnLogout(Player* player); // 离线欠费防蒸发落盘

    // 会籍管理（公会前台）
    uint8 GetPlayerGuildId(ObjectGuid const& playerGuid);
    bool SetPlayerGuild(Player* player, uint8 guildId);
    bool LeavePlayerGuild(Player* player); // 退会：前置清算 + 彻底销毁使魔
    void LoadGuildMembershipsFromDB();

    /// @brief 服务端伴侣召唤法术重定向：把 10 个公会使魔的召唤目标 Entry
    ///        就地改写为独立高段模版 70201-70210，无需改动客户端 DBC。
    ///        必须在数据库（world）载入完成后的世界启动阶段调用一次。
    void RedirectGuildPetSpells();

    // 每日行军补给
    bool CanClaimDailySupply(ObjectGuid const& playerGuid);
    void RecordDailySupplyClaim(ObjectGuid const& playerGuid);

    // 契约只读快照（供随身使魔终端展示待结佣金与宽限倒计时）
    bool GetContractSnapshot(ObjectGuid const& playerGuid, uint32& pendingCopper, uint32& killedCount,
                             bool& inGracePeriod, uint32& graceRemainingMs);

    // 费率折扣计算
    static float CalculateGuildDiscount(uint8 playerGuildId, uint8 botGuildId, bool isCrossFaction);

private:
    BotGuildEscrowMgr() = default;
    ~BotGuildEscrowMgr() = default;

    std::mutex _lock;
    std::unordered_map<ObjectGuid, BotHireContract> _activeContracts;
    std::unordered_map<ObjectGuid, uint32> _contractProbeTimers; // 开户探针节流器
    std::unordered_map<ObjectGuid, uint32> _bankruptDebts;       // GUID -> 欠款金额(铜)

    std::unordered_map<ObjectGuid, uint8>  _guildMemberships;    // GUID -> 所属公会 ID
    std::unordered_map<ObjectGuid, uint64> _lastSupplyClaimTime; // GUID -> 上次领取补给时间戳
};

#define sBotGuildEscrowMgr BotGuildEscrowMgr::Instance()

// 挂载玩家事件监听器
class BotGuildEscrowPlayerScript : public PlayerScript
{
public:
    BotGuildEscrowPlayerScript();

    void OnPlayerLogin(Player* player) override;
    void OnPlayerLogout(Player* player) override;
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override;
    void OnPlayerUpdate(Player* player, uint32 p_time) override;
};

// 数据库就绪后才载入黑名单：脚本注册阶段（AddSC_*）数据库尚未连通
class BotGuildEscrowWorldScript : public WorldScript
{
public:
    BotGuildEscrowWorldScript();

    void OnStartup() override;
};

void AddSC_BotGuildEscrowMgr();
