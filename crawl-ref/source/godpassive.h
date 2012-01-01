/**
 * @file
 * @brief Passive god effects.
**/

#ifndef GODPASSIVE_H
#define GODPASSIVE_H

#include "mon-info.h"
#include "religion-enum.h"

enum che_boost_type
{
    CB_RNEG,
    CB_RCOLD,
    CB_RFIRE,
    CB_STATS,
    NUM_BOOSTS
};

enum che_change_type
{
    CB_PIETY,     // Change in piety_rank.
    CB_PONDEROUSNESS,   // Change in ponderousness.
    CB_PONDEROUS_COUNT, // Change in number of worn ponderous items.
    CB_SLOTS      // Change in number of slots.
};

enum jiyva_slurp_results
{
        JS_NONE = 0,
        JS_FOOD = 1,
        JS_HP   = 2,
        JS_MP   = 4,
};

int che_boost_level();
int che_boost(che_boost_type bt, int level = che_boost_level());
void che_handle_change(che_change_type ct, int diff);
monster_type che_monster_tier(const monster *mon);
void jiyva_eat_offlevel_items();
void jiyva_slurp_bonus(int item_value, int *js);
void jiyva_slurp_message(int js);
int ash_bondage_level(int type_only = 0);
void ash_check_bondage();
int ash_detect_portals(bool all);
//int ash_skill_boost(skill_type sk);

#endif
