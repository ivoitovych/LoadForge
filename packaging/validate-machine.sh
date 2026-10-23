#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The scripted half of the hardware validation of docs/hardware-validation.md:
# runs one loadforge binary on this machine through the commands and exit codes
# the checklist lists, keeps every output, and writes the results file the owner
# completes by hand and hands back.
#
#   validate-machine.sh --binary PATH --label MACHINE --flavour static|dynamic|deb
#                       [--out DIR] [--workload-seconds N] [--soak-hours H]
#
#   --binary   the loadforge to validate: the one unpacked from the artifact, or
#              /usr/bin/loadforge after installing the package. Never a build tree.
#   --label    a short name for the machine, letters digits and '-': "thinkpad-t14".
#   --flavour  which artifact the binary came from (recorded, not checked).
#   --out      where the results go (default ./results). The results file is
#              DIR/m7-LABEL-FLAVOUR-YYYY-MM-DD-HHMM-UTC.md and its logs are in
#              the directory of the same name without .md.
#   --workload-seconds   how long each of the eleven workloads runs (default 20;
#              the full validation uses 60).
#   --soak-hours   also run the mixed workload for this many hours and watch the
#              controller's memory, descriptors and threads (default: no soak).
#
# VALIDATE_SOAK_UNIT=m (minutes) and VALIDATE_SOAK_POLL=SECONDS exist to test the soak
# logic in minutes; a real soak leaves them unset (hours, one sample a minute).
#
# Exit code: 0 every scripted step gave the expected exit code and a clean
# verdict; 1 one did not (the results file says which); 2 usage. The script does
# not decide that the machine is good: the results file has a section only a
# person can fill in (which medium, which cooling, what the temperatures did).
set -uo pipefail

usage() {
  sed -n '4,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

binary=""
label=""
flavour=""
out="./results"
seconds=20
soak_hours=0
while [ $# -gt 0 ]; do
  case "$1" in
    --binary) [ $# -ge 2 ] || usage; binary="$2"; shift 2 ;;
    --label) [ $# -ge 2 ] || usage; label="$2"; shift 2 ;;
    --flavour) [ $# -ge 2 ] || usage; flavour="$2"; shift 2 ;;
    --out) [ $# -ge 2 ] || usage; out="$2"; shift 2 ;;
    --workload-seconds) [ $# -ge 2 ] || usage; seconds="$2"; shift 2 ;;
    --soak-hours) [ $# -ge 2 ] || usage; soak_hours="$2"; shift 2 ;;
    *) usage ;;
  esac
done
if [ -z "$binary" ] || [ -z "$label" ] || [ -z "$flavour" ]; then usage; fi
case "$flavour" in static | dynamic | deb) ;; *) usage ;; esac
[[ "$label" =~ ^[A-Za-z0-9-]+$ ]] || { echo "validate-machine: the label is letters, digits and '-'" >&2; exit 2; }
if ! [[ "$seconds" =~ ^[0-9]+$ ]] || [ "$seconds" -lt 5 ]; then
  echo "validate-machine: --workload-seconds is a whole number of at least 5" >&2
  exit 2
fi
[[ "$soak_hours" =~ ^[0-9]+$ ]] || { echo "validate-machine: --soak-hours is a whole number" >&2; exit 2; }
[ -x "$binary" ] || { echo "validate-machine: $binary is not an executable" >&2; exit 2; }
binary="$(cd "$(dirname "$binary")" && pwd)/$(basename "$binary")"

stamp="$(date -u +%Y-%m-%d-%H%M-UTC)"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
name="m7-$label-$flavour-$stamp"
logs="$out/$name"
report="$out/$name.md"
mkdir -p "$logs"

failures=0
rows=""

