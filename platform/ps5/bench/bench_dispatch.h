// The three Spike A benches as callable entry points, plus the title-only
// convert mode.
//
// Each bench keeps its own main() for the payload build (a payload is launched
// with its ELF; nothing selects anything). The one homebrew title links all
// three and calls the entry point the BENCH= selector names
// (platform/ps5/bench/bench_title.cpp).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

int op_audit_run(int argc, char** argv);
int da2_bench_run(int argc, char** argv);
int sharp_bench_run(int argc, char** argv);

// Not a payload bench: converting the inbox is a batch over many photos and
// only exists as a title mode (BENCH=convert), so it has no payload main().
int convert_inbox_run(int argc, char** argv);

// BENCH=http: the sceHttp probe (head, ranged download, bounded text GET). Like
// convert it is title-only -- sceHttp from a *title* is what needs measuring.
int http_probe_run(int argc, char** argv);
