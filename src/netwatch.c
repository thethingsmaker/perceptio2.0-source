/* Backend watch — see include/netwatch.h. */
#include "netwatch.h"
#include "net.h"

static netwatch_ok_fn s_ok;
static uint32_t       s_bad_s;

void netwatch_init(netwatch_ok_fn backend_ok) { s_ok = backend_ok; }

void netwatch_tick_1s(void)
{
    bool ok = s_ok ? s_ok() : net_is_connected();
    s_bad_s = ok ? 0 : s_bad_s + 1;
}

uint32_t netwatch_backend_bad_s(void) { return s_bad_s; }
