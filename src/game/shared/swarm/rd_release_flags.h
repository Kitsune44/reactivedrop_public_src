#pragma once

//#define RD_DISABLE_ALL_RELEASE_FLAGS
#ifndef RD_DISABLE_ALL_RELEASE_FLAGS

// new campaigns sort at the start of the list until some time has passed,
// after which point they are put in approximate order of difficulty
#define RD_NEW_CAMPAIGN_SPOTLIGHT

// new campaigns
//#define RD_6A_CAMPAIGNS_ADANAXIS
//#define RD__CAMPAIGNS_DEADCITY

// 7th anniversary
#define RD_7A_CHATWHEEL
//#define RD_7A_CRAFTING
#define RD_7A_DROPS
//#define RD_7A_ENEMIES
#define RD_7A_LOADOUTS
//#define RD_7A_QUESTS
#define RD_7A_WEAPONS

// features that are not ready for prime time
//#define RD_BONUS_MISSION_ACHIEVEMENTS
//#define RD_SPLITSCREEN_ENABLED
//#define RD_FADE_SINGLE_EDICT

#define RD_IS_RELEASE 0
#else
// new campaigns sort at the start of the list until some time has passed,
// after which point they are put in approximate order of difficulty
#define RD_NEW_CAMPAIGN_SPOTLIGHT

// testing in production, starting january 1st 2024
#define RD_7A_DROPS_PRE
// testing in production, starting december 1st 2024
#define RD_7A_DROPS

#define RD_IS_RELEASE 1
#endif