# step ID EXPECTED_EXIT [--clean] COMMAND...
# Runs the command with its output kept in LOGS/ID.log; records its real exit code and,
# with --clean, that the output holds "verdict: clean".
step() {
  local id="$1" want="$2" clean=0
  shift 2
  if [ "${1:-}" = "--clean" ]; then clean=1; shift; fi
  local started finished code verdict ok
  started="$(date -u +%H:%M:%S)"
  "$@" > "$logs/$id.log" 2>&1
  code=$?
  finished="$(date -u +%H:%M:%S)"
  verdict="-"
  ok="PASS"
  if [ "$code" -ne "$want" ]; then ok="FAIL"; fi
  if [ "$clean" -eq 1 ]; then
    if grep -q '^verdict: clean$' "$logs/$id.log"; then
      verdict="clean"
    else
      verdict="$(grep -m1 '^verdict:' "$logs/$id.log" | sed 's/^verdict: //')"
      verdict="${verdict:-none printed}"
      ok="FAIL"
    fi
  fi
  [ "$ok" = PASS ] || failures=$((failures + 1))
  rows+="| $id | \`${*:1:6}\` | $want | $code | $verdict | $started-$finished | $ok |"$'\n'
  echo "$ok  $id (exit $code, wanted $want, verdict $verdict)"
}

lf="$binary"

# ---- the machine ---------------------------------------------------------
{
  echo '$ uname -a';           uname -a
  echo '$ cat /etc/os-release'; cat /etc/os-release 2>&1
  echo '$ lscpu';              lscpu 2>&1
  echo '$ free -h';            free -h 2>&1
  echo '$ cat /proc/sys/kernel/perf_event_paranoid'; cat /proc/sys/kernel/perf_event_paranoid 2>&1
  echo '$ hwmon chips';        for f in /sys/class/hwmon/hwmon*/name; do echo "$f: $(cat "$f" 2>&1)"; done
  echo '$ thermal zones';      for f in /sys/class/thermal/thermal_zone*/type; do echo "$f: $(cat "$f" 2>&1)"; done
  echo '$ cpufreq governor';   cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>&1
  echo '$ powercap';           ls /sys/class/powercap 2>&1
  echo '$ edac';               ls /sys/devices/system/edac/mc 2>&1
  echo '$ dmidecode (needs root)'; dmidecode -t system -t baseboard -t memory 2>&1 | head -80
  echo '$ id';                 id
} > "$logs/machine.log" 2>&1

# ---- the artifact ---------------------------------------------------------
{
  echo '$ sha256sum'; sha256sum "$lf"
  echo '$ ls -l';     ls -l "$lf"
  echo '$ file';      file "$lf" 2>&1
  echo '$ readelf -d | NEEDED'; readelf -d "$lf" 2>&1 | grep -E 'NEEDED|no dynamic section'
  echo '$ command -v loadforge'; command -v loadforge 2>&1
} > "$logs/artifact.log" 2>&1

# ---- the scripted steps ---------------------------------------------------
step version 0 "$lf" --version
step help 0 "$lf" --help
step selftest 0 "$lf" selftest
step selftest-digest 0 "$lf" --selftest-digest
step usage-error 2 "$lf" nonsense
step unknown-workload 2 "$lf" stress --test nosuch --duration 1s

cat > "$logs/quick.toml" <<'TOML'
schema_version = 1

[run]
mode     = "stress"
duration = "5s"

[[workload]]
name    = "sort"
threads = "all"
TOML
step check-config 0 "$lf" --check-config "$logs/quick.toml"
step stress-config 0 --clean "$lf" stress --config "$logs/quick.toml" --output json,csv --output-dir "$logs"

step capabilities 0 --clean "$lf" stress --test sort --duration 3s

for workload in adaptive_mesh compression dense_linear fft graph monte_carlo particles raytrace sort sparse stencil; do
  mkdir -p "$logs/$workload"
  step "workload-$workload" 0 --clean "$lf" stress --test "$workload" --duration "${seconds}s" \
    --output json,csv --output-dir "$logs/$workload" --journal "$logs/$workload/journal"
done

mkdir -p "$logs/mixed" "$logs/cycling"
rotation=$((seconds / 4))
[ "$rotation" -ge 2 ] || rotation=2
step mixed 0 --clean "$lf" stress --test mixed --duration "$((seconds * 3))s" --rotation "${rotation}s" \
  --output json --output-dir "$logs/mixed" --journal "$logs/mixed/journal"
step cycling 0 --clean "$lf" stress --test cycling --workload dense_linear --duration "$((seconds * 3))s" \
  --dwell "${rotation}s" --output json --output-dir "$logs/cycling" --journal "$logs/cycling/journal"
step journal-clean 0 "$lf" journal "$logs/mixed/journal"
step journal-missing 1 "$lf" journal "$logs/does-not-exist"
step output-dir-missing 1 "$lf" stress --test sort --duration 1s --output json --output-dir "$logs/does-not-exist"

