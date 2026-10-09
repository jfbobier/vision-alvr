#!/usr/bin/env bash
# Scoped helper for the Windows builder (the PC that builds and runs everything; see docs/DEVELOPMENT.md).
# Host from $VISIONALVR_BUILDER or tools/builder.local (VISIONALVR_BUILDER=user@host, not committed), key-based auth only
# (BatchMode), working dir %USERPROFILE%\openxr on the builder.
#
#   tools/builder.sh run  'cmd /c command'            run a cmd.exe command on the builder
#   tools/builder.sh ps   script.ps1 [args...]        copy a local PowerShell script to ~/openxr/_tmp and run it
#   tools/builder.sh put  local_path remote_rel_path  copy file/dir to ~/openxr/<remote_rel_path>
#   tools/builder.sh get  remote_rel_path local_path  copy file/dir from ~/openxr/<remote_rel_path>
#
# Commands that look destructive are refused unless --allow-destructive is the first argument.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
if [[ -z "${VISIONALVR_BUILDER:-}" && -f "$HERE/builder.local" ]]; then
  VISIONALVR_BUILDER="$(sed -n 's/^VISIONALVR_BUILDER=//p' "$HERE/builder.local" | head -1)"
fi
HOST="${VISIONALVR_BUILDER:?set VISIONALVR_BUILDER=user@host (or write it to tools/builder.local), see docs/DEVELOPMENT.md}"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30 -o LogLevel=ERROR)
NOISE='post-quantum|store now, decrypt later|openssh.com/pq|server may need to be upgraded|^\*\* *$'

allow_destructive=0
if [[ "${1:-}" == "--allow-destructive" ]]; then allow_destructive=1; shift; fi
mode="${1:-}"; shift || true

guard() {
  local text="$1"
  if [[ $allow_destructive -eq 0 ]] && echo "$text" | grep -qiE '(^|[^a-z])(rmdir|rd |del |erase |format |shutdown|restart-computer|remove-item|winget +(uninstall|remove)|reg +delete|diskpart|bcdedit|net +user|icacls|takeown)'; then
    echo "builder.sh: refused (looks destructive). Re-run with --allow-destructive after the user confirms." >&2
    exit 3
  fi
}

filter() { grep -vE "$NOISE" || true; }

case "$mode" in
  run)
    cmd="${1:?usage: builder.sh run '<cmd>'}"
    guard "$cmd"
    ssh "${SSH_OPTS[@]}" "$HOST" "cd /d %USERPROFILE%\\openxr && $cmd" 2>&1 | filter
    ;;
  ps)
    script="${1:?usage: builder.sh ps script.ps1 [args]}"; shift
    [[ -f "$script" ]] || { echo "no such script: $script" >&2; exit 2; }
    guard "$(cat "$script")"
    ssh "${SSH_OPTS[@]}" "$HOST" "if not exist %USERPROFILE%\\openxr\\_tmp mkdir %USERPROFILE%\\openxr\\_tmp" 2>&1 | filter
    base="$(basename "$script")"
    scp "${SSH_OPTS[@]}" "$script" "$HOST:openxr/_tmp/$base" 2>&1 | filter
    ssh "${SSH_OPTS[@]}" "$HOST" "cd /d %USERPROFILE%\\openxr && powershell -NoProfile -ExecutionPolicy Bypass -File _tmp\\$base $*" 2>&1 | filter
    ;;
  put)
    src="${1:?usage: put local remote_rel}"; dst="${2:?usage: put local remote_rel}"
    [[ "$dst" != /* && "$dst" != *..* && "$dst" != *:* ]] || { echo "remote path must be relative to ~/openxr" >&2; exit 2; }
    scp -r "${SSH_OPTS[@]}" "$src" "$HOST:openxr/$dst" 2>&1 | filter
    ;;
  get)
    src="${1:?usage: get remote_rel local}"; dst="${2:?usage: get remote_rel local}"
    [[ "$src" != /* && "$src" != *..* && "$src" != *:* ]] || { echo "remote path must be relative to ~/openxr" >&2; exit 2; }
    scp -r "${SSH_OPTS[@]}" "$HOST:openxr/$src" "$dst" 2>&1 | filter
    ;;
  *)
    sed -n '2,12p' "$0"; exit 2 ;;
esac
