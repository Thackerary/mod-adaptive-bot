#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
AdaptiveBot - Phase 1 世界常驻随从、装备库与公会接待员全量 SQL 生成器
适配最新 AzerothCore 真实表结构 (creature_template + creature_template_model + creature_equip_template + creature)

产出文件 (自动写入 ../data/sql/db-world/):
  1. base_bot_templates.sql : 310 个专精模版 + 1240 条 creature_template_model 映射 (8 选 4 切片抽样)
  2. base_bot_equipment.sql : 310 个模版对应的 3720 套武器库搭配配置 (每专精 12 套独立方案)
  3. base_bot_spawns.sql    : 2400 个大世界随从 (联盟 1200 / 部落 1200) + 2 名主城公会联络官
"""

import math
import os
import random

FIXED_RANDOM_SEED = 20260924

# 31 专精基础映射 (与 C++ AdaptiveBotLoader 注册名逐字一致)
SPECS = [
    {"spec_id": 1, "class_id": 1, "script": "bot_protection_warrior", "armor": "PLATE", "role": "TANK"},
    {"spec_id": 2, "class_id": 1, "script": "bot_arms_warrior",       "armor": "PLATE", "role": "DPS"},
    {"spec_id": 3, "class_id": 1, "script": "bot_fury_warrior",       "armor": "PLATE", "role": "DPS"},
    {"spec_id": 4, "class_id": 2, "script": "bot_protection_paladin", "armor": "PLATE", "role": "TANK"},
    {"spec_id": 5, "class_id": 2, "script": "bot_holy_paladin",       "armor": "PLATE", "role": "HEAL"},
    {"spec_id": 6, "class_id": 2, "script": "bot_retribution_paladin","armor": "PLATE", "role": "DPS"},
    {"spec_id": 7, "class_id": 6, "script": "bot_blood_death_knight", "armor": "PLATE", "role": "TANK"},
    {"spec_id": 8, "class_id": 6, "script": "bot_frost_death_knight", "armor": "PLATE", "role": "DPS"},
    {"spec_id": 9, "class_id": 6, "script": "bot_unholy_death_knight","armor": "PLATE", "role": "DPS"},
    {"spec_id": 10, "class_id": 11, "script": "bot_bear_druid",        "armor": "LEATHER", "role": "TANK"},
    {"spec_id": 11, "class_id": 11, "script": "bot_feral_cat_druid",   "armor": "LEATHER", "role": "DPS"},
    {"spec_id": 12, "class_id": 11, "script": "bot_balance_druid",     "armor": "CLOTH",   "role": "DPS"},
    {"spec_id": 13, "class_id": 11, "script": "bot_restoration_druid", "armor": "CLOTH",   "role": "HEAL"},
    {"spec_id": 14, "class_id": 4, "script": "bot_assassination_rogue","armor": "LEATHER", "role": "DPS"},
    {"spec_id": 15, "class_id": 4, "script": "bot_combat_rogue",       "armor": "LEATHER", "role": "DPS"},
    {"spec_id": 16, "class_id": 4, "script": "bot_subtlety_rogue",     "armor": "LEATHER", "role": "DPS"},
    {"spec_id": 17, "class_id": 3, "script": "bot_beast_mastery_hunter","armor": "MAIL", "role": "DPS"},
    {"spec_id": 18, "class_id": 3, "script": "bot_marksmanship_hunter", "armor": "MAIL", "role": "DPS"},
    {"spec_id": 19, "class_id": 3, "script": "bot_survival_hunter",    "armor": "MAIL", "role": "DPS"},
    {"spec_id": 20, "class_id": 7, "script": "bot_elemental_shaman",   "armor": "MAIL", "role": "DPS"},
    {"spec_id": 21, "class_id": 7, "script": "bot_enhancement_shaman", "armor": "MAIL", "role": "DPS"},
    {"spec_id": 22, "class_id": 7, "script": "bot_restoration_shaman", "armor": "MAIL", "role": "HEAL"},
    {"spec_id": 23, "class_id": 8, "script": "bot_arcane_mage",        "armor": "CLOTH",  "role": "DPS"},
    {"spec_id": 24, "class_id": 8, "script": "bot_fire_mage",          "armor": "CLOTH",  "role": "DPS"},
    {"spec_id": 25, "class_id": 8, "script": "bot_frost_mage",         "armor": "CLOTH",  "role": "DPS"},
    {"spec_id": 26, "class_id": 5, "script": "bot_discipline_priest",  "armor": "CLOTH",  "role": "HEAL"},
    {"spec_id": 27, "class_id": 5, "script": "bot_holy_priest",        "armor": "CLOTH",  "role": "HEAL"},
    {"spec_id": 28, "class_id": 5, "script": "bot_shadow_priest",      "armor": "CLOTH",  "role": "DPS"},
    {"spec_id": 29, "class_id": 9, "script": "bot_affliction_warlock", "armor": "CLOTH",  "role": "DPS"},
    {"spec_id": 30, "class_id": 9, "script": "bot_demonology_warlock", "armor": "CLOTH",  "role": "DPS"},
    {"spec_id": 31, "class_id": 9, "script": "bot_destruction_warlock","armor": "CLOTH",  "role": "DPS"}
]

SPEC_MAP = {s["spec_id"]: s for s in SPECS}

# 十大公会与阶梯基准等级
GUILDS = [
    {
        "id": 1, "subname": "铁炉堡探险者协会", "faction_side": "ALLIANCE", "base_level": 60,
        "hubs": ["Ironforge", "TheStormPeaks", "BoreanTundra_Alliance", "HowlingFjord_Alliance", "LochModan", "AeriePeak"],
        "preferred_specs": [1, 2, 18, 19, 25]
    },
    {
        "id": 2, "subname": "暴风城军情七处", "faction_side": "ALLIANCE", "base_level": 20,
        "hubs": ["Stormwind", "Goldshire", "Redridge", "Duskwood", "GrizzlyHills_Alliance", "Southshore"],
        "preferred_specs": [14, 15, 16, 18, 28]
    },
    {
        "id": 3, "subname": "达拉然银色盟约", "faction_side": "ALLIANCE", "base_level": 80,
        "hubs": ["Dalaran_SilverCovenant", "Darnassus", "TheExodar", "CrystalsongForest", "Icecrown_Tournament"],
        "preferred_specs": [5, 6, 18, 23, 24]
    },
    {
        "id": 4, "subname": "战歌远征突击队", "faction_side": "HORDE", "base_level": 20,
        "hubs": ["Orgrimmar", "RazorHill", "Barrens_Crossroads", "WarsongHold", "GrizzlyHills_Horde", "Brill"],
        "preferred_specs": [1, 2, 3, 17, 21]
    },
    {
        "id": 5, "subname": "夺日者议会", "faction_side": "HORDE", "base_level": 80,
        "hubs": ["Silvermoon", "Dalaran_Sunreavers", "FalconwingSquare", "Icecrown_Tournament"],
        "preferred_specs": [4, 5, 24, 25, 31]
    },
    {
        "id": 6, "subname": "幽暗城死亡猎手狂怒社", "faction_side": "HORDE", "base_level": 60,
        "hubs": ["Undercity", "ThunderBluff", "TarrenMill", "HowlingFjord_Horde", "Dragonblight_Venomspite", "Bloodhoof"],
        "preferred_specs": [7, 8, 14, 28, 29]
    },
    {
        "id": 7, "subname": "银色北伐军先锋营", "faction_side": "NEUTRAL", "base_level": 80,
        "hubs_alliance": ["LightsHopeChapel", "Icecrown_Vanguard"],
        "hubs_horde": ["Icecrown_Tournament", "ZulDrak_Crusade"],
        "preferred_specs": [1, 4, 5, 27, 9]
    },
    {
        "id": 8, "subname": "达拉然下水道黑市行会", "faction_side": "NEUTRAL", "base_level": 70,
        "hubs_alliance": ["Dalaran_Underbelly", "BootyBay"],
        "hubs_horde": ["Gadgetzan", "Ratchet"],
        "preferred_specs": [15, 16, 20, 30, 31]
    },
    {
        "id": 9, "subname": "塞纳里奥远征队", "faction_side": "NEUTRAL", "base_level": 60,
        "hubs_alliance": ["Moonglade", "CenarionRefuge"],
        "hubs_horde": ["CenarionHold", "BoreanTundra_Cenarion"],
        "preferred_specs": [10, 11, 12, 13, 22]
    },
    {
        "id": 10, "subname": "热砂财阀雇佣行", "faction_side": "NEUTRAL", "base_level": 60,
        "hubs_alliance": ["BootyBay", "Everlook"],
        "hubs_horde": ["Gadgetzan", "Area52"],
        "preferred_specs": list(range(1, 32))
    }
]

# 经典地理锚点 (Map, X, Y, Z, O, indoor)
WORLD_HUBS = {
    # 联盟主要据点
    "Stormwind":                 {"map": 0, "x": -8833.0, "y": 628.0,   "z": 94.0,  "o": 3.14, "indoor": False},
    "Ironforge":                 {"map": 0, "x": -4918.0, "y": -940.0,  "z": 501.5, "o": 5.40, "indoor": True},
    "Darnassus":                 {"map": 1, "x": 9948.0,  "y": 2490.0,  "z": 1316.0,"o": 4.70, "indoor": False},
    "TheExodar":                 {"map": 530,"x": -3960.0, "y": -11640.0,"z": -138.0,"o": 0.0,  "indoor": True},
    "Goldshire":                 {"map": 0, "x": -9464.0, "y": 62.0,    "z": 56.0,  "o": 3.14, "indoor": True},
    "Redridge":                  {"map": 0, "x": -9464.0, "y": -2066.0, "z": 58.3,  "o": 3.14, "indoor": True},
    "Duskwood":                  {"map": 0, "x": -10560.0, "y": -1166.0,"z": 27.8,  "o": 0.44, "indoor": True},
    "Southshore":                {"map": 0, "x": -838.0,  "y": -540.0,  "z": 14.5,  "o": 1.57, "indoor": True},
    "LochModan":                 {"map": 0, "x": -5380.0, "y": -2950.0, "z": 323.0, "o": 3.14, "indoor": True},
    "AeriePeak":                 {"map": 0, "x": 260.0,   "y": -2110.0, "z": 115.0, "o": 0.0,  "indoor": False},
    "BoreanTundra_Alliance":     {"map": 571, "x": 5800.0, "y": 572.0,  "z": 16.0,  "o": 0.80, "indoor": True},
    "HowlingFjord_Alliance":     {"map": 571, "x": 597.0,  "y": -5102.0,"z": 5.2,   "o": 0.00, "indoor": True},
    "GrizzlyHills_Alliance":     {"map": 571, "x": 3175.0, "y": -2870.0,"z": 98.0,  "o": 4.20, "indoor": False},
    "Dalaran_SilverCovenant":    {"map": 571, "x": 5718.0, "y": 728.0,  "z": 641.5, "o": 4.10, "indoor": True},

    # 部落主要据点
    "Orgrimmar":                 {"map": 1, "x": 1582.0,  "y": -4415.0,"z": 8.0,   "o": 0.50, "indoor": False},
    "Undercity":                 {"map": 0, "x": 1618.0,  "y": 236.0,  "z": -52.0, "o": 2.70, "indoor": True},
    "ThunderBluff":              {"map": 1, "x": -1270.0, "y": 115.0,   "z": 131.0, "o": 2.10, "indoor": False},
    "Silvermoon":                {"map": 530, "x": 9735.0, "y": -7450.0,"z": 13.5, "o": 3.80, "indoor": False},
    "RazorHill":                 {"map": 1, "x": 310.0,   "y": -4820.0, "z": 18.0,  "o": 1.57, "indoor": True},
    "Barrens_Crossroads":        {"map": 1, "x": -440.0,  "y": -2648.0,"z": 95.8,  "o": 1.20, "indoor": True},
    "Bloodhoof":                 {"map": 1, "x": -2320.0, "y": -380.0,  "z": -8.0,  "o": 0.0,  "indoor": True},
    "Brill":                     {"map": 0, "x": 2230.0,  "y": 250.0,   "z": 33.5,  "o": 0.50, "indoor": True},
    "TarrenMill":                {"map": 0, "x": -31.0,   "y": -914.0, "z": 54.8,  "o": 0.90, "indoor": True},
    "FalconwingSquare":          {"map": 530, "x": 9470.0, "y": -6850.0,"z": 16.5, "o": 3.14, "indoor": True},
    "WarsongHold":               {"map": 571, "x": 2770.0, "y": 6170.0, "z": 54.0,  "o": 3.60, "indoor": True},
    "HowlingFjord_Horde":        {"map": 571, "x": 800.0,  "y": -4120.0,"z": 168.0, "o": 2.20, "indoor": True},
    "Dragonblight_Venomspite":   {"map": 571, "x": 3760.0, "y": -730.0, "z": 162.0, "o": 1.10, "indoor": False},
    "GrizzlyHills_Horde":        {"map": 571, "x": 3780.0, "y": -4150.0,"z": 182.0, "o": 0.30, "indoor": True},
    "Dalaran_Sunreavers":        {"map": 571, "x": 5925.0, "y": 590.0,  "z": 640.0, "o": 1.50, "indoor": True},

    # 中立战略要地
    "Dalaran_Underbelly":        {"map": 571, "x": 5835.0, "y": 575.0,  "z": 600.0, "o": 0.80, "indoor": True},
    "CrystalsongForest":         {"map": 571, "x": 5600.0, "y": 800.0,  "z": 160.0, "o": 1.20, "indoor": False},
    "Icecrown_Tournament":       {"map": 571, "x": 8515.0, "y": 700.0,  "z": 558.0, "o": 3.14, "indoor": False},
    "Icecrown_Vanguard":         {"map": 571, "x": 6430.0, "y": 2420.0, "z": 482.0, "o": 5.80, "indoor": False},
    "TheStormPeaks":             {"map": 571, "x": 6800.0, "y": -1000.0,"z": 900.0, "o": 0.00, "indoor": False},
    "ZulDrak_Crusade":           {"map": 571, "x": 5500.0, "y": -3200.0,"z": 370.0, "o": 2.10, "indoor": False},
    "BoreanTundra_Cenarion":     {"map": 571, "x": 4500.0, "y": 5500.0, "z": 70.0,  "o": 4.70, "indoor": False},
    "LightsHopeChapel":          {"map": 0, "x": 2280.0,  "y": -5320.0,"z": 88.0,  "o": 4.50, "indoor": False},
    "BootyBay":                  {"map": 0, "x": -14300.0,"y": 510.0,  "z": 8.8,   "o": 4.20, "indoor": True},
    "Gadgetzan":                 {"map": 1, "x": -7130.0, "y": -3800.0,"z": 8.4,   "o": 2.70, "indoor": False},
    "Ratchet":                   {"map": 1, "x": -970.0,  "y": -3730.0,"z": 5.5,   "o": 1.50, "indoor": False},
    "Everlook":                  {"map": 1, "x": 6730.0,  "y": -4680.0,"z": 720.0, "o": 4.90, "indoor": True},
    "Moonglade":                 {"map": 1, "x": 7800.0,  "y": -2200.0,"z": 460.0, "o": 3.14, "indoor": False},
    "CenarionHold":              {"map": 1, "x": -6800.0, "y": 780.0,  "z": 48.0,  "o": 1.10, "indoor": False},
    "CenarionRefuge":            {"map": 530, "x": -180.0, "y": 5500.0, "z": 22.0,  "o": 2.50, "indoor": False},
    "Area52":                    {"map": 530, "x": 3050.0, "y": 3680.0, "z": 142.0, "o": 5.10, "indoor": True}
}

# 10 大冒险者公会独立着装深度切片库 (每个公会 4 甲类 x 8 模型 = 320 个独立 DisplayID)
GUILD_ARMOR_POOLS = {
    # --------------------------------------------------------------------------
    # 1. 铁炉堡探险者协会：矮人山丘步兵、侏儒技师、勘探学者与野外调研装
    # --------------------------------------------------------------------------
    1: {
        # 步兵, 精钢板甲, 山丘重装, 探索者, 矮人狂怒, 铁炉堡老兵, 侏儒战甲, 白银卫士
        "PLATE":   [14732, 26857, 15111, 20387, 26852, 27375, 16223, 21639],
        # 勘探员, 测绘官, 山地火枪手, 侏儒斥候, 机械工程师, 狙击手, 盟军哨兵, 洛丹伦游侠
        "MAIL":    [15112, 27376, 16222, 26850, 20388, 16221, 17009, 26848],
        # 探险旅行装, 地质皮甲, 多功能束带, 追踪者, 侦测皮甲, 潜入者, 外勤特工, 荒野猎手
        "LEATHER": [15112, 27376, 26850, 16222, 20388, 25338, 26848, 16221],
        # 学者长袍, 考古教士, 侏儒机械学者, 肯瑞托研究员, 大教堂主教, 元素法师, 矮人牧师, 秘术师
        "CLOTH":   [20317, 26858, 20386, 19646, 27339, 26851, 15110, 24535]
    },
    # --------------------------------------------------------------------------
    # 2. 暴风城军情七处：黑衣蒙面、暴风城精锐禁卫、军情特工与隐秘暗杀者
    # --------------------------------------------------------------------------
    2: {
        # 精锐卫士, 女骑士, 王室先锋, 白银重铠, 暴风城士官, 卫队队长, 圣光重装, 禁卫破法者
        "PLATE":   [16210, 26779, 17540, 21639, 16211, 17539, 26792, 24536],
        # 潜伏锁铠, 洛丹伦游荡者, 暗夜军情外勤, 伪装斥候, 暗夜哨兵, 外勤特工, 精确射手, 测绘特工
        "MAIL":    [25338, 26848, 21250, 16078, 17009, 25339, 16222, 27376],
        # 经典黑衣刺客, 暗影行者, 迪菲亚夜行衣, 潜伏者, 特工皮甲, 哨兵潜行者, 游荡者, 赏金斥候
        "LEATHER": [25338, 26848, 16078, 21250, 25339, 17009, 22986, 15384],
        # 暗影审讯官, 档案学者, 大教堂密探, 隐秘法师, 肯瑞托法师, 战地军医, 秘法师, 军情破译员
        "CLOTH":   [26788, 20317, 27339, 19646, 24535, 27130, 25078, 20386]
    },
    # --------------------------------------------------------------------------
    # 3. 达拉然银色盟约：高等精灵破法者、奎尔萨拉斯蓝金魔导袍、盟约巡林客
    # --------------------------------------------------------------------------
    3: {
        # 高精破法卫士, 盟约女骑士, 白银使者, 纯白精钢, 暴风城卫士, 辛多雷反叛者, 盟约督军, 寒冰死骑
        "PLATE":   [24536, 26779, 21639, 26792, 16210, 24388, 17540, 26168],
        # 银色盟约游侠(蓝金), 哨兵长, 盟约斥候, 精灵锁甲, 远行者同盟, 锁甲巡逻兵, 密林游侠, 特工射手
        "MAIL":    [24534, 17009, 26848, 24536, 19921, 27341, 21250, 25338],
        # 盟约巡林客, 蓝金轻巧皮铠, 达纳苏斯潜行者, 盟约特工, 密林追踪者, 荒野行者, 精灵斥候, 刺客
        "LEATHER": [24534, 26848, 17009, 25338, 21250, 22986, 19921, 20138],
        # 银色盟约大魔导(蓝金), 肯瑞托法师, 高阶祭司, 达拉然学者, 秘法研究员, 白衣医护, 纯洁法师, 战斗法师
        "CLOTH":   [24535, 19646, 27339, 20317, 26858, 27130, 24424, 25078]
    },
    # --------------------------------------------------------------------------
    # 4. 战歌远征突击队：战歌狼骑兵、库卡隆重装、蛮荒兽骨红甲、萨满战袍
    # --------------------------------------------------------------------------
    4: {
        # 步兵, 库卡隆重装, 战歌钢铁督军, 牛头人重卫, 战歌督军, 蛮荒狂暴者, 蛮牛先锋, 亡灵卫士
        "PLATE":   [14730, 20582, 20583, 15077, 14731, 20584, 26084, 21980],
        # 战歌斥候, 巨魔猎头者, 莫高雷猎鹰, 战歌锁甲, 游侠同盟, 荒原追踪者, 撼地者, 蛮荒链甲
        "MAIL":    [14730, 16522, 26084, 20583, 19921, 20138, 15077, 27341],
        # 蛮荒剥皮手, 巨魔刺客, 荒原猎手, 兽人格斗装, 狂暴皮甲, 丛林斥候, 劫掠者, 暗影行者
        "LEATHER": [20138, 16522, 26084, 14730, 20582, 19921, 15384, 26848],
        # 巫毒萨满, 部落战斗法师, 战歌先祖法衣, 烈焰术士, 部落学者, 通灵学徒, 战地医者, 仪祭师
        "CLOTH":   [19468, 24424, 26788, 25078, 20317, 21980, 27130, 24388]
    },
    # --------------------------------------------------------------------------
    # 5. 夺日者议会：辛多雷血骑士红黑尖角重铠、红金魔导师袍、远行者游侠
    # --------------------------------------------------------------------------
    5: {
        # 血骑士(红黑), 夺日者近卫, 辛多雷督军, 赤红铁铠, 女骑士, 破法重装, 重装巨魔, 圣殿骑士
        "PLATE":   [24388, 24536, 20583, 14730, 26779, 26792, 15077, 21639],
        # 远行者游侠(翠绿红金), 血骑士链甲, 巨魔猎手, 辛多雷火枪手, 游侠锁甲, 斥候, 赤红护胸, 雇佣兵
        "MAIL":    [19921, 24388, 16522, 26084, 24534, 26848, 14730, 27341],
        # 远行者林地皮装, 夺日者刺客, 辛多雷潜伏者, 轻装斥候, 精灵皮甲, 潜入者, 赏金猎人, 游荡者
        "LEATHER": [19921, 20138, 16522, 26084, 24534, 25338, 15384, 26848],
        # 夺日者大魔导师(赤红烈焰), 辛多雷学者, 夺日者祭司, 魔能术士, 烈焰大法师, 肯瑞托学者, 高阶主教, 治愈者
        "CLOTH":   [24424, 25078, 26788, 19468, 24535, 19646, 27339, 27130]
    },
    # --------------------------------------------------------------------------
    # 6. 幽暗城死亡猎手狂怒社：被遗忘者死亡卫士、死疽通灵法衣、枯骨行者
    # --------------------------------------------------------------------------
    6: {
        # 死亡卫士, 黑锋死灵铠, 锈蚀重甲, 枯骨战铠, 堕落十字军, 蛮荒死铠, 凋零巨兽, 黑暗骑士
        "PLATE":   [21980, 26168, 14730, 20582, 26792, 20583, 15077, 24388],
        # 死亡猎手链甲, 腐蚀锁甲, 剧毒斥候, 凋零游侠, 锈蚀锁甲, 枯骨猎人, 伏击者, 佣兵锁甲
        "MAIL":    [20138, 21980, 16522, 19921, 14730, 26084, 26848, 27341],
        # 死亡猎手刺客(黑蒙面), 凋零皮铠, 剧毒潜行者, 枯骨行者, 浸毒特工, 黑暗行者, 剥皮者, 影刃
        "LEATHER": [20138, 26168, 16522, 20582, 25338, 19921, 15384, 26848],
        # 通灵师(死疽长袍), 暗影祭司, 枯萎术士, 亡灵学者, 黑暗秘术师, 堕落教士, 药剂师, 破灭法师
        "CLOTH":   [26788, 19468, 24424, 25078, 19646, 27339, 27130, 24535]
    },
    # --------------------------------------------------------------------------
    # 7. 银色北伐军先锋营：白银十字军战袍、神圣白银板甲、前线救援教士
    # --------------------------------------------------------------------------
    7: {
        # 前锋骑士(白银), 北伐军女骑士, 白银之手圣骑, 忏悔死骑, 联盟重步兵, 部落狂暴者, 矮人圣骑, 牛头人圣阳
        "PLATE":   [26792, 26779, 21639, 26168, 16210, 20582, 14732, 15077],
        # 北伐军游侠锁甲, 圣光勘探官, 远征军哨兵, 白银锁铠, 远行者哨兵, 巨魔猎头者, 荒原猎手, 雇佣神射手
        "MAIL":    [26848, 15112, 17009, 26792, 19921, 16522, 26084, 27341],
        # 先锋侦察皮装, 圣光追踪者, 急救外勤装, 北伐军斥候, 刺客同盟, 丛林斥候, 游林客, 赏金猎人
        "LEATHER": [26848, 15112, 25338, 22986, 20138, 17009, 19921, 15384],
        # 医护修女(白十字长袍), 圣光主教, 战斗法师, 纯洁法师, 烈焰祭司, 圣光神甫, 典籍学者, 部落医师
        "CLOTH":   [27130, 27339, 19646, 24535, 24424, 20317, 26858, 19468]
    },
    # --------------------------------------------------------------------------
    # 8. 达拉然下水道黑市行会：地下拳手、违约打手、黑市掮客保镖、违禁法师
    # --------------------------------------------------------------------------
    8: {
        # 角斗场拳手, 变节禁卫, 违约黑市打手, 蛮荒死斗士, 重装保镖, 堕落骑士, 辛多雷弃徒, 暴徒重装
        "PLATE":   [20582, 16210, 26168, 14730, 15077, 26792, 24388, 21639],
        # 地精防暴锁甲, 下水道大副, 地下锁甲斥候, 凶残佣兵, 飞刀掷弹手, 走私者, 粗砺锁甲, 荒原盗匪
        "MAIL":    [27341, 15383, 26848, 17009, 16522, 19921, 14730, 20138],
        # 违约绞肉刺客(粗粝兜帽), 地精防爆皮衣, 浸毒皮手, 黑市保镖, 地下杀手, 迪菲亚暴徒, 游荡者, 伏击者
        "LEATHER": [26848, 27341, 25338, 15384, 20138, 16078, 22986, 21250],
        # 恶魔契约黑商, 堕落肯瑞托学者, 违禁法术贩子, 遗弃法师, 黑市术士, 暗影巫医, 破落神甫, 地下缝合医
        "CLOTH":   [26788, 20317, 24424, 19646, 25078, 19468, 27339, 27130]
    },
    # --------------------------------------------------------------------------
    # 9. 塞纳里奥远征队：林地守护德鲁伊、自然皮革、角鹰兽羽饰巡林客
    # --------------------------------------------------------------------------
    9: {
        # 雷霆崖大地守护铠, 自然骑士, 山丘石铠, 圣殿守卫, 圣光盟军, 荒野重战士, 翡翠守护者, 花岗岩铠
        "PLATE":   [15077, 26792, 14732, 24388, 16210, 20582, 21639, 26857],
        # 达纳苏斯月神哨兵, 莫高雷猎鹰巡林客, 塞纳里奥斥候, 林地游侠, 飞斧猎人, 勘探者, 追踪游侠, 远征锁甲
        "MAIL":    [17009, 26084, 15481, 19921, 16522, 15112, 26848, 27341],
        # 塞纳里奥自然行者(羽饰皮甲), 巡林德鲁伊, 猛禽德鲁伊, 荒野漫步者, 荒原猎手, 密林追踪者, 游侠皮衣, 旅人
        "LEATHER": [22986, 15481, 17009, 26084, 20138, 25338, 19921, 15384],
        # 远征学者(翠绿长袍), 自然医护者, 植物学家, 树林萨满, 艾露恩女祭司, 翡翠研究员, 博物学者, 星界法师
        "CLOTH":   [15481, 27130, 20317, 19468, 27339, 19646, 26858, 24535]
    },
    # --------------------------------------------------------------------------
    # 10. 热砂财阀雇佣行：藏宝海湾水手服、加基森防风镜、地精重装雇佣兵
    # --------------------------------------------------------------------------
    10: {
        # 雇佣蛮荒重装, 雇佣人类板甲士, 热砂保镖督军, 白银重锤手, 牛头人重保镖, 辛多雷佣兵, 矮人打手, 圣殿雇佣兵
        "PLATE":   [20582, 16210, 14730, 26792, 15077, 24388, 14732, 21639],
        # 藏宝海湾大副(水手帽), 地精齿轮防爆衣, 加基森巡逻兵, 雇佣游侠, 海盗猎手, 飞斧打手, 走私锁甲, 佣兵步兵
        "MAIL":    [15383, 27341, 15384, 19921, 17009, 16522, 26848, 14730],
        # 加基森防风镜皮衣, 水手条纹旅行装, 黑水海盗斥候, 赏金猎人, 地精飞行皮衣, 雇佣刺客, 暴徒, 走私客
        "LEATHER": [15384, 15383, 26848, 25338, 27341, 20138, 16078, 22986],
        # 地精炼金术士, 商业契约学者, 外聘战斗法师, 赏金巫师, 雇佣火法, 佣兵巫医, 战地行医, 商业顾问
        "CLOTH":   [27341, 20317, 19646, 26788, 24424, 19468, 27130, 25078]
    }
}

# 专精分类对应的武器搭配池 (ID, ItemID1, ItemID2, ItemID3) - 每类严格 12 套
EQUIPMENT_POOLS = {
    "TANK": [
        (1, 19352, 19349, 0), (2, 28393, 28606, 0), (3, 40345, 40400, 0),
        (4, 19351, 19348, 0), (5, 28576, 34185, 0), (6, 28263, 28358, 0),
        (7, 34165, 34185, 0), (8, 1728,  17066, 0), (9, 39344, 40400, 0),
        (10, 27901, 28606, 0), (11, 40407, 39281, 0), (12, 37401, 43085, 0)
    ],
    "MELEE_2H": [
        (1, 19364, 0, 0), (2, 12784, 0, 0), (3, 40343, 0, 0),
        (4, 19334, 0, 0), (5, 28773, 0, 0), (6, 32332, 0, 0),
        (7, 28429, 0, 0), (8, 19353, 0, 0), (9, 34247, 0, 0),
        (10, 39417, 0, 0), (11, 17076, 0, 0), (12, 37883, 0, 0)
    ],
    "MELEE_DUAL": [
        (1, 19859, 19347, 0), (2, 19351, 18832, 0), (3, 28295, 28297, 0),
        (4, 28768, 28503, 0), (5, 32262, 32369, 0), (6, 28189, 28189, 0),
        (7, 28307, 28307, 0), (8, 34165, 34165, 0), (9, 39224, 39224, 0),
        (10, 19865, 19866, 0), (11, 37631, 37631, 0), (12, 40383, 40383, 0)
    ],
    "RANGED": [
        (1, 19374, 0, 18713), (2, 24044, 0, 19361), (3, 19347, 19347, 13021),
        (4, 28659, 0, 28772), (5, 28777, 0, 28581), (6, 32254, 0, 32336),
        (7, 34183, 0, 34196), (8, 40497, 0, 40385), (9, 19354, 0, 28772),
        (10, 28429, 0, 28581), (11, 37883, 0, 37191), (12, 39763, 0, 40265)
    ],
    "CASTER_HOLY": [
        (1, 18608, 0, 0), (2, 28604, 0, 0), (3, 19360, 19315, 0),
        (4, 28771, 28603, 0), (5, 34199, 0, 0), (6, 28263, 19348, 0),
        (7, 32353, 0, 0), (8, 40395, 40400, 0), (9, 28522, 0, 0),
        (10, 34212, 34179, 0), (11, 39497, 0, 0), (12, 40396, 40273, 0)
    ],
    "CASTER_DARK": [
        (1, 18609, 0, 0), (2, 18803, 0, 0), (3, 29355, 0, 0),
        (4, 28770, 28734, 0), (5, 32374, 0, 0), (6, 19344, 19366, 0),
        (7, 34336, 0, 0), (8, 40348, 0, 0), (9, 39712, 39712, 0),
        (10, 28782, 28734, 0), (11, 34182, 0, 0), (12, 40244, 40273, 0)
    ],
    "CASTER_ARCANE": [
        (1, 19356, 0, 0), (2, 28782, 0, 0), (3, 19344, 19315, 0),
        (4, 30048, 28603, 0), (5, 34182, 0, 0), (6, 32344, 0, 0),
        (7, 19379, 19315, 0), (8, 28658, 0, 0), (9, 18842, 0, 0),
        (10, 40396, 40273, 0), (11, 39274, 39712, 0), (12, 34176, 34179, 0)
    ]
}

FIRST_NAMES = [
    "雷恩", "瓦伦", "萨尔娜", "凯尔", "伊利斯", "莫格", "布兰", "艾琳", "达利安", "泰兰",
    "洛克", "芬娜", "哈罗德", "莉亚", "乌瑟", "安娜", "维克多", "赛拉", "索尔", "卡特琳娜",
    "加文", "贝恩", "希尔", "格罗姆", "德雷克", "米拉", "罗兰", "瑟琳", "杜隆", "埃尔文"
]
TITLES = ["勇者", "老兵", "追寻者", "漫步者", "守卫", "游侠", "学者", "先锋", "督军", "使徒"]

# ID 空间规划 (严格避开已占用点，落在用户确认的空白区内)
BOT_ENTRY_BASE     = 71000
BOT_ENTRY_SLOT     = 31
BOT_ENTRY_MIN      = BOT_ENTRY_BASE + 1 * BOT_ENTRY_SLOT + 1   # 71032
BOT_ENTRY_MAX      = BOT_ENTRY_BASE + 10 * BOT_ENTRY_SLOT + 31 # 71341

RECEPTIONIST_ENTRIES = (70111, 70112) # 避开 70100，改用安全空白段
RECEPTIONIST_GUIDS   = (4000000, 4000001)
BOT_GUID_START       = 4000002
TOTAL_BOT_COUNT      = 2400
BOTS_PER_GUILD       = TOTAL_BOT_COUNT // len(GUILDS) # 每公会 240
BOT_GUID_END         = BOT_GUID_START + TOTAL_BOT_COUNT - 1


def resolve_entry(guild_id, spec_id):
    return BOT_ENTRY_BASE + guild_id * BOT_ENTRY_SLOT + spec_id


def get_equipment_pool_type(role, spec_id, class_id):
    # 1. 防御坦克（单手 + 盾牌）
    if role == "TANK":
        return "TANK"
    
    # 2. 物理远程（猎人：远程武器 + 近战长柄/双持）
    if spec_id in [17, 18, 19]:
        return "RANGED"
    
    # 3. 近战双持敏捷系（刺杀/战斗/敏锐贼、冰霜DK、增强萨）
    if spec_id in [14, 15, 16, 8, 21]:
        return "MELEE_DUAL"
    
    # 4. 近战双手武器（战士、惩戒骑、邪恶DK、猫德双手长柄/法杖）
    if (role == "DPS" and class_id in [1, 2, 6]) or spec_id == 11:
        return "MELEE_2H"
    
    # 5. 法系 - 纯暗影/恶魔学派（暗牧、痛苦术、恶魔术、毁灭术）
    if spec_id in [28, 29, 30, 31]:
        return "CASTER_DARK"
    
    # 6. 法系 - 纯神圣/自然学派（神圣骑、神牧、戒律牧、恢复萨、恢复德）
    if role == "HEAL":
        return "CASTER_HOLY"
    
    # 7. 法系 - 奥术/火焰/自然元素（三系法师、平衡德、元素萨）
    return "CASTER_ARCANE"


def validate_config():
    spec_ids = {s["spec_id"] for s in SPECS}
    assert len(spec_ids) == 31, "SPECS 必须正好定义 31 个专精"

    for guild in GUILDS:
        hubs = guild.get("hubs", []) + guild.get("hubs_alliance", []) + guild.get("hubs_horde", [])
        assert hubs, f"公会 {guild['id']} 未配置任何刷新锚点"
        for hub in hubs:
            assert hub in WORLD_HUBS, f"公会 {guild['id']} 引用了未定义的锚点: {hub}"
        for sid in guild["preferred_specs"]:
            assert sid in spec_ids, f"公会 {guild['id']} 引用了未定义的专精 spec_id: {sid}"

    # 验证 10 大公会独立模型池完整性 (每个公会 4 甲类且每类至少 8 个模型)
    for gid in range(1, 11):
        assert gid in GUILD_ARMOR_POOLS, f"缺少公会 {gid} 的模型切片池"
        for armor_type in ["PLATE", "MAIL", "LEATHER", "CLOTH"]:
            models = GUILD_ARMOR_POOLS[gid].get(armor_type, [])
            assert len(models) >= 8, f"公会 {gid} 的 {armor_type} 模型池少于 8 个 (当前: {len(models)})"

    # 验证 7 大武器库每类严格包含 12 套配置
    for pool_type, pool in EQUIPMENT_POOLS.items():
        assert len(pool) == 12, f"武器池 {pool_type} 未配置齐 12 套方案 (当前: {len(pool)})"


def generate_templates_and_equipment_sql(templates_path, equipment_path):
    print(">> 正在生成 310 个专精模版、独立模型切片映射与 12 套武器库...")
    t_lines = [
        "-- AdaptiveBot 随从模版数据 (严格适配最新 AC 表结构)",
        f"-- entry 区间: {BOT_ENTRY_MIN} ~ {BOT_ENTRY_MAX}",
        "SET FOREIGN_KEY_CHECKS=0;",
        f"DELETE FROM `creature_template` WHERE `entry` BETWEEN {BOT_ENTRY_MIN} AND {BOT_ENTRY_MAX};",
        f"DELETE FROM `creature_template_model` WHERE `CreatureID` BETWEEN {BOT_ENTRY_MIN} AND {BOT_ENTRY_MAX};"
    ]

    e_lines = [
        "-- AdaptiveBot 随从装备库 (严格使用真实字段: CreatureID, ID, ItemID1..3)",
        f"-- CreatureID 区间: {BOT_ENTRY_MIN} ~ {BOT_ENTRY_MAX}",
        "SET FOREIGN_KEY_CHECKS=0;",
        f"DELETE FROM `creature_equip_template` WHERE `CreatureID` BETWEEN {BOT_ENTRY_MIN} AND {BOT_ENTRY_MAX};"
    ]

    for guild in GUILDS:
        gid = guild["id"]
        base_lvl = guild["base_level"]
        side = guild["faction_side"]

        for spec in SPECS:
            sid = spec["spec_id"]
            entry = resolve_entry(gid, sid)

            if side == "ALLIANCE":
                faction = 35   # 暴风城友善
            elif side == "HORDE":
                faction = 190  # 奥格瑞玛友善
            else:
                faction = 390  # 双阵营中立友善

            name = f"{random.choice(FIRST_NAMES)}·{random.choice(TITLES)}"
            subname = f"<{guild['subname']}>"
            script_name = spec["script"]

            # 1. 写入主表 creature_template (剔除旧 scale/modelid，注入 unit_flags = 512 防野怪攻击)
            t_lines.append(
                "INSERT INTO `creature_template` "
                "(`entry`, `name`, `subname`, `IconName`, `minlevel`, `maxlevel`, "
                "`faction`, `npcflag`, `speed_walk`, `speed_run`, `rank`, `unit_class`, "
                "`unit_flags`, `type`, `type_flags`, `RegenHealth`, `flags_extra`, `ScriptName`) "
                f"VALUES ({entry}, '{name}', '{subname}', 'Speak', {base_lvl}, {base_lvl}, "
                f"{faction}, 1, 1.0, 1.14286, 0, {spec['class_id']}, 512, 7, 0, 1, 0, '{script_name}');"
            )

            # 2. 写入子表 creature_template_model (从公会专属 8 模型池中独立抽取 4 个专属模型)
            armor_type = spec["armor"]
            full_pool = GUILD_ARMOR_POOLS[gid][armor_type]

            spec_rng = random.Random(FIXED_RANDOM_SEED + entry)
            models = spec_rng.sample(full_pool, k=min(4, len(full_pool)))

            for idx, display_id in enumerate(models):
                t_lines.append(
                    "INSERT INTO `creature_template_model` "
                    "(`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) "
                    f"VALUES ({entry}, {idx}, {display_id}, 1.0, 0.25, 0);"
                )

            # 3. 写入装备表 creature_equip_template (写入全部 12 套候选装)
            eq_pool_type = get_equipment_pool_type(spec["role"], sid, spec["class_id"])
            for eq_id, item1, item2, item3 in EQUIPMENT_POOLS[eq_pool_type]:
                e_lines.append(
                    "INSERT INTO `creature_equip_template` (`CreatureID`, `ID`, `ItemID1`, `ItemID2`, `ItemID3`, `VerifiedBuild`) "
                    f"VALUES ({entry}, {eq_id}, {item1}, {item2}, {item3}, 0);"
                )

    t_lines.append("SET FOREIGN_KEY_CHECKS=1;\n")
    e_lines.append("SET FOREIGN_KEY_CHECKS=1;\n")

    with open(templates_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(t_lines))
    print(f">> 模版与模型子表 SQL 已导出: {templates_path} (310 模版 + 1240 模型)")

    with open(equipment_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(e_lines))
    print(f">> 装备搭配库 SQL 已导出: {equipment_path} (3720 套武器配置)")


def generate_spawns_sql(out_path):
    print(">> 正在基于几何散射算法生成 2400 个常驻随从与 2 名接待员...")
    lines = [
        "-- AdaptiveBot Phase 1 随从与接待员刷新数据 (严格适配真实字段: id, wander_distance)",
        f"-- 随从 guid 区间: {BOT_GUID_START} ~ {BOT_GUID_END}",
        "SET FOREIGN_KEY_CHECKS=0;",
        f"DELETE FROM `creature` WHERE `guid` BETWEEN {BOT_GUID_START} AND {BOT_GUID_END};",
        f"DELETE FROM `creature` WHERE `guid` IN ({RECEPTIONIST_GUIDS[0]}, {RECEPTIONIST_GUIDS[1]});"
    ]

    creature_columns = (
        "(`guid`, `id`, `map`, `spawnMask`, `phaseMask`, `equipment_id`, `position_x`, `position_y`, `position_z`, "
        "`orientation`, `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`, `MovementType`, `unit_flags`)"
    )

    # 1. 主城公会联络官刷新点 (70111 暴风城 / 70112 奥格瑞玛)
    receptionists = [
        {"guid": RECEPTIONIST_GUIDS[0], "entry": RECEPTIONIST_ENTRIES[0], "map": 0, "x": -8854.0, "y": 622.0,   "z": 94.0, "o": 0.60},
        {"guid": RECEPTIONIST_GUIDS[1], "entry": RECEPTIONIST_ENTRIES[1], "map": 1, "x": 1596.0,  "y": -4400.0, "z": 17.5, "o": 3.20}
    ]
    for rec in receptionists:
        lines.append(
            f"INSERT INTO `creature` {creature_columns} VALUES "
            f"({rec['guid']}, {rec['entry']}, {rec['map']}, 1, 1, 0, {rec['x']:.2f}, {rec['y']:.2f}, {rec['z']:.2f}, "
            f"{rec['o']:.2f}, 300, 0, 0, 50000, 0, 0, 0);"
        )

    # 2. 2400 个常驻随从实体分发 (阵营 1200:1200 配平)
    spawn_guid = BOT_GUID_START

    for guild in GUILDS:
        gid = guild["id"]
        preferred_specs = guild["preferred_specs"]

        if guild["faction_side"] == "NEUTRAL":
            sub_groups = [
                (guild["hubs_alliance"], BOTS_PER_GUILD // 2),
                (guild["hubs_horde"],    BOTS_PER_GUILD // 2)
            ]
        else:
            sub_groups = [(guild["hubs"], BOTS_PER_GUILD)]

        for hubs, total_guild_bots in sub_groups:
            bots_per_hub = total_guild_bots // len(hubs)
            remainder = total_guild_bots % len(hubs)

            for hub_index, hub_name in enumerate(hubs):
                hub_coord = WORLD_HUBS[hub_name]
                count = bots_per_hub + (1 if hub_index < remainder else 0)
                is_indoor = hub_coord.get("indoor", False)

                for _ in range(count):
                    spec_id = random.choice(preferred_specs)
                    spec_data = SPEC_MAP[spec_id]
                    entry = resolve_entry(gid, spec_id)

                    radius = random.uniform(1.5, 4.8) if is_indoor else random.uniform(3.0, 8.5)
                    angle = random.uniform(0.0, 2.0 * math.pi)
                    x = hub_coord["x"] + radius * math.cos(angle)
                    y = hub_coord["y"] + radius * math.sin(angle)
                    z = hub_coord["z"]
                    orient = random.uniform(0.0, 2.0 * math.pi)
                    
                    # 动态提取对应武器大类并抽取 1~12 号方案
                    eq_pool_type = get_equipment_pool_type(spec_data["role"], spec_id, spec_data["class_id"])
                    equip_id = random.randint(1, len(EQUIPMENT_POOLS[eq_pool_type]))

                    lines.append(
                        f"INSERT INTO `creature` {creature_columns} VALUES "
                        f"({spawn_guid}, {entry}, {hub_coord['map']}, 1, 1, {equip_id}, {x:.2f}, {y:.2f}, {z:.2f}, "
                        f"{orient:.2f}, 120, 0, 0, 20000, 20000, 0, 512);"
                    )
                    spawn_guid += 1

    lines.append("SET FOREIGN_KEY_CHECKS=1;\n")

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    print(f">> 实体刷新 SQL 已导出: {out_path} (2400 随从 + 2 接待员)")


def main():
    random.seed(FIXED_RANDOM_SEED)
    validate_config()

    base_dir = os.path.dirname(os.path.abspath(__file__))
    sql_dir = os.path.abspath(os.path.join(base_dir, "..", "data", "sql", "db-world"))
    os.makedirs(sql_dir, exist_ok=True)

    generate_templates_and_equipment_sql(
        os.path.join(sql_dir, "base_bot_templates.sql"),
        os.path.join(sql_dir, "base_bot_equipment.sql")
    )
    generate_spawns_sql(os.path.join(sql_dir, "base_bot_spawns.sql"))

    print("\n[SUCCESS] 全部三张核心表数据生成完毕！")
    print(f"           模版 Entry 区间 : {BOT_ENTRY_MIN} ~ {BOT_ENTRY_MAX}")
    print(f"           接待员 Entry    : {RECEPTIONIST_ENTRIES}")
    print(f"           实体 GUID 区间  : {BOT_GUID_START} ~ {BOT_GUID_END}")


if __name__ == "__main__":
    main()