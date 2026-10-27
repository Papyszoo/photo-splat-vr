// Minimal PS5 payload: start line, firmware, one tick per second for 5 s, exit.
// Every line goes to stdout (which elfldr streams back over 9021) and to the
// kernel log via klog_puts (which klogsrv / etaHEN forward to a PC).
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <sys/types.h>
#include <sys/sysctl.h>

#include <ps5/klog.h>

extern "C" int sceKernelGetSystemSwVersion(void *version);
extern "C" int sceKernelUsleep(unsigned int usec);

// Text form ("13.590.001"); 0x100 bytes is well past what the kernel writes.
struct SwVersionText {
    char text[0x100];
};

static void
line(const char *fmt, ...) {
    char    msg[256];
    va_list args;

    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::printf("%s\n", msg);
    std::fflush(stdout);
    klog_puts(msg);
}

int
main() {
    line("hello: start (ps5-splat-vr)");

    uint32_t sdk = 0;
    size_t   len = sizeof(sdk);
    if (sysctlbyname("kern.sdk_version", &sdk, &len, nullptr, 0) == 0 && sdk) {
        line("hello: firmware %x.%02x (kern.sdk_version 0x%08x)",
             sdk >> 24, (sdk >> 16) & 0xff, sdk);
    } else {
        line("hello: firmware unknown (kern.sdk_version unavailable)");
    }

    SwVersionText sw = {};
    if (sceKernelGetSystemSwVersion(&sw) == 0) {
        bool printable = false;
        for (size_t i = 0; i < sizeof(sw.text); i++) {
            if (sw.text[i] == '\0') {
                printable = i > 0;
                break;
            }
            if (sw.text[i] < 0x20 || sw.text[i] >= 0x7f)
                break;
        }
        if (printable)
            line("hello: system sw version %s", sw.text);
    }

    for (int i = 1; i <= 5; i++) {
        line("hello: tick %d/5", i);
        sceKernelUsleep(1000 * 1000);
    }

    line("hello: done, exit 0");
    return 0;
}
