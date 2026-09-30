// Wi-Fi: station for the user's network, access point for setup, SNTP.
#pragma once
#include <Print.h>
#include <stddef.h>
#include <stdint.h>

void netInit();  // once, first: credentials live in our NVS only
void netBegin(const char *ssid, const char *pass);  // background join; no-op if ssid is empty
bool netConnected();
// Blocking join, keeping the access point up if it is. false after timeoutMs.
bool netJoin(const char *ssid, const char *pass, uint32_t timeoutMs);
void netApStart(const char *ssid, const char *pass);
void netApStop();
int netApClients();
int netScan(char names[][33], int max);  // strongest first, no duplicates or hidden names
void netLoop();                          // starts SNTP after the first connection
int64_t netNow();                        // epoch seconds; 0 until SNTP set the clock
void netApName(char *out, size_t cap);   // "swamp-" + last 4 hex of the AP MAC
void netStatusJson(Print &out, const char *state);
