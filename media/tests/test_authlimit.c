// SPDX-License-Identifier: MIT
#include "jipc.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const uint64_t t = UINT64_C(1000000000), second = UINT64_C(1000000);
    /* Below the limit nothing is refused, and a success resets the count. */
    for (unsigned i = 0; i + 1 < JIPC_AUTH_FAILURES; ++i) jipc_auth_result_at("10.0.0.2", t, 0);
    assert(!jipc_auth_blocked_at("10.0.0.2", t));
    jipc_auth_result_at("10.0.0.2", t, 1);
    for (unsigned i = 0; i + 1 < JIPC_AUTH_FAILURES; ++i) jipc_auth_result_at("10.0.0.2", t + second, 0);
    assert(!jipc_auth_blocked_at("10.0.0.2", t + second));
    /* The limit-th failure blocks this peer only, until its window ends. */
    jipc_auth_result_at("10.0.0.2", t + 2 * second, 0);
    assert(jipc_auth_blocked_at("10.0.0.2", t + 2 * second));
    assert(!jipc_auth_blocked_at("10.0.0.3", t + 2 * second));
    assert(jipc_auth_blocked_at("10.0.0.2", t + second + JIPC_AUTH_WINDOW_US - 1));
    assert(!jipc_auth_blocked_at("10.0.0.2", t + second + JIPC_AUTH_WINDOW_US));
    /* A success during a block does not lift it. */
    for (unsigned i = 0; i < JIPC_AUTH_FAILURES; ++i) jipc_auth_result_at("fd00::5", t, 0);
    jipc_auth_result_at("fd00::5", t, 1);
    assert(jipc_auth_blocked_at("fd00::5", t));
    /* Many peers cannot grow the table: the oldest entry is reused. */
    char peer[32];
    for (unsigned i = 0; i < 100; ++i) {
        snprintf(peer, sizeof(peer), "192.168.1.%u", i);
        jipc_auth_result_at(peer, t + 3 * second + i, 0);
    }
    puts("auth throttle: PASS");
    return 0;
}