step explore 0 "$lf" explore --objective frequency.busy_ticks:rate:higher \
  --candidate one:sort:1 --candidate all:sort:2 --duration 4s --replicates 1 \
  --final-replicates 2 --reference-replicates 1

# An objective on a metric no machine has is refused before anything runs.
step explore-refused 1 "$lf" explore --objective nosuch.metric:max:higher --candidate a:sort:1

# The controller killed: no worker may outlive it, and the journal must say how it ended.
killed_ok=1
mkdir -p "$logs/killed"
"$lf" stress --test sort --duration 120s --journal "$logs/killed/journal" > "$logs/killed.log" 2>&1 &
controller=$!
sleep 8
before="$(pgrep -P "$controller" | wc -l)"
kill -KILL "$controller"
wait "$controller" 2> /dev/null
sleep 2
left=0
for pid in $(pgrep -x "$(basename "$lf")" || true); do
  # A process of this binary that is a worker of the killed controller.
  if tr '\0' ' ' < "/proc/$pid/cmdline" 2> /dev/null | grep -q -- "--supervisor-pid $controller "; then
    left=$((left + 1))
  fi
done
"$lf" journal "$logs/killed/journal" > "$logs/killed-journal.log" 2>&1
journal_code=$?
grep -q 'ended abnormally' "$logs/killed-journal.log" || killed_ok=0
[ "$left" -eq 0 ] || killed_ok=0
[ "$before" -gt 0 ] || killed_ok=0
if [ "$killed_ok" -eq 1 ] && [ "$journal_code" -eq 0 ]; then
  rows+="| controller-killed | SIGKILL the controller 8 s in | workers left 0 | $left (of $before) | journal: ended abnormally | - | PASS |"$'\n'
  echo "PASS  controller-killed (workers left $left of $before)"
else
  failures=$((failures + 1))
  rows+="| controller-killed | SIGKILL the controller 8 s in | workers left 0 | $left (of $before) | journal exit $journal_code | - | FAIL |"$'\n'
  echo "FAIL  controller-killed (workers left $left of $before, journal exit $journal_code)"
fi

# ---- the soak, when asked ---------------------------------------------------
soak_text=""
if [ "$soak_hours" -gt 0 ]; then
  mkdir -p "$logs/soak"
  "$lf" stress --test mixed --duration "${soak_hours}${VALIDATE_SOAK_UNIT:-h}" --rotation 60s --sample-interval 10s \
    --journal "$logs/soak/journal" --output json --output-dir "$logs/soak" > "$logs/soak.log" 2>&1 &
  soak_pid=$!
  echo "time_utc,rss_kib,fds,threads,processes" > "$logs/soak-monitor.csv"
  while kill -0 "$soak_pid" 2> /dev/null; do
    if [ -r "/proc/$soak_pid/status" ]; then
      rss="$(awk '/^VmRSS:/ {print $2}' "/proc/$soak_pid/status")"
      threads="$(awk '/^Threads:/ {print $2}' "/proc/$soak_pid/status")"
      fds="$(find "/proc/$soak_pid/fd" -mindepth 1 2> /dev/null | wc -l)"
      procs="$(pgrep -P "$soak_pid" | wc -l)"
      echo "$(date -u +%H:%M:%S),${rss:-0},$fds,${threads:-0},$procs" >> "$logs/soak-monitor.csv"
    fi
    sleep "${VALIDATE_SOAK_POLL:-60}"
  done
  wait "$soak_pid"
  soak_code=$?
  # Compare the sample five minutes in (warm) with the last: growth is the finding.
  read -r first_rss first_fds first_threads < <(awk -F, 'NR==7 {print $2, $3, $4}' "$logs/soak-monitor.csv")
  read -r last_rss last_fds last_threads < <(awk -F, 'END {print $2, $3, $4}' "$logs/soak-monitor.csv")
  max_rss="$(awk -F, 'NR>1 && $2>m {m=$2} END {print m+0}' "$logs/soak-monitor.csv")"
  verdict_line="$(grep -m1 '^verdict:' "$logs/soak.log" || echo 'verdict: none printed')"
  soak_ok=PASS
  [ "$soak_code" -eq 0 ] || soak_ok=FAIL
  grep -q '^verdict: clean$' "$logs/soak.log" || soak_ok=FAIL
  if [ -z "${first_rss:-}" ] || [ -z "${last_rss:-}" ]; then
    soak_ok=FAIL
  else
    [ "$last_rss" -le $((first_rss + first_rss / 10 + 4096)) ] || soak_ok=FAIL
    [ "$last_fds" -le "$first_fds" ] || soak_ok=FAIL
    [ "$last_threads" -le "$first_threads" ] || soak_ok=FAIL
  fi
  [ "$soak_ok" = PASS ] || failures=$((failures + 1))
  soak_text="Soak of ${soak_hours} h, mixed workload, rotation 60 s: exit $soak_code, $verdict_line.
