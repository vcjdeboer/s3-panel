// The panel's top-level state. In a header so the prototypes arduino-cli
// generates for s3panel.ino (placed above its first function) can name it.
#pragma once

enum DevState { ST_SETUP, ST_CONNECTING, ST_BADGE, ST_HOST };
