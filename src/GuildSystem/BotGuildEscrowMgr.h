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
#include <unordered_set>

class Player;
class Creature;

// 契合职业位掩码：将 Classes 枚举（1 = 战士 … 11 = 德鲁伊）映射为无符号位。
// 采用内联移位而非查表，保证在 constexpr GUILD_CONFIGS 静态初始化阶段即可求值。
#define GUILD_CLASS_MASK(cls) (1U << ((cls) - 1))

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
    uint32 compatibleClassMask; // 契合职业掩码：非契合职业无法激活战术光环
    uint32 auraSpellId;         // 专属战术光环 SpellID (全队共享，0 为无光环)
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
    uint32 auraSyncTimer{ 0 };          // 公会战术光环动态同步节流计时器 (2000ms)

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
    static constexpr uint32 AURA_SYNC_INTERVAL_MS        = 2000;           // 公会战术光环动态同步周期

    /// @brief 十大冒险者公会专属使魔配置表（creatureEntry 采用全新独立高段 70201-70210，
    ///        与暴雪原生伴侣宠物 Entry 彻底解耦，杜绝任何形式的数据污染）。
    static constexpr std::array<GuildPetConfig, 10> GUILD_CONFIGS =
    {{
        // 1. 探险者: 战/猎/骑/贼 -> 勘探迅捷 (全队地下城移速 +5%)
        { GUILD_EXPLORERS_LEAGUE,     7560,  8496,  70201, "铁炉堡探险者协会",     true,  false,
          GUILD_CLASS_MASK(1) | GUILD_CLASS_MASK(3) | GUILD_CLASS_MASK(2) | GUILD_CLASS_MASK(4), 58857 },

        // 2. 军情七处: 贼/猎/法/术/牧 -> 斩首赏金 (全队 3% 破甲)
        { GUILD_SI7_MERCENARIES,      8491,  10674, 70202, "暴风城军情七处",       true,  false,
          GUILD_CLASS_MASK(4) | GUILD_CLASS_MASK(3) | GUILD_CLASS_MASK(8) | GUILD_CLASS_MASK(9) | GUILD_CLASS_MASK(5), 73878 },

        // 3. 银色盟约: 法/牧/猎/骑 -> 魔枢谐振 (全队法力消耗 -3%)
        { GUILD_SILVER_COVENANT,      8485,  10673, 70203, "达拉然银色盟约",       true,  false,
          GUILD_CLASS_MASK(8) | GUILD_CLASS_MASK(5) | GUILD_CLASS_MASK(3) | GUILD_CLASS_MASK(2), 61316 },

        // 4. 战歌远征队: 战/萨/猎/贼 -> 战歌怒火 (全队物理攻强 +3%)
        { GUILD_WARSONG_OFFENSIVE,    10393, 12643, 70204, "战歌远征突击队",       false, true,
          GUILD_CLASS_MASK(1) | GUILD_CLASS_MASK(7) | GUILD_CLASS_MASK(3) | GUILD_CLASS_MASK(4), 65987 },

        // 5. 夺日者: 法/骑/术/牧 -> 魔能共鸣 (全队法术暴击 +2%)
        { GUILD_SUNREAVERS,           27445, 33050, 70205, "夺日者议会",           false, true,
          GUILD_CLASS_MASK(8) | GUILD_CLASS_MASK(2) | GUILD_CLASS_MASK(9) | GUILD_CLASS_MASK(5), 23645 },

        // 6. 死亡猎手: 贼/术/DK/牧 -> 凋零契约 (全队受暗影/自然伤害降低 5%)
        { GUILD_DEATHSTALKERS,        10392, 12642, 70206, "幽暗城死亡猎手狂怒社", false, true,
          GUILD_CLASS_MASK(4) | GUILD_CLASS_MASK(9) | GUILD_CLASS_MASK(6) | GUILD_CLASS_MASK(5), 32049 },

        // 7. 银色北伐军: 骑/战/牧/DK -> 圣光避难所 (全队受治疗效果 +4%)
        { GUILD_ARGENT_CRUSADE,       44982, 63317, 70207, "银色北伐军先锋营",     false, false,
          GUILD_CLASS_MASK(2) | GUILD_CLASS_MASK(1) | GUILD_CLASS_MASK(5) | GUILD_CLASS_MASK(6), 65634 },

        // 8. 下水道黑市: 全职业开放，无战术光环 (纯个人修理与道具特权)
        { GUILD_UNDERBELLY_SYNDICATE, 43698, 59250, 70208, "达拉然下水道黑市行会", false, false,
          0xFFFFFFFF, 0 },

        // 9. 塞纳里奥: 德/萨/猎 -> 荒野赐福 (全队微量常驻全属性加成)
        { GUILD_CENARION_EXPEDITION,  44794, 61773, 70209, "塞纳里奥议会/远征队",  false, false,
          GUILD_CLASS_MASK(11) | GUILD_CLASS_MASK(7) | GUILD_CLASS_MASK(3), 48470 },

        // 10. 热砂财阀: 全职业开放，无战术光环 (全能商业补位)
        { GUILD_STEAMWHEEDLE_CARTEL,  11026, 13548, 70210, "热砂财阀雇佣行",       false, false,
          0xFFFFFFFF, 0 }
    }};

    static GuildPetConfig const* GetGuildConfig(uint8 guildId);
    static GuildPetConfig const* GetGuildConfigByCreature(uint32 creatureEntry);

    // 随从原生公会反解：由 Phase 1 模版 Entry 公式逆向推导真实会籍
    static uint8 GetBotGuildIdFromEntry(uint32 entry);

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

    /// @brief 职业契合度门禁：判断指定职业是否为该公会认可的正统编制。
    ///        非契合职业加入公会后，自身无法激活战术光环，全队亦不得享受其光环加持。
    static bool IsPlayerClassAffiliated(uint8 guildId, uint8 playerClass);

    /// @brief 查询公会专属战术光环 SpellID（0 表示该公会无战术光环）。
    static uint32 GetGuildAuraSpellId(uint8 guildId);

    /// @brief 全队公会战术光环动态同步：归集队内应生效的光环合集后，对指挥官与
    ///        所有存活随从做差量增删。同公会天然去重（集合语义），异公会完全叠加。
    ///        严禁在持有 _lock 的临界区内调用（内部会遍历 Unit 并施加/移除光环）。
    void UpdateTeamGuildAuras(Player* player);

    /// @brief 剥离全队公会战术光环：退会 / 登出 / 强制遣散等会籍失效路径的收尾动作。
    void ClearTeamGuildAuras(Player* player);

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
    std::unordered_set<ObjectGuid>         _pendingAuraCleanup;  // GUID -> 契约注销后待收尾的残留战术光环标记
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
    void OnMapChanged(Player* player) override; // 监听指挥官跨地图/进出副本事件
};

// 数据库就绪后才载入黑名单：脚本注册阶段（AddSC_*）数据库尚未连通
class BotGuildEscrowWorldScript : public WorldScript
{
public:
    BotGuildEscrowWorldScript();

    void OnStartup() override;
};

void AddSC_BotGuildEscrowMgr();
