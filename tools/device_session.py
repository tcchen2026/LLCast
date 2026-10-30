"""Device-side lifetime and restoration shared by the two display runners.

Generating a script has no side effects. Android service state is read on the
device immediately before the session, so a dry-run prints the actual script.
"""
import shlex

DISPLAY_LOCK = "/data/local/tmp/llstream-display.lock"
BOOT_ID = "/proc/sys/kernel/random/boot_id"
DRM_CLASS = "/sys/class/drm"


def session_script(remote, command, watchdog_seconds, *, display=False, apk=False):
    # Zero means an unlimited streaming session. The exit/signal guard still
    # restores Android and reaps the receiver before releasing display ownership.
    if watchdog_seconds < 0:
        raise ValueError("watchdog must be nonnegative")
    if apk and (not display or watchdog_seconds or command):
        raise ValueError("APK sessions use the fixed unlimited display command")
    # APK mode is compiled into the installed native launcher. Its positional
    # arguments are generated/validated there, never read from a writable script.
    lines = ["#!/system/bin/sh", "set -u", 'lls_remote=$1' if apk else f"lls_remote={shlex.quote(str(remote))}",
             'cd "$lls_remote" || exit 1', "lls_monitor=", "lls_input=", "lls_child=", "lls_watchdog=", "lls_saved=0"]
    if display:
        lines += ["lls_system_server=", "lls_system_server_start=",
                  # A toast/window animation after SF stops can crash system_server
                  # and tear down Wi-Fi. Keep its binder clients alive, but prevent
                  # framework UI work until Android's compositor is restored.
                  "pause_framework() {",
                  "  lls_pid=$(pidof system_server 2>/dev/null || true)",
                  '  case "$lls_pid" in', "    '') return 0 ;;",
                  '    *[!0-9]*|0|1) echo "Cannot identify system_server" >&2; return 1 ;;', "  esac",
                  '  grep -q "^State:[[:space:]]*T" "/proc/$lls_pid/status" && return 0',
                  '  lls_system_server_start=$(cut -d " " -f 22 "/proc/$lls_pid/stat") || return 1',
                  '  lls_system_server=$lls_pid',
                  '  echo "Suspending system_server pid=$lls_system_server start=$lls_system_server_start"',
                  '  kill -STOP "$lls_system_server" || return 1', "  lls_i=0",
                  '  while ! grep -q "^State:[[:space:]]*T" "/proc/$lls_system_server/status"; do',
                  '    [ "$lls_i" -lt 100 ] || { echo "system_server did not stop" >&2; return 1; }',
                  '    kill -0 "$lls_system_server" 2>/dev/null || return 1',
                  "    sleep 0.02", "    lls_i=$((lls_i+1))", "  done", "}",
                  "resume_framework() {",
                  '  if [ -n "$lls_system_server" ] &&',
                  '     [ "$(cut -d " " -f 22 "/proc/$lls_system_server/stat" 2>/dev/null)" = "$lls_system_server_start" ]; then',
                  '    echo "Resuming system_server pid=$lls_system_server"',
                  '    kill -CONT "$lls_system_server" 2>/dev/null || true', "  fi",
                  "  lls_system_server=", "}",
                  f"lls_lock={shlex.quote(DISPLAY_LOCK)}",
                  f"lls_boot=$(cat {BOOT_ID}) || exit 1",
                  "lock_busy() {", '  echo "Another LLCast display session holds $lls_lock" >&2',
                  '  echo 75 > status', "  exit 75", "}",
                  'if ! mkdir "$lls_lock" 2>/dev/null; then',
                  '  lls_old_boot=$(cat "$lls_lock/boot_id" 2>/dev/null || true)',
                  '  lls_old_pid=$(cat "$lls_lock/pid" 2>/dev/null || true)', "  lls_stale=0",
                  '  if [ -n "$lls_old_boot" ] && [ "$lls_old_boot" != "$lls_boot" ]; then lls_stale=1; fi',
                  '  case "$lls_old_pid" in', "    ''|*[!0-9]*|0) ;;",
                  '    *) kill -0 "$lls_old_pid" 2>/dev/null || lls_stale=1 ;;', "  esac",
                  '  [ "$lls_stale" = 1 ] || lock_busy',
                  '  echo "Removing stale display lock (boot=$lls_old_boot pid=$lls_old_pid)" >&2',
                  '  rm -f "$lls_lock/pid" "$lls_lock/boot_id"',
                  '  rmdir "$lls_lock" 2>/dev/null || lock_busy',
                  '  mkdir "$lls_lock" 2>/dev/null || lock_busy', "fi",
                  'echo "$$" > "$lls_lock/pid"', 'echo "$lls_boot" > "$lls_lock/boot_id"']
    lines += [
        'echo "$$" > session.pid',
        # A cancellable subshell owns its sleep process too. Killing toybox
        # timeout itself does not reliably forward TERM to the receiver.
        "watchdog() {", "  lls_sleep=",
        "  trap 'exit 0' INT TERM HUP",
        "  trap '[ -z \"$lls_sleep\" ] || { kill \"$lls_sleep\" 2>/dev/null; wait \"$lls_sleep\" 2>/dev/null; }' EXIT",
        '  if [ "$1" -gt 0 ]; then',
        '    sleep "$1" &', "    lls_sleep=$!", '    wait "$lls_sleep" || exit 0', "    lls_sleep=",
        "    echo 1 > watchdog-expired", '    kill -TERM "$lls_child" 2>/dev/null || exit 0', "  fi",
        "  sleep 3 &", "  lls_sleep=$!", '  wait "$lls_sleep" || exit 0', "  lls_sleep=",
        '  kill -KILL "$lls_child" 2>/dev/null || true', "}",
        "cancel_watchdog() {", '  if [ -n "$lls_watchdog" ]; then',
        '    kill -TERM "$lls_watchdog" 2>/dev/null || true',
        '    wait "$lls_watchdog" 2>/dev/null || true', "    lls_watchdog=", "  fi", "}",
        "restore() {", "  lls_result=$?", "  trap '' INT TERM HUP", "  trap - EXIT",
        '  if [ -n "$lls_input" ]; then',
        '    kill -TERM "$lls_input" 2>/dev/null || true',
        '    wait "$lls_input" 2>/dev/null || true', "    lls_input=", "  fi",
        "  cancel_watchdog", '  if [ -n "$lls_child" ]; then',
              '    kill -TERM "$lls_child" 2>/dev/null || true',
              "    watchdog 0 &", "    lls_watchdog=$!",
              '    wait "$lls_child" 2>/dev/null || true', "    lls_child=", "    cancel_watchdog", "  fi",
              '  if [ -n "$lls_monitor" ]; then',
              '    kill "$lls_monitor" 2>/dev/null || true',
              '    wait "$lls_monitor" 2>/dev/null || true', "  fi",
              '  [ ! -f watchdog-expired ] || lls_result=124',
              '  echo "$lls_result" > program-status']
    if display:
        lines += [
            '  if [ "$lls_saved" = 1 ]; then',
            '    while read -r lls_service lls_state; do',
            '      [ "$lls_state" != running ] || start "$lls_service"',
            '    done < hwc-services-before.txt',
            '    if [ "$lls_sf" = running ]; then',
            "      start surfaceflinger",
            "      resume_framework",
            '      [ "$lls_zygote_secondary" != running ] || setprop ctl.restart zygote_secondary',
            '      [ "$lls_zygote" != running ] || setprop ctl.restart zygote',
            "    fi",
            "    echo 'not requested: SurfaceFlinger was stopped' > restoration-display-check.txt",
            "    lls_restored=0", "    lls_i=0", '    while [ "$lls_i" -lt 60 ]; do',
            "      lls_ok=1",
            '      while read -r lls_service lls_state; do',
            '        if [ "$lls_state" = running ] && [ "$(getprop "init.svc.$lls_service")" != running ]; then lls_ok=0; fi',
            '      done < hwc-services-before.txt',
            '      if [ "$lls_sf" = running ]; then',
            '        [ "$(getprop init.svc.surfaceflinger)" = running ] || lls_ok=0',
            '        [ "$(getprop init.svc.bootanim)" != running ] || lls_ok=0',
            f"        if grep -q '^connected$' {DRM_CLASS}/card*-HDMI-A-*/status 2>/dev/null; then",
            "          echo 'required: HDMI connected' > restoration-display-check.txt",
            "          grep -qw ACTIVE /sys/kernel/debug/dri/0/summary 2>/dev/null || lls_ok=0",
            "        else",
            "          echo 'not checked: no HDMI connector reports connected' > restoration-display-check.txt",
            "        fi",
            "      fi",
            '      if [ "$lls_ok" = 1 ]; then lls_restored=1; break; fi',
            "      sleep 1", "      lls_i=$((lls_i+1))", "    done",
            '    echo "$lls_restored" > restoration-status',
            '    if [ "$lls_restored" != 1 ]; then',
            '      echo "Android restoration was not confirmed within 60 seconds" >&2',
            '      [ "$lls_result" != 0 ] || lls_result=70', "    fi", "  fi",
            "  cat /sys/kernel/debug/dri/0/summary > restored-display.txt 2>&1 || true",
            "  cat /sys/kernel/debug/dri/0/state > restored-drm-state.txt 2>&1 || true",
            '  rm -f "$lls_lock/pid" "$lls_lock/boot_id"', '  rmdir "$lls_lock" 2>/dev/null || true',
        ]
    lines += ['  echo "$lls_result" > status.tmp', "  mv status.tmp status", "}",
              "trap restore EXIT", "trap 'exit 130' INT", "trap 'exit 143' TERM", "trap '' HUP"]
    if apk:
        lines += ['"$4" --watch-back "$$" &', "lls_input=$!", "lls_input_tries=0",
                  "while [ ! -f input-ready ]; do",
                  '  kill -0 "$lls_input" 2>/dev/null || exit 1',
                  '  [ "$lls_input_tries" -lt 50 ] || exit 1',
                  "  sleep 0.02", "  lls_input_tries=$((lls_input_tries+1))", "done"]
    if display:
        lines += [
            "lls_sf=$(getprop init.svc.surfaceflinger)",
            # Firmware uses different HWC names (Rockchip 2-1, this MediaTek 2-3,
            # AIDL on newer releases). Remember each actual service separately.
            'lls_hwc_names=$(getprop | sed -n "s/^\\[init\\.svc\\.\\([^]]*composer[^]]*\\)\\]:.*/\\1/p")',
            'if [ -z "$lls_hwc_names" ]; then lls_hwc_names="vendor.hwcomposer-2-1 vendor.hwcomposer-2-3"; fi',
            ': > hwc-services-before.txt',
            'for lls_service in $lls_hwc_names; do',
            '  case "$lls_service" in *[!A-Za-z0-9_.-]*) continue ;; esac',
            '  lls_state=$(getprop "init.svc.$lls_service")',
            '  [ -z "$lls_state" ] || printf "%s %s\\n" "$lls_service" "$lls_state" >> hwc-services-before.txt',
            'done',
            "lls_zygote=$(getprop init.svc.zygote)",
            "lls_zygote_secondary=$(getprop init.svc.zygote_secondary)",
            'printf "surfaceflinger=%s\\nzygote=%s\\nzygote_secondary=%s\\n" '
            '"$lls_sf" "$lls_zygote" "$lls_zygote_secondary" > services-before.txt',
            'cat hwc-services-before.txt >> services-before.txt',
            "cat /sys/kernel/debug/dri/0/summary > before-display.txt 2>&1 || true",
            "cat /sys/kernel/debug/dri/0/state > before-drm-state.txt 2>&1 || true",
            *(['echo READY >&5 || exit 1', 'exec 5>&-'] if apk else []),
            "lls_saved=1",
            'if [ "$lls_sf" = running ]; then pause_framework || exit 70; fi',
            '[ "$lls_sf" != running ] || stop surfaceflinger',
            'while read -r lls_service lls_state; do',
            '  [ "$lls_state" != running ] || stop "$lls_service"',
            'done < hwc-services-before.txt',
            "sleep 1", "(", "  lls_i=0",
            # Any mode: the receiver keeps Android's. Bounded: 10 s when the session has no limit.
            f'  while [ "$lls_i" -lt {watchdog_seconds * 4 if watchdog_seconds else 40} ]; do',
            "    if grep -q NV12 /sys/kernel/debug/dri/0/summary 2>/dev/null; then",
            "      cat /sys/kernel/debug/dri/0/summary > active-display.txt 2>&1",
            "      cat /sys/kernel/debug/dri/0/state > active-drm-state.txt 2>&1 || true",
            "      exit 0", "    fi", "    sleep 0.25", "    lls_i=$((lls_i+1))", "  done",
            ") &", "lls_monitor=$!",
        ]
    lines += [('"$2" --display --drm-card "$3" --plane-color-space 3 --stop-hint' if apk else
               shlex.join(list(map(str, command)))) + " &", "lls_child=$!"]
    if watchdog_seconds:
        lines += [f"watchdog {watchdog_seconds} &", "lls_watchdog=$!"]
    lines += ['wait "$lls_child"', "lls_result=$?", "lls_child=", 'exit "$lls_result"', ""]
    return "\n".join(lines)