Controller at the sixth sample (about five minutes in) and at the end: resident ${first_rss:-?} -> ${last_rss:-?} KiB (peak $max_rss), descriptors ${first_fds:-?} -> ${last_fds:-?}, threads ${first_threads:-?} -> ${last_threads:-?}. Result: $soak_ok. The samples are in soak-monitor.csv."
  echo "$soak_ok  soak ($soak_text)"
fi

# ---- the results file -------------------------------------------------------
digest="$(head -1 "$logs/selftest-digest.log" 2> /dev/null)"
version_text="$("$lf" --version 2>&1)"
sha="$(sha256sum "$lf" | cut -d' ' -f1)"
{
  echo "# Hardware validation: $label, $flavour artifact"
  echo
  echo "**Written:** $(date -u '+%Y-%m-%d %H:%M UTC') (script part; the sections marked OWNER are filled in by hand)"
  echo "**Machine label:** $label"
  echo "**Artifact:** $flavour, $version_text, sha256 $sha"
  echo "**Architecture:** $(uname -m)"
  echo "**Selftest:** $(grep -E '^selftest (passed|FAILED)' "$logs/selftest.log" 2> /dev/null | cut -d. -f1 | head -1)"
  echo "**Selftest digest:** \`$digest\` (must be identical on x86-64 and ARM64 for one version)"
  echo "**Scripted steps:** $([ "$failures" -eq 0 ] && echo 'all passed' || echo "$failures FAILED")"
  echo
  echo "Raw outputs of every step are in the directory \`$name/\` beside this file."
  echo
  echo "## Scripted steps"
  echo
  echo "| Step | Command | Wanted exit | Exit | Verdict | UTC | Result |"
  echo "|---|---|---|---|---|---|---|"
  printf '%s' "$rows"
  echo
  if [ -n "$soak_text" ]; then
    echo "## Soak"
    echo
    echo "$soak_text"
    echo
  fi
  echo "## Machine and artifact"
  echo
  echo '```text'
  sed -n '2p' "$logs/machine.log"
  grep -m1 '^PRETTY_NAME' "$logs/machine.log"
  grep -E '^(Architecture|Model name|CPU\(s\)|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)):' "$logs/machine.log"
  echo '```'
  echo
  echo '```text'
  cat "$logs/artifact.log"
  echo '```'
  echo
  echo "## Capability report of this machine"
  echo
  echo '```text'
  sed -n '/^telemetry capability/,/^loadforge /p' "$logs/capabilities.log" | grep -v '^loadforge ' | grep -E '^telemetry|^  [a-z]+: '
  echo '```'
  echo
  echo "## OWNER: the medium and the observations"
  echo
  echo "- Booted from (live medium, its version, how it was booted; or the installed system):"
  echo "- How the artifact reached the machine (USB stick, network), and the sha256 checked there against the release's SHA256SUMS:"
  echo "- Cooling and ambient (fans heard, laptop on a table, room temperature):"
  echo "- Temperatures seen (idle, under the mixed run, peak) and with what tool:"
  echo "- Anything odd on the console, in dmesg, or on the machine (a fan that changed, a throttle, a shutdown):"
  echo "- Any step above that failed, and what you did about it:"
  echo
  echo "## OWNER: verdict for this machine and artifact"
  echo
  echo "- [ ] Every scripted step passed (or each failure is explained above and filed as an issue)."
  echo "- [ ] No unexplained finding, corrected error, or timed-out worker in any run."
  echo "- [ ] Observations above are filled in."
} > "$report"

echo
echo "results: $report"
[ "$failures" -eq 0 ]
