// The setup page, served on the panel's own hotspot at 192.168.4.1. The
// check (join + profile) runs after the reply is sent, because joining the
// home network can move the hotspot's channel and briefly drop the phone.
#pragma once

enum PortalPhase { PORTAL_IDLE, PORTAL_CHECKING, PORTAL_ERROR, PORTAL_DONE };

void portalBegin();  // on a running hotspot; scans networks now
void portalLoop();   // serves requests; runs a pending check (blocking, ~20 s)
void portalEnd();
PortalPhase portalPhase();
// Diagnostics over USB (`setup status`): phase, last message, recent requests.
#include <Print.h>
void portalStatusJson(Print &out);
