#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void net_recv_run(void);

#ifndef NET_RECV_SELFTEST
#define NET_RECV_SELFTEST 0
#endif

#if NET_RECV_SELFTEST
void net_recv_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