def launch_command(remote, script_name="receiver-session.sh", log_name="run.log"):
    return ("nohup sh " + shlex.quote(str(remote) + "/" + script_name) +
            " </dev/null >" + shlex.quote(str(remote) + "/" + log_name) + " 2>&1 &")


def prune_command(parent, *, prefix="", keep=5):
    """Retain the newest completed runs; never remove live runs or unpulled dumps.

    Only timestamp directories made by our runners qualify. Paths are quoted,
    symlink directories are excluded, and pruning happens after collection.
    """
    if keep < 1 or prefix not in ("", "display-"):
        raise ValueError("Invalid session retention settings")
    script = f'''lls_parent={shlex.quote(str(parent))}
lls_prefix={shlex.quote(prefix)}
lls_keep={keep}
lls_count=0
printf '%s\\n' "$lls_parent"/"$lls_prefix"* | LC_ALL=C sort -r |
while IFS= read -r lls_path; do
  lls_name=${{lls_path##*/}}
  lls_stamp=${{lls_name#"$lls_prefix"}}
  case "$lls_stamp" in *[!0-9TZ.]*) continue ;; esac
  case "$lls_stamp" in ????????T*Z) ;; *) continue ;; esac
  [ -d "$lls_path" ] && [ ! -L "$lls_path" ] && [ -f "$lls_path/status" ] || continue
  [ ! -e "$lls_path/stream.hevc" ] || continue
  lls_count=$((lls_count + 1))
  [ "$lls_count" -le "$lls_keep" ] || rm -rf -- "$lls_path"
done'''
    return ["sh", "-c", script]
