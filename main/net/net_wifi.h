#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_WIFI_MAX_AP    24
#define NET_WIFI_SSID_LEN  33
#define NET_WIFI_PASS_LEN  65

typedef struct {
    char    ssid[NET_WIFI_SSID_LEN];
    int8_t  rssi;
    bool    secure;
} net_ap_t;

typedef enum {
    NET_WIFI_OFF = 0,
    NET_WIFI_IDLE,
    NET_WIFI_SCANNING,
    NET_WIFI_CONNECTED,
} net_wifi_state_t;

esp_err_t net_wifi_start(void);

bool net_wifi_started(void);

net_wifi_state_t net_wifi_state(void);

const char *net_wifi_state_text(void);

bool net_wifi_connected(void);

int net_wifi_rssi(void);

const char *net_wifi_ssid(void);

const char *net_wifi_ip(void);

int net_wifi_scan(net_ap_t *out, int max);

esp_err_t net_wifi_connect(const char *ssid, const char *pass, bool save);

void net_wifi_disconnect(void);

bool net_wifi_is_saved(const char *ssid);

bool net_wifi_load_saved(char *ssid, size_t n, char *pass, size_t m);

void net_wifi_forget(void);

#define NET_WIFI_AP_PASS      "12345678"
#define NET_WIFI_AP_PASS_MIN  8

esp_err_t net_wifi_ap_start(const char *ssid, const char *pass);

void net_wifi_ap_stop(void);

void net_wifi_shutdown(void);

bool net_wifi_ap_on(void);

const char *net_wifi_ap_ssid(void);

const char *net_wifi_ap_ip(void);

int net_wifi_ap_clients(void);

void net_wifi_ap_default_ssid(char *buf, size_t n);

void net_wifi_mem(const char *where);

#ifndef NET_BOOT_SELFTEST
#define NET_BOOT_SELFTEST 0
#endif

#if NET_BOOT_SELFTEST
void net_wifi_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
