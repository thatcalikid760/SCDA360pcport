// xam.xex NetDll_* exports. The offline title starts WSA and XNet (left as logging stubs: they
// fire and 0 = success keeps the menu flow) but has no network: every socket or XNet call fails.
// Every NetDll export takes the caller type in r3; its own arguments start at r4.
#include <runtime.h>

constexpr uint32_t kSocketError = 0xFFFFFFFF, kWsaeNetDown = 10050;

SCDA_IMPORT(NetDll_WSACleanup) { ctx.r3.u64 = 0; }
SCDA_IMPORT(NetDll_WSAGetLastError) { ctx.r3.u64 = kWsaeNetDown; }
SCDA_IMPORT(NetDll_XNetCleanup) { ctx.r3.u64 = 0; }

SCDA_IMPORT(NetDll_socket) { ctx.r3.u64 = kSocketError; } // INVALID_SOCKET
SCDA_IMPORT(NetDll_accept) { ctx.r3.u64 = kSocketError; }
SCDA_IMPORT(NetDll_closesocket) { ctx.r3.u64 = kSocketError; }
SCDA_IMPORT(NetDll_ioctlsocket) { ctx.r3.u64 = kSocketError; }
SCDA_IMPORT(NetDll_setsockopt) { ctx.r3.u64 = kSocketError; }
SCDA_IMPORT(NetDll_getpeername) { ctx.r3.u64 = kSocketError; }
SCDA_IMPORT(NetDll_bind) { ctx.r3.u64 = kSocketError; }
SCDA_IMPORT(NetDll_connect) { ctx.r3.u64 = kSocketError; }
SCDA_IMPORT(NetDll_listen) { ctx.r3.u64 = kSocketError; }
SCDA_IMPORT(NetDll_recv) { ctx.r3.u64 = kSocketError; }
SCDA_IMPORT(NetDll_send) { ctx.r3.u64 = kSocketError; }
SCDA_IMPORT(NetDll_sendto) { ctx.r3.u64 = kSocketError; }
SCDA_IMPORT(NetDll_inet_addr) { ctx.r3.u64 = 0xFFFFFFFF; } // INADDR_NONE

// XNetGetTitleXnAddr(Caller, XNADDR* Addr): XNET_GET_XNADDR_NONE, address zeroed (36 bytes).
SCDA_IMPORT(NetDll_XNetGetTitleXnAddr)
{
    if (ctx.r4.u32)
        for (uint32_t i = 0; i < 36; i += 4)
            GuestStore32(ctx.r4.u32 + i, 0);
    ctx.r3.u64 = 1;
}

SCDA_IMPORT(NetDll_XNetGetConnectStatus) { ctx.r3.u64 = 3; } // XNET_CONNECT_STATUS_LOST
SCDA_IMPORT(NetDll_XNetConnect) { ctx.r3.u64 = kWsaeNetDown; }
SCDA_IMPORT(NetDll_XNetServerToInAddr) { ctx.r3.u64 = kWsaeNetDown; }

// XNetXnAddrToMachineId(Caller, const XNADDR*, u64* MachineId)
SCDA_IMPORT(NetDll_XNetXnAddrToMachineId)
{
    if (ctx.r5.u32)
        GuestStore64(ctx.r5.u32, 0);
    ctx.r3.u64 = kWsaeNetDown;
}
