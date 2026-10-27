#!/usr/bin/env python3
# Capture one title run: connect to klog, launch through title.sh, read until
# the title's own end line appears (or the window closes), write the capture.
#
# Shared by platform/ps5/scripts/run-viewer.sh and run-vr-hello.sh so there is
# one launch/capture/stop-error contract for both titles. It is not a general
# tool: every setting comes from the environment the caller exports.
#
#   PS5_RUN_TAG      message prefix ("run-viewer", "run-vr-hello")
#   PS5_RUN_HOST     console address (required)
#   PS5_RUN_PORT     klog port
#   PS5_RUN_SECONDS  capture window
#   PS5_RUN_OUT      log file to write
#   PS5_RUN_TITLE_SH title.sh path
#   PS5_RUN_PACKAGE  package folder to launch
#   PS5_RUN_KLOG     klog.sh path (--ensure is run first)
#   PS5_RUN_END      Python regex for the title's last line
#
# Exit: 0 the end line appeared, 1 it did not within the window, 3 no log
# stream, 4 launch failed, 5 the capture stayed empty.
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import re
import signal
import socket
import subprocess
import sys
import time


def main():
    tag = os.environ.get("PS5_RUN_TAG", "title-capture")
    host = os.environ["PS5_RUN_HOST"]
    port = int(os.environ["PS5_RUN_PORT"])
    seconds = float(os.environ["PS5_RUN_SECONDS"])
    out_path = os.environ["PS5_RUN_OUT"]
    title_sh = os.environ["PS5_RUN_TITLE_SH"]
    package = os.environ["PS5_RUN_PACKAGE"]
    klog = os.environ["PS5_RUN_KLOG"]
    end = re.compile(os.environ["PS5_RUN_END"])

    def note(message):
        sys.stderr.write("%s: %s\n" % (tag, message))
        sys.stderr.flush()

    def fail(message, code):
        note(message)
        sys.exit(code)

    ensure = subprocess.run([klog, "--ensure"])
    if ensure.returncode != 0:
        fail("no log stream: klog.sh --ensure exited %d" % ensure.returncode, 3)

    try:
        sock = socket.create_connection((host, port), timeout=5)
    except OSError as error:
        fail("cannot connect to the log stream %s:%d: %s" % (host, port, error), 3)
    sock.settimeout(0.25)
    note("streaming %s:%d for up to %ds (log %s)" % (host, port, int(seconds), out_path))
    log_file = open(out_path, "wb")

    note("launch %s" % package)
    launch = subprocess.run([title_sh, "launch", package], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    for line in launch.stdout.decode("utf-8", "replace").splitlines():
        sys.stderr.write("  launch| %s\n" % line)
    sys.stderr.flush()
    if launch.returncode != 0:
        log_file.close()
        sock.close()
        fail("launch failed (title.sh exit %d)" % launch.returncode, 4)

    def stop(signum, frame):
        raise SystemExit(130)

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    found = None
    pending = ""
    total = 0
    stdout = sys.stdout.buffer
    deadline = time.time() + seconds
    try:
        while time.time() < deadline:
            try:
                data = sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            stdout.write(data)
            stdout.flush()
            log_file.write(data)
            log_file.flush()
            total += len(data)
            pending += data.decode("utf-8", "replace")
            *lines_done, pending = pending.split("\n")
            for line in lines_done:
                if found is None and end.search(line):
                    found = line.strip()
    finally:
        sock.close()
        log_file.close()
        if found is None and pending and end.search(pending):
            found = pending.strip()

    if found is not None:
        note("end line: %s" % found)
        note("captured %d bytes to %s" % (total, out_path))
        sys.exit(0)
    if total == 0:
        note("no log lines at all in %ds (is /dev/klog alive? see klog.sh --ensure)" % int(seconds))
        sys.exit(5)
    note("no title end line in %ds (captured %d bytes to %s)" % (int(seconds), total, out_path))
    sys.exit(1)


if __name__ == "__main__":
    main()
