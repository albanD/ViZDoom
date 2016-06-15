/*
 Copyright (C) 2016 by Wojciech Jaśkowski, Michał Kempka, Grzegorz Runc, Jakub Toczek, Marek Wydmuch

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

#include "vizdoom_game.h"
#include "vizdoom_defines.h"
#include "vizdoom_shared_memory.h"
#include "vizdoom_message_queue.h"
#include "vizdoom_screen.h"

#include "d_netinf.h"
#include "d_event.h"
#include "g_game.h"
#include "c_dispatch.h"
#include "p_acs.h"

// New includes

#include "r_state.h"
#include "m_fixed.h"
#include "actor.h"
#include "doomdata.h"

// End of new includes

EXTERN_CVAR (Int, vizdoom_screen_format)
EXTERN_CVAR (Bool, vizdoom_loop_map)

#define VIZDOOM_PLAYER players[consoleplayer]

player_t *vizdoomPlayer;

bip::mapped_region *vizdoomGameVarsSMRegion = NULL;
ViZDoomGameVarsStruct *vizdoomGameVars = NULL;

int ViZDoom_CheckItem(FName name) {
    if(vizdoomPlayer->mo != NULL) {
        AInventory *item = vizdoomPlayer->mo->FindInventory(name);
        if(item != NULL) return item->Amount;
    }
    return 0;
}

int ViZDoom_CheckItem(const PClass *type) {
    if(vizdoomPlayer->mo != NULL) {
        AInventory *item = vizdoomPlayer->mo->FindInventory(type);
        if (item != NULL) return item->Amount;
    }
    return 0;
}

int ViZDoom_CheckWeaponAmmo(AWeapon* weapon){
    if(weapon != NULL) return ViZDoom_CheckItem(weapon->AmmoType1);
    return -1;
}

int ViZDoom_CheckSelectedWeapon(){

    if(vizdoomPlayer->ReadyWeapon == NULL) return -1;

    const PClass *type1 = vizdoomPlayer->ReadyWeapon->GetClass();
    if(type1 == NULL) return -1;

    for(int i=0; i< VIZDOOM_GV_SLOTS_SIZE; ++i){
        for(int j = 0; j < vizdoomPlayer->weapons.Slots[i].Size(); ++j){
            const PClass *type2 = vizdoomPlayer->weapons.Slots[i].GetWeapon(j);
            //if(strcmp(type1->TypeName.GetChars(), type2->TypeName.GetChars()) == 0) return i;
            if(type1 == type2) return i;
        }
    }

    return -1;
}

int ViZDoom_CheckSelectedWeaponAmmo(){
    return ViZDoom_CheckWeaponAmmo(vizdoomPlayer->ReadyWeapon);
}

int ViZDoom_CheckSlotAmmo(int slot){
    if(vizdoomPlayer->weapons.Slots[slot].Size() <= 0) return 0;

    const PClass *typeWeapon = vizdoomPlayer->weapons.Slots[slot].GetWeapon(0);
    AWeapon *weapon = (AWeapon*) typeWeapon->CreateNew();
    //AWeapon *weapon = (AWeapon*)vizdoomPlayer->mo->FindInventory(type);
    if (weapon != NULL){
        const PClass *typeAmmo = weapon->AmmoType1;
        weapon->Destroy();
        return ViZDoom_CheckItem(typeAmmo);
    }
    else return 0;
}

int ViZDoom_CheckSlotWeapons(int slot){
    int inSlot = 0;
    for(int i = 0; i < vizdoomPlayer->weapons.Slots[slot].Size(); ++i){
        const PClass *type = vizdoomPlayer->weapons.Slots[slot].GetWeapon(i);
        inSlot += ViZDoom_CheckItem(type);
    }
    return inSlot;
}

void ViZDoom_GameVarsInit(){

    vizdoomPlayer = &players[consoleplayer];
    try {
        vizdoomGameVarsSMRegion = new bip::mapped_region(vizdoomSM, bip::read_write, 0, sizeof(ViZDoomGameVarsStruct));
        vizdoomGameVars = static_cast<ViZDoomGameVarsStruct *>(vizdoomGameVarsSMRegion->get_address());
    }
    catch(bip::interprocess_exception &ex){
        Printf("ViZDoom_GameVarsInit: Error GameVars SM");
        ViZDoom_MQSend(VIZDOOM_MSG_CODE_DOOM_ERROR);
        exit(1);
    }

    vizdoomGameVars->VERSION = VIZDOOM_VERSION;
    strncpy(vizdoomGameVars->VERSION_STR, VIZDOOM_VERSION_STR, 8);
}

void ViZDoom_GameVarsTic(){

    vizdoomGameVars->GAME_TIC = gametic;
    vizdoomGameVars->GAME_STATE = gamestate;
    vizdoomGameVars->GAME_ACTION = gameaction;
    vizdoomGameVars->GAME_STATIC_SEED = staticrngseed;
    vizdoomGameVars->GAME_SETTINGS_CONTROLLER = vizdoomPlayer->settings_controller;
    vizdoomGameVars->NET_GAME = netgame || multiplayer;

    vizdoomGameVars->SCREEN_WIDTH = vizdoomScreenWidth;
    vizdoomGameVars->SCREEN_HEIGHT = vizdoomScreenHeight;
    vizdoomGameVars->SCREEN_PITCH = vizdoomScreenPitch;
    vizdoomGameVars->SCREEN_SIZE = vizdoomScreenSize;
    vizdoomGameVars->SCREEN_FORMAT = *vizdoom_screen_format;

    vizdoomGameVars->MAP_START_TIC = level.starttime;
    vizdoomGameVars->MAP_TIC = level.maptime;

    vizdoomGameVars->MAP_REWARD = ACS_GlobalVars[0];

    for(int i = 0; i < VIZDOOM_GV_USER_SIZE; ++i){
        vizdoomGameVars->MAP_USER_VARS[i] = ACS_GlobalVars[i+1];
    }

    vizdoomGameVars->PLAYDEMO = gameaction == ga_playdemo;
    vizdoomGameVars->MAP_END = gamestate != GS_LEVEL || gameaction == ga_completed;
    if(vizdoomGameVars->MAP_END) vizdoomGameVars->PLAYER_DEATHCOUNT = 0;
//    if(*vizdoom_loop_map && !level.MapName.Compare(level.NextMap)){
//        level.NextMap = level.MapName;
//        level.NextSecretMap = level.MapName;
//    }

    bool prevDead = vizdoomGameVars->PLAYER_DEAD;

    if(vizdoomPlayer->mo != NULL) {
        vizdoomGameVars->PLAYER_HAS_ACTOR = true;
        vizdoomGameVars->PLAYER_DEAD = vizdoomPlayer->playerstate == PST_DEAD || vizdoomPlayer->mo->health <= 0;
    }
    else {
        vizdoomGameVars->PLAYER_HAS_ACTOR = false;
        vizdoomGameVars->PLAYER_DEAD = true;
    }

    if(vizdoomGameVars->PLAYER_DEAD && !prevDead) ++vizdoomGameVars->PLAYER_DEATHCOUNT;

    strncpy(vizdoomGameVars->PLAYER_NAME, vizdoomPlayer->userinfo.GetName(), VIZDOOM_MAX_PLAYER_NAME_LEN);

    vizdoomGameVars->MAP_KILLCOUNT = level.killed_monsters;
    vizdoomGameVars->MAP_ITEMCOUNT = level.found_items;
    vizdoomGameVars->MAP_SECRETCOUNT = level.found_secrets;

    vizdoomGameVars->PLAYER_KILLCOUNT = vizdoomPlayer->killcount;
    vizdoomGameVars->PLAYER_ITEMCOUNT = vizdoomPlayer->itemcount;
    vizdoomGameVars->PLAYER_SECRETCOUNT = vizdoomPlayer->secretcount;
    vizdoomGameVars->PLAYER_FRAGCOUNT = vizdoomPlayer->fragcount;

    vizdoomGameVars->PLAYER_ATTACK_READY = (vizdoomPlayer->WeaponState & WF_WEAPONREADY);
    vizdoomGameVars->PLAYER_ALTATTACK_READY = (vizdoomPlayer->WeaponState & WF_WEAPONREADYALT);
    vizdoomGameVars->PLAYER_ON_GROUND = vizdoomPlayer->onground;

    if (vizdoomGameVars->PLAYER_HAS_ACTOR) vizdoomGameVars->PLAYER_HEALTH = vizdoomPlayer->mo->health;
    else vizdoomGameVars->PLAYER_HEALTH = vizdoomPlayer->health;

    vizdoomGameVars->PLAYER_ARMOR = ViZDoom_CheckItem(NAME_BasicArmor);

    vizdoomGameVars->PLAYER_SELECTED_WEAPON_AMMO = ViZDoom_CheckSelectedWeaponAmmo();
    vizdoomGameVars->PLAYER_SELECTED_WEAPON = ViZDoom_CheckSelectedWeapon();

    for (int i = 0; i < VIZDOOM_GV_SLOTS_SIZE; ++i) {
        vizdoomGameVars->PLAYER_AMMO[i] = ViZDoom_CheckSlotAmmo(i);
        vizdoomGameVars->PLAYER_WEAPON[i] = ViZDoom_CheckSlotWeapons(i);
    }

    vizdoomGameVars->PLAYER_NUMBER = consoleplayer;
    vizdoomGameVars->PLAYER_NUMBER = 1;
    if(vizdoomGameVars->NET_GAME) {
        vizdoomGameVars->PLAYER_NUMBER = 0;
        for (int i = 0; i < VIZDOOM_MAX_PLAYERS; ++i) {
            strncpy(vizdoomGameVars->PLAYERS_NAME[i], players[i].userinfo.GetName(), VIZDOOM_MAX_PLAYER_NAME_LEN);
            vizdoomGameVars->PLAYERS_FRAGCOUNT[i] = players[i].fragcount;
            if(players[i].mo) ++vizdoomGameVars->PLAYER_NUMBER;
        }
    }

    vizdoomGameVars->WALLS_COUNT = numlines;
    for (int i=0; i<numlines; ++i) {
        vizdoomGameVars->WALLS_POS[i][0][0] = FIXED2FLOAT(lines[i].v1->x);
        vizdoomGameVars->WALLS_POS[i][0][1] = FIXED2FLOAT(lines[i].v1->y);
        vizdoomGameVars->WALLS_POS[i][1][0] = FIXED2FLOAT(lines[i].v2->x);
        vizdoomGameVars->WALLS_POS[i][1][1] = FIXED2FLOAT(lines[i].v2->y);

        bool nonBlocking = 0;
        if (lines[i].backsector && lines[i].frontsector) {
            nonBlocking |= lines[i].backsector->floorplane != lines[i].frontsector->floorplane;
            nonBlocking |= lines[i].backsector->ceilingplane != lines[i].frontsector->ceilingplane;
        }
        vizdoomGameVars->WALLS_NON_BLOCKING[i] = nonBlocking;

        vizdoomGameVars->WALLS_SEEN[i] = bool(lines[i].flags & ML_MAPPED);
    }

    int thingsCount = 0;
    AActor* t;
    for (int i=0; i<numsectors; ++i) {
        t = sectors[i].thinglist;
        while(t) {
            vizdoomGameVars->THINGS_POS[thingsCount][0] = FIXED2FLOAT(t->X());
            vizdoomGameVars->THINGS_POS[thingsCount][1] = FIXED2FLOAT(t->Y());

            vizdoomGameVars->THINGS_ANGLE[thingsCount] = float((double)t->angle / (double)(1u << 31) * (double)180.0);

            vizdoomGameVars->THINGS_TYPE[thingsCount] = t->GetSpecies().GetIndex();
            strncpy(vizdoomGameVars->THINGS_NAME[thingsCount], t->GetSpecies().GetChars(), VIZDOOM_MAX_THINGS_INFO);

            vizdoomGameVars->THINGS_VISIBLE[thingsCount] = t->isVisible;
            t->isVisible = 0;

            ++thingsCount;
            t = t->snext;
        }
    }
    vizdoomGameVars->THINGS_COUNT = thingsCount;

}

void ViZDoom_GameVarsClose(){
    delete vizdoomGameVarsSMRegion;
}



