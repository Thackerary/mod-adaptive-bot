#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
AdaptiveBot - Phase 1 世界常驻随从与公会接待员批量 SQL 生成器

产出（写入 <module>/data/sql/db-world/，连字符目录才会被模块 SQL 升级器扫描）:
  1. base_bot_templates.sql : 310 个公会专属专精模版 (entry 71032 ~ 71341)
  2. base_bot_spawns.sql    : 2400 个常驻世界随从 + 2 名主城公会接待员刷新点

用法:
    python tools/generate_world_bots.py

注意:
  * 随机种子已固定为 FIXED_RANDOM_SEED，故多次重跑产物完全一致（便于 git diff 与线上复现）。
    需要重新洗牌布局时，改大该常量即可。
  * 落点 Z 轴直接取锚点高度，半径 2~18 码散射未做地形贴合；
    若个别点卡进建筑，用 GM 指令 `.npc move` 微调后回写本脚本的 WORLD_HUBS 锚点。
"""

import math
import os
import random

# 固定随机种子：保证多次运行产出完全一致，避免每次重跑都产生两万行无意义 diff
FIXED_RANDOM_SEED = 20250922

# ==============================================================================
# 1. 核心架构映射配置 (严格匹配 AzerothCore 3.3.5a 与 AdaptiveBotLoader)
# ==============================================================================

# 31 专精脚本名与基础属性定义（ScriptName 必须与 AdaptiveBotLoader 注册名逐字一致）
SPECS = [
    # 战士
    {"spec_id": 1, "class_id": 1, "script": "bot_protection_warrior", "role": "TANK", "name": "防御战士"},
    {"spec_id": 2, "class_id": 1, "script": "bot_arms_warrior",       "role": "DPS",  "name": "武器战士"},
    {"spec_id": 3, "class_id": 1, "script": "bot_fury_warrior",       "role": "DPS",  "name": "狂怒战士"},
    # 圣骑士
    {"spec_id": 4, "class_id": 2, "script": "bot_protection_paladin", "role": "TANK", "name": "防护圣骑士"},
    {"spec_id": 5, "class_id": 2, "script": "bot_holy_paladin",       "role": "HEAL", "name": "神圣圣骑士"},
    {"spec_id": 6, "class_id": 2, "script": "bot_retribution_paladin","role": "DPS",  "name": "惩戒圣骑士"},
    # 死亡骑士
    {"spec_id": 7, "class_id": 6, "script": "bot_blood_death_knight", "role": "TANK", "name": "鲜血死亡骑士"},
    {"spec_id": 8, "class_id": 6, "script": "bot_frost_death_knight", "role": "DPS",  "name": "冰霜死亡骑士"},
    {"spec_id": 9, "class_id": 6, "script": "bot_unholy_death_knight","role": "DPS",  "name": "邪恶死亡骑士"},
    # 德鲁伊
    {"spec_id": 10, "class_id": 11, "script": "bot_bear_druid",        "role": "TANK", "name": "守护德鲁伊"},
    {"spec_id": 11, "class_id": 11, "script": "bot_feral_cat_druid",   "role": "DPS",  "name": "野性德鲁伊"},
    {"spec_id": 12, "class_id": 11, "script": "bot_balance_druid",     "role": "DPS",  "name": "平衡德鲁伊"},
    {"spec_id": 13, "class_id": 11, "script": "bot_restoration_druid", "role": "HEAL", "name": "恢复德鲁伊"},
    # 潜行者
    {"spec_id": 14, "class_id": 4, "script": "bot_assassination_rogue","role": "DPS",  "name": "刺杀潜行者"},
    {"spec_id": 15, "class_id": 4, "script": "bot_combat_rogue",       "role": "DPS",  "name": "战斗潜行者"},
    {"spec_id": 16, "class_id": 4, "script": "bot_subtlety_rogue",     "role": "DPS",  "name": "敏锐潜行者"},
    # 猎人
    {"spec_id": 17, "class_id": 3, "script": "bot_beast_mastery_hunter","role": "DPS", "name": "野兽控制猎人"},
    {"spec_id": 18, "class_id": 3, "script": "bot_marksmanship_hunter", "role": "DPS", "name": "射击猎人"},
    {"spec_id": 19, "class_id": 3, "script": "bot_survival_hunter",    "role": "DPS",  "name": "生存猎人"},
    # 萨满祭司
    {"spec_id": 20, "class_id": 7, "script": "bot_elemental_shaman",   "role": "DPS",  "name": "元素萨满"},
    {"spec_id": 21, "class_id": 7, "script": "bot_enhancement_shaman", "role": "DPS",  "name": "增强萨满"},
    {"spec_id": 22, "class_id": 7, "script": "bot_restoration_shaman", "role": "HEAL", "name": "恢复萨满"},
    # 法师
    {"spec_id": 23, "class_id": 8, "script": "bot_arcane_mage",        "role": "DPS",  "name": "奥术法师"},
    {"spec_id": 24, "class_id": 8, "script": "bot_fire_mage",          "role": "DPS",  "name": "火焰法师"},
    {"spec_id": 25, "class_id": 8, "script": "bot_frost_mage",         "role": "DPS",  "name": "冰霜法师"},
    # 牧师
    {"spec_id": 26, "class_id": 5, "script": "bot_discipline_priest",  "role": "HEAL", "name": "戒律牧师"},
    {"spec_id": 27, "class_id": 5, "script": "bot_holy_priest",        "role": "HEAL", "name": "神圣牧师"},
    {"spec_id": 28, "class_id": 5, "script": "bot_shadow_priest",      "role": "DPS",  "name": "暗影牧师"},
    # 术士
    {"spec_id": 29, "class_id": 9, "script": "bot_affliction_warlock", "role": "DPS",  "name": "痛苦术士"},
    {"spec_id": 30, "class_id": 9, "script": "bot_demonology_warlock", "role": "DPS",  "name": "恶魔学识术士"},
    {"spec_id": 31, "class_id": 9, "script": "bot_destruction_warlock","role": "DPS",  "name": "毁灭术士"}
]

# 十大冒险者公会及阵营与专精倾向
GUILDS = [
    {
        "id": 1, "name": "探险者协会", "subname": "铁炉堡探险者协会", "faction_side": "ALLIANCE",
        "hubs": ["Ironforge", "TheStormPeaks", "BoreanTundra_Alliance", "HowlingFjord_Alliance"],
        "preferred_specs": [1, 2, 17, 18, 19, 23, 24, 25]  # 战、猎、法
    },
    {
        "id": 2, "name": "军情七处", "subname": "暴风城军情七处", "faction_side": "ALLIANCE",
        "hubs": ["Stormwind", "Redridge", "Duskwood", "GrizzlyHills_Alliance"],
        "preferred_specs": [14, 15, 16, 17, 18, 19, 28]  # 潜行者、射手、暗牧
    },
    {
        "id": 3, "name": "银色盟约", "subname": "达拉然银色盟约", "faction_side": "ALLIANCE",
        "hubs": ["Dalaran_SilverCovenant", "CrystalsongForest", "Icecrown_Tournament"],
        "preferred_specs": [4, 5, 6, 23, 24, 25, 26, 27]  # 骑、法、牧
    },
    {
        "id": 4, "name": "战歌远征队", "subname": "战歌远征突击队", "faction_side": "HORDE",
        "hubs": ["Orgrimmar", "WarsongHold", "GrizzlyHills_Horde", "Barrens_Crossroads"],
        "preferred_specs": [1, 2, 3, 20, 21, 22, 17, 18]  # 战、萨、猎
    },
    {
        "id": 5, "name": "夺日者", "subname": "夺日者议会", "faction_side": "HORDE",
        "hubs": ["Silvermoon", "Dalaran_Sunreavers", "Icecrown_Tournament"],
        "preferred_specs": [4, 5, 6, 23, 24, 25, 29, 30, 31]  # 骑、法、术
    },
    {
        "id": 6, "name": "死亡猎手", "subname": "幽暗城死亡猎手狂怒社", "faction_side": "HORDE",
        "hubs": ["Undercity", "TarrenMill", "HowlingFjord_Horde", "Dragonblight_Venomspite"],
        "preferred_specs": [7, 8, 9, 14, 15, 16, 28, 29, 30, 31]  # 死骑、刺客、暗术
    },
    {
        "id": 7, "name": "银色北伐军", "subname": "银色北伐军先锋营", "faction_side": "NEUTRAL",
        "hubs": ["LightsHopeChapel", "Icecrown_Vanguard", "Icecrown_Tournament", "ZulDrak_Crusade"],
        "preferred_specs": [4, 5, 6, 7, 8, 9, 26, 27]  # 骑士、死骑、牧师
    },
    {
        "id": 8, "name": "下水道黑市", "subname": "达拉然下水道黑市行会", "faction_side": "NEUTRAL",
        "hubs": ["Dalaran_Underbelly", "Gadgetzan", "BootyBay", "Ratchet"],
        "preferred_specs": [14, 15, 16, 29, 30, 31, 3, 8]  # 刺客、术士、亡命徒
    },
    {
        "id": 9, "name": "塞纳里奥议会", "subname": "塞纳里奥远征队", "faction_side": "NEUTRAL",
        "hubs": ["Moonglade", "CenarionHold", "CenarionRefuge", "BoreanTundra_Cenarion"],
        "preferred_specs": [10, 11, 12, 13, 20, 21, 22, 19]  # 德鲁伊、萨满、生存猎
    },
    {
        "id": 10, "name": "热砂财阀", "subname": "热砂财阀雇佣行", "faction_side": "NEUTRAL",
        "hubs": ["BootyBay", "Gadgetzan", "Ratchet", "Everlook", "Area52"],
        "preferred_specs": list(range(1, 32))  # 全能商业雇佣
    }
]

# 经典地理刷新锚点 (Map, X, Y, Z, O)
WORLD_HUBS = {
    # 联盟主城与核心要地
    "Stormwind":                 {"map": 0, "x": -8833.0, "y": 628.0,   "z": 94.0,  "o": 3.14},
    "Ironforge":                 {"map": 0, "x": -4918.0, "y": -940.0,  "z": 501.5, "o": 5.40},
    "Redridge":                  {"map": 0, "x": -9464.0, "y": -2066.0, "z": 58.3,  "o": 3.14},
    "Duskwood":                  {"map": 0, "x": -10560.0, "y": -1166.0,"z": 27.8,  "o": 0.44},
    "BoreanTundra_Alliance":     {"map": 571, "x": 5800.0, "y": 572.0,  "z": 16.0,  "o": 0.80},
    "HowlingFjord_Alliance":     {"map": 571, "x": 597.0,  "y": -5102.0,"z": 5.2,   "o": 0.00},
    "GrizzlyHills_Alliance":     {"map": 571, "x": 3175.0, "y": -2870.0,"z": 98.0,  "o": 4.20},

    # 部落主城与核心要地
    "Orgrimmar":                 {"map": 1, "x": 1582.0,  "y": -4415.0,"z": 8.0,   "o": 0.50},
    "Undercity":                 {"map": 0, "x": 1618.0,  "y": 236.0,  "z": -52.0, "o": 2.70},
    "Silvermoon":                {"map": 530, "x": 9735.0, "y": -7450.0,"z": 13.5, "o": 3.80},
    "Barrens_Crossroads":        {"map": 1, "x": -440.0,  "y": -2648.0,"z": 95.8,  "o": 1.20},
    "TarrenMill":                {"map": 0, "x": -31.0,   "y": -914.0, "z": 54.8,  "o": 0.90},
    "WarsongHold":               {"map": 571, "x": 2770.0, "y": 6170.0, "z": 54.0,  "o": 3.60},
    "HowlingFjord_Horde":        {"map": 571, "x": 800.0,  "y": -4120.0,"z": 168.0, "o": 2.20},
    "Dragonblight_Venomspite":   {"map": 571, "x": 3760.0, "y": -730.0, "z": 162.0, "o": 1.10},
    "GrizzlyHills_Horde":        {"map": 571, "x": 3780.0, "y": -4150.0,"z": 182.0, "o": 0.30},

    # 达拉然与北裂境核心
    "Dalaran_SilverCovenant":    {"map": 571, "x": 5718.0, "y": 728.0,  "z": 641.5, "o": 4.10},
    "Dalaran_Sunreavers":        {"map": 571, "x": 5925.0, "y": 590.0,  "z": 640.0, "o": 1.50},
    "Dalaran_Underbelly":        {"map": 571, "x": 5835.0, "y": 575.0,  "z": 600.0, "o": 0.80},
    "CrystalsongForest":         {"map": 571, "x": 5600.0, "y": 800.0,  "z": 160.0, "o": 1.20},
    "Icecrown_Tournament":       {"map": 571, "x": 8515.0, "y": 700.0,  "z": 558.0, "o": 3.14},
    "Icecrown_Vanguard":         {"map": 571, "x": 6430.0, "y": 2420.0, "z": 482.0, "o": 5.80},
    "TheStormPeaks":             {"map": 571, "x": 6800.0, "y": -1000.0,"z": 900.0, "o": 0.00},
    "ZulDrak_Crusade":           {"map": 571, "x": 5500.0, "y": -3200.0,"z": 370.0, "o": 2.10},
    "BoreanTundra_Cenarion":     {"map": 571, "x": 4500.0, "y": 5500.0, "z": 70.0,  "o": 4.70},

    # 经典中立与热砂商埠
    "LightsHopeChapel":          {"map": 0, "x": 2280.0,  "y": -5320.0,"z": 88.0,  "o": 4.50},
    "BootyBay":                  {"map": 0, "x": -14300.0,"y": 510.0,  "z": 8.8,   "o": 4.20},
    "Gadgetzan":                 {"map": 1, "x": -7130.0, "y": -3800.0,"z": 8.4,   "o": 2.70},
    "Ratchet":                   {"map": 1, "x": -970.0,  "y": -3730.0,"z": 5.5,   "o": 1.50},
    "Everlook":                  {"map": 1, "x": 6730.0,  "y": -4680.0,"z": 720.0, "o": 4.90},
    "Moonglade":                 {"map": 1, "x": 7800.0,  "y": -2200.0,"z": 460.0, "o": 3.14},
    "CenarionHold":              {"map": 1, "x": -6800.0, "y": 780.0,  "z": 48.0,  "o": 1.10},
    "CenarionRefuge":            {"map": 530, "x": -180.0, "y": 5500.0, "z": 22.0,  "o": 2.50},
    "Area52":                    {"map": 530, "x": 3050.0, "y": 3680.0, "z": 142.0, "o": 5.10}
}

# 随从命名随机词库
FIRST_NAMES = [
    "雷恩", "瓦伦", "萨尔娜", "凯尔", "伊利斯", "莫格", "布兰", "艾琳", "达利安", "泰兰",
    "洛克", "芬娜", "哈罗德", "莉亚", "乌瑟", "安娜", "维克多", "赛拉", "索尔", "卡特琳娜",
    "加文", "贝恩", "希尔", "格罗姆", "德雷克", "米拉", "罗兰", "瑟琳", "杜隆", "埃尔文"
]
TITLES = ["勇者", "老兵", "追寻者", "漫步者", "守卫", "游侠", "学者", "先锋", "督军", "使徒"]

# 阵营与模型定义（若个别 displayId 在你们客户端表现异常，直接替换为常用人形模型即可，
# 例如 19723 / 19724 为人类男/女，19725 / 19726 为兽人男/女）
MODELS_ALLIANCE = [19723, 19724, 20317, 20318]
MODELS_HORDE    = [19725, 19726, 20582, 20583]
MODELS_NEUTRAL  = [19723, 19725, 20317, 20582]

# ==============================================================================
# 2. 模版生成 (310 个公会模版，entry 71032 ~ 71341)
# ==============================================================================
#  entry 公式: 71000 + guild_id * 31 + spec_id
#    guild_id 1  spec_id 1  -> 71032
#    guild_id 10 spec_id 31 -> 71341
BOT_ENTRY_BASE     = 71000
BOT_ENTRY_SLOT     = 31
BOT_ENTRY_MIN      = BOT_ENTRY_BASE + 1 * BOT_ENTRY_SLOT + 1
BOT_ENTRY_MAX      = BOT_ENTRY_BASE + 10 * BOT_ENTRY_SLOT + 31

# creature.guid 规划：900000/900001 归接待员，900002 起为随从
RECEPTIONIST_GUIDS = (900000, 900001)
BOT_GUID_START     = 900002
BOTS_PER_GUILD     = 240
BOT_GUID_END       = BOT_GUID_START + BOTS_PER_GUILD * len(GUILDS) - 1  # 900002 + 2400 - 1


def resolve_entry(guild_id, spec_id):
    return BOT_ENTRY_BASE + guild_id * BOT_ENTRY_SLOT + spec_id


def validate_config():
    """前置校验：避免刷出指向不存在锚点的随从，或专精号越界。"""
    spec_ids = {s["spec_id"] for s in SPECS}
    assert len(spec_ids) == 31, "SPECS 必须正好定义 31 个专精"

    for guild in GUILDS:
        assert guild["hubs"], f"公会 {guild['id']} 未配置任何刷新锚点"
        for hub in guild["hubs"]:
            assert hub in WORLD_HUBS, f"公会 {guild['id']} 引用了未定义的锚点: {hub}"
        for sid in guild["preferred_specs"]:
            assert sid in spec_ids, f"公会 {guild['id']} 引用了未定义的专精 spec_id: {sid}"


def generate_templates_sql(out_path):
    print(">> 正在生成 310 个公会专精 creature_template 模版...")
    lines = [
        "-- AdaptiveBot Phase 1 随从模版数据（自动生成，请勿手工编辑）",
        f"-- entry 区间: {BOT_ENTRY_MIN} ~ {BOT_ENTRY_MAX}",
        "SET FOREIGN_KEY_CHECKS=0;",
        f"DELETE FROM `creature_template` WHERE `entry` BETWEEN {BOT_ENTRY_MIN} AND {BOT_ENTRY_MAX};"
    ]

    for guild in GUILDS:
        gid = guild["id"]
        for spec in SPECS:
            sid = spec["spec_id"]
            entry = resolve_entry(gid, sid)

            if guild["faction_side"] == "ALLIANCE":
                model = random.choice(MODELS_ALLIANCE)
                faction = 35   # 联盟友善
            elif guild["faction_side"] == "HORDE":
                model = random.choice(MODELS_HORDE)
                faction = 190  # 部落友善
            else:
                model = random.choice(MODELS_NEUTRAL)
                faction = 35   # 中立友善（对双方玩家均可交互）

            name = f"{random.choice(FIRST_NAMES)}·{random.choice(TITLES)}"
            subname = f"<{guild['subname']}>"
            script_name = spec["script"]

            lines.append(
                "INSERT INTO `creature_template` "
                "(`entry`, `modelid1`, `name`, `subname`, `IconName`, `minlevel`, `maxlevel`, "
                "`faction`, `npcflag`, `speed_walk`, `speed_run`, `scale`, `rank`, `unit_class`, "
                "`unit_flags`, `type`, `type_flags`, `RegenHealth`, `flags_extra`, `ScriptName`) "
                f"VALUES ({entry}, {model}, '{name}', '{subname}', 'Speak', 80, 80, "
                f"{faction}, 1, 1.0, 1.14286, 1.0, 0, {spec['class_id']}, 0, 7, 0, 1, 0, '{script_name}');"
            )

    lines.append("SET FOREIGN_KEY_CHECKS=1;")
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    print(f">> 模版 SQL 已导出: {out_path}  (共 {BOT_ENTRY_MAX - BOT_ENTRY_MIN + 1} 条)")


def generate_spawns_sql(out_path):
    print(">> 正在基于几何散射算法生成 2400 个常驻世界随从与接待员实体...")
    lines = [
        "-- AdaptiveBot Phase 1 随从与接待员刷新数据（自动生成，请勿手工编辑）",
        f"-- 随从 guid 区间: {BOT_GUID_START} ~ {BOT_GUID_END}",
        "SET FOREIGN_KEY_CHECKS=0;",
        # 随从行先清空；接待员行单独 DELETE，保证与 base_bot_guild_receptionist.sql
        # 重复写入同一主键时不会报 duplicate entry（两文件按字母序后者覆盖前者）。
        f"DELETE FROM `creature` WHERE `guid` BETWEEN {BOT_GUID_START} AND {BOT_GUID_END};",
        f"DELETE FROM `creature` WHERE `guid` IN ({RECEPTIONIST_GUIDS[0]}, {RECEPTIONIST_GUIDS[1]});"
    ]

    creature_columns = (
        "(`guid`, `id1`, `map`, `spawnMask`, `phaseMask`, `position_x`, `position_y`, `position_z`, "
        "`orientation`, `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`, `MovementType`)"
    )

    # ---- 1. 主城公会前台接待员刷新点 (70100 暴风城 / 70101 奥格瑞玛) ----
    receptionists = [
        {"guid": 900000, "entry": 70100, "map": 0, "x": -8854.0, "y": 622.0,   "z": 94.0, "o": 0.60},
        {"guid": 900001, "entry": 70101, "map": 1, "x": 1596.0,  "y": -4400.0, "z": 17.5, "o": 3.20}
    ]
    for rec in receptionists:
        lines.append(
            f"INSERT INTO `creature` {creature_columns} VALUES "
            f"({rec['guid']}, {rec['entry']}, {rec['map']}, 1, 1, {rec['x']:.2f}, {rec['y']:.2f}, {rec['z']:.2f}, "
            f"{rec['o']:.2f}, 300, 0, 0, 50000, 0, 0);"
        )

    # ---- 2. 2400 个常驻随从实体（每公会 240 个，按锚点均分） ----
    spawn_guid = BOT_GUID_START

    for guild in GUILDS:
        gid = guild["id"]
        hubs = guild["hubs"]
        preferred_specs = guild["preferred_specs"]
        bots_per_hub = BOTS_PER_GUILD // len(hubs)
        remainder = BOTS_PER_GUILD % len(hubs)

        for hub_index, hub_name in enumerate(hubs):
            hub_coord = WORLD_HUBS[hub_name]
            count = bots_per_hub + (1 if hub_index < remainder else 0)

            for _ in range(count):
                spec_id = random.choice(preferred_specs)
                entry = resolve_entry(gid, spec_id)

                # 几何圆盘散射：半径 2~18 码随机，角度全域随机，
                # 使同一锚点内的随从呈现自然的营地式散布，而非单点堆叠。
                radius = random.uniform(2.0, 18.0)
                angle = random.uniform(0.0, 2.0 * math.pi)
                x = hub_coord["x"] + radius * math.cos(angle)
                y = hub_coord["y"] + radius * math.sin(angle)
                z = hub_coord["z"]
                orient = random.uniform(0.0, 2.0 * math.pi)

                lines.append(
                    f"INSERT INTO `creature` {creature_columns} VALUES "
                    f"({spawn_guid}, {entry}, {hub_coord['map']}, 1, 1, {x:.2f}, {y:.2f}, {z:.2f}, "
                    f"{orient:.2f}, 120, 0, 0, 20000, 20000, 0);"
                )
                spawn_guid += 1

    lines.append("SET FOREIGN_KEY_CHECKS=1;")
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    print(f">> 实体刷新 SQL 已导出: {out_path}  (共 {spawn_guid - BOT_GUID_START} 个随从 + {len(receptionists)} 名接待员)")


def main():
    random.seed(FIXED_RANDOM_SEED)
    validate_config()

    base_dir = os.path.dirname(os.path.abspath(__file__))
    # 必须使用连字符目录 db-world：模块 SQL 自动升级器只扫描 db-world / db-characters / db-auth
    sql_dir = os.path.abspath(os.path.join(base_dir, "..", "data", "sql", "db-world"))
    os.makedirs(sql_dir, exist_ok=True)

    generate_templates_sql(os.path.join(sql_dir, "base_bot_templates.sql"))
    generate_spawns_sql(os.path.join(sql_dir, "base_bot_spawns.sql"))

    print("\n[SUCCESS] Phase 1 自动化数据库数据批处理生成完毕！")
    print(f"           模版 entry 区间: {BOT_ENTRY_MIN} ~ {BOT_ENTRY_MAX}")
    print(f"           随从 guid 区间: {BOT_GUID_START} ~ {BOT_GUID_END}")


if __name__ == "__main__":
    main()
