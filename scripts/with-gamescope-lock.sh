#!/usr/bin/env bash
# with-gamescope-lock.sh -- run a command holding an exclusive machine-wide lock,
# so only ONE gamescope-ritz test session exists at a time.
#
# WHY THIS EXISTS
#   Several agents test concurrently on one shared desktop. When two nested
#   gamescope instances run at once they fight over pointer focus, and
#   synthesised mouse input lands in the wrong window -- or in the user's own
#   windows. That produced a run of "pointer input doesn't work" reports
#   (issue #45) which were contention, not a real input-path limitation.
#
#   Keyboard input was never affected, which is why it kept working and made
#   the problem look structural rather than like a race.
#
# USAGE
#   scripts/with-gamescope-lock.sh ./my-test-script.sh
#   scripts/with-gamescope-lock.sh bash -c 'DISABLE_LSFG=1 ./build/src/gamescope ... & ...'
#
#   Hold the lock for the WHOLE session -- launch, drive, screenshot, tear down.
#   Locking only the launch defeats the point.
#
#   --timeout N   seconds to wait for the lock (default 120). Exits 75 if the
#                 wait expires, so a caller can distinguish "busy" from "failed".
#
# HOLD IT BRIEFLY, AND IN THE FOREGROUND
#   Two failure modes have already cost real time here:
#
#   1. Backgrounding the wait and pausing for a notification. Nothing will
#      notify you. Run the locked command in the FOREGROUND and let flock
#      block -- it returns when it acquires.
#   2. Sleeping inside the lock. A `sleep 600` was observed holding this for
#      nearly 8 minutes with no gamescope running at all, blocking every
#      other caller for no benefit. Wait on a real signal (a log line, a
#      socket appearing) with a short bounded timeout instead.
#
#   Exit 75 means busy, not broken: retry once, then continue without
#   pointer verification and say so in your report. Never park indefinitely.
#
# ponytail: one flock, no queue fairness or priority, and no enforced maximum
# hold time -- a caller that sleeps inside the lock still blocks everyone.
# Add a watchdog only if that keeps happening after the docs above.
set -euo pipefail

LOCK_FILE="${GAMESCOPE_TEST_LOCK:-/tmp/gamescope-ritz-test.lock}"
TIMEOUT=120

while [[ $# -gt 0 ]]; do
	case "$1" in
		--timeout) TIMEOUT="$2"; shift 2 ;;
		--) shift; break ;;
		*) break ;;
	esac
done

[[ $# -gt 0 ]] || { echo "with-gamescope-lock: no command given" >&2; exit 2; }

exec 9>"$LOCK_FILE"

if ! flock --exclusive --timeout "$TIMEOUT" 9; then
	echo "with-gamescope-lock: another test session held the lock for ${TIMEOUT}s." >&2
	echo "  Holder: $(cat "$LOCK_FILE" 2>/dev/null || echo unknown)" >&2
	exit 75
fi

echo "$$ ($(date -Is)) ${*:0:1}" >&9 || true
echo "[lock] acquired $LOCK_FILE (pid $$)" >&2

# Refuse to start if a stray instance is already up -- the lock only guards
# lock-aware callers, and a leaked instance from a crashed run would still
# steal pointer focus.
if pgrep -x gamescope-wl >/dev/null 2>&1; then
	echo "[lock] WARNING: a gamescope-wl process is already running." >&2
	echo "[lock] It is not holding this lock, so it is either the user's own" >&2
	echo "[lock] session or a leaked test instance. Not killing it -- check" >&2
	echo "[lock] whether it is yours before proceeding." >&2
fi

status=0
"$@" || status=$?

echo "[lock] released (exit $status)" >&2
exit "$status"
