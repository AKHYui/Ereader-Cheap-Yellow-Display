#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void net_ap_run(void);

#ifndef NET_AP_SELFTEST
#define NET_AP_SELFTEST 0
#endif

#if NET_AP_SELFTEST
void net_ap_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
