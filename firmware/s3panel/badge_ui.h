// The badge screens: logo idle; taps cycle profile, badges, activity.
#pragma once
#include "profile_data.h"

void uiBadgeEnter();   // logo, full brightness, timers reset
bool uiBadgeTap();     // wake or advance; false when there is nothing to show
void uiBadgeLoop();    // back to logo after 30 s; dim; redraw on new data
bool uiBadgeOnLogo();
void uiConnecting();
void uiHello(const Profile &p);
