/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "ScriptMgr.h"
#include "ScriptedGossip.h"

class BotGuildReceptionistScript : public CreatureScript
{
public:
    BotGuildReceptionistScript();

    bool OnGossipHello(Player* player, Creature* creature) override;
    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override;

private:
    static void ShowMainMenu(Player* player, Creature* creature);
    static void ShowGuildListMenu(Player* player, Creature* creature);
    static void ShowGuildDetail(Player* player, Creature* creature, uint8 guildId);
    static void HandleJoinGuild(Player* player, Creature* creature, uint8 guildId);
    static void ShowLeaveConfirmMenu(Player* player, Creature* creature);
    static void HandleLeaveGuild(Player* player, Creature* creature);
    static void HandleReissuePet(Player* player, Creature* creature);
};

void AddSC_BotGuildReceptionist();
