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
// ESP-IDF's reason for the last station disconnect since netJoin started; 0 if none.
int netLastReason();
void netApStart(const char *ssid, const char *pass);
void netApStop();
int netApClients();
int netScan(char names[][33], int max);  // blocking; strongest first, no duplicates or hidden names
void netScanStart();                      // background scan
int netScanCollect(char names[][33], int max);  // -1 while running; then as netScan
void netLoop();                          // starts SNTP after the first connection
int64_t netNow();                        // epoch seconds; 0 until SNTP set the clock
void netApName(uint32_t session, char *out, size_t cap);  // "swamp-" + 4 hex, new per setup
void netStatusJson(Print &out, const char *state);
