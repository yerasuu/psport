#!/usr/bin/env bash
# tools/regress/plan.sh [options] PLAN STATE_DIR: runs a night's steps one after the other, so that a
# cut (power, reboot, a crashed session) loses at most the step under way. PLAN is a bash file
# sourced here; besides its own variables and functions it declares the steps, in order:
#   step ID DESCRIPTION -- COMMAND [ARG...]
# ID is [A-Za-z0-9._-] and unique. Words are expanded when the plan is sourced, so whatever needs
# the step's folder goes in a plan function. Each command runs in its own process group, in its
# attempt folder STATE_DIR/ID/ (ID.attemptN/ for attempt N, so that a retry never mixes with a
# half-written run) with STEP_ID, STEP_DIR, STEP_ATTEMPT and STATE_DIR exported and its output in
# STEP_DIR/step.log. `step_dir ID` prints the folder of an earlier step's finished run; a line
# written to STEP_DIR/note shows in status.txt.
# STATE_DIR keeps ID.running while a step runs and ID.done once it ended (state, exit, attempt, start
# and end times, folder), plan.log, status.txt (each step's state, exit, attempts, duration and note)
# and lock (one instance at a time). On a start, done steps are skipped; a step left .running was cut
# and runs again in a new folder, up to -a attempts (what the cut runner left running is stopped
# first). A failing step is logged and the plan goes on.
# Every step runs with online play off (decision #601): PS_ONLINE, PS_BBSC, PS_BBSC_LOCAL_SERVER and
# PS_UPNP are exported as 0 and the server variables unset after the plan is sourced; a step whose
# command, plan functions or titles file (a .txt word) turn one on is not run (state refused), and
# one whose logs show the online module anyway ends as state online.
#   -w HH:MM-HH:MM  start steps only inside this local time window (it may cross midnight); outside
#                   it, exit 0 without starting one (a reboot by day must not start a game)
#   -a N            attempts of a cut step (default 2)
#   -n              list what would run and change nothing
set -uo pipefail
window='' attempts=2 is_dry=0
while getopts 'w:a:n' opt; do
    case $opt in
        w) window=$OPTARG ;; a) attempts=$OPTARG ;; n) is_dry=1 ;; *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(($# == 2)) || { sed -n '2,25p' "$0" | cut -c3-; exit 2; }
[[ $attempts =~ ^[1-9][0-9]*$ ]] || { echo "plan.sh: -a needs a positive integer" >&2; exit 2; }
if [[ -n $window ]]; then
    [[ $window =~ ^([01][0-9]|2[0-3]):([0-5][0-9])-([01][0-9]|2[0-3]):([0-5][0-9])$ ]] ||
        { echo "plan.sh: -w is HH:MM-HH:MM" >&2; exit 2; }
    window_start=$((10#${BASH_REMATCH[1]} * 60 + 10#${BASH_REMATCH[2]}))
    window_end=$((10#${BASH_REMATCH[3]} * 60 + 10#${BASH_REMATCH[4]}))
fi
plan=$(realpath -- "$1") || exit 2
[[ -f $plan ]] || { echo "plan.sh: no plan $1" >&2; exit 2; }
state=$(realpath -m -- "$2")

ids=() descriptions=() commands=()
step() {
    local id=$1 description=${2:-} known command
    [[ $id =~ ^[A-Za-z0-9._-]+$ && ${3:-} == -- && $# -ge 4 ]] ||
        { echo "plan.sh: step ID DESCRIPTION -- COMMAND...: bad step '$id'" >&2; exit 2; }
    for known in "${ids[@]}"; do
        [[ $known != "$id" ]] || { echo "plan.sh: step $id declared twice" >&2; exit 2; }
    done
    shift 3
    printf -v command '%q ' "$@"
    ids+=("$id") descriptions+=("$description") commands+=("$command")
}
# step_dir ID: the folder of ID's finished run, for a later step (a baseline to compare with, a seed).
step_dir() {
    local dir
    dir=$(field "$STATE_DIR/$1.done" dir) && [[ -n $dir && -d $STATE_DIR/$dir ]] || return 1
    printf '%s' "$STATE_DIR/$dir"
}
field() { sed -n "s/^$2=//p" -- "$1" 2> /dev/null | tail -1; [[ -f $1 ]]; }

export STATE_DIR=$state
# shellcheck source=/dev/null
source -- "$plan" || { echo "plan.sh: $plan failed" >&2; exit 2; }
((${#ids[@]})) || { echo "plan.sh: $plan declares no step" >&2; exit 2; }

online_switches=(PS_ONLINE PS_BBSC PS_BBSC_LOCAL_SERVER PS_UPNP)
online_servers=(PS_SHADNET_SERVER PS_SHADNET_WEBAPI PS_SHADNET_NPID PS_SHADNET_PASSWORD PS_SHADNET_TOKEN
    PS_SHADNET_CLIENT_VERSION PS_COMMUNITY_SERVER PS_NET_MODULE SHADPS4_HTTP_HOST_OVERRIDES_JSON)
for name in "${online_switches[@]}"; do export "$name=0"; unset "BB_${name#PS_}"; done
for name in "${online_servers[@]}"; do unset "$name" "BB_${name#PS_}"; done
online_on="(PS|BB)_(ONLINE|BBSC|BBSC_LOCAL_SERVER|UPNP)=[\"']?[1-9A-Za-z]|(PS|BB)_(SHADNET_[A-Z_]+|COMMUNITY_SERVER|NET_MODULE)=|SHADPS4_HTTP_HOST_OVERRIDES_JSON="
# online_guard COMMAND: fails, saying why, when the environment or the step would turn online play
# on: its command line, the plan functions it names and the titles files (.txt words) it passes.
online_guard() {
    local text=$1 name word words
    for name in "${online_switches[@]}"; do
        [[ ${!name:-0} == 0 ]] || { echo "$name=${!name} in the environment"; return 1; }
    done
    for name in "${online_servers[@]}"; do
        [[ -z ${!name+set} ]] || { echo "$name set in the environment"; return 1; }
    done
    read -ra words <<< "$text"
    for word in "${words[@]}"; do
        if [[ $word =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] && declare -F -- "$word" > /dev/null; then text+=$'\n'$(declare -f -- "$word")
        elif [[ $word == *.txt && -f $word ]]; then text+=$'\n'$(< "$word"); fi
    done
    [[ ! $text =~ $online_on ]] || { echo "the step sets ${BASH_REMATCH[0]}"; return 1; }
}

in_window() {
    [[ -n $window ]] || return 0
    local now
    now=$((10#$(printf '%(%H)T' -1) * 60 + 10#$(printf '%(%M)T' -1)))
    if ((window_start < window_end)); then ((now >= window_start && now < window_end))
    elif ((window_start > window_end)); then ((now >= window_start || now < window_end))
    fi
}

# start_time PID: the process's start (clock ticks since boot), so that a pid the system gave to
# another process since is not taken for the step's.
start_time() {
    local stat
    { read -r stat < "/proc/$1/stat"; } 2> /dev/null || return 1
    stat=${stat##*) }
    read -ra stat <<< "$stat"
    printf '%s' "${stat[19]}"
}

# stop_left_over ID: what a killed runner left of the step (its process group, whose regress.sh then
# stops its game sessions) is stopped before the step runs again.
stop_left_over() {
    local file=$state/$1.running pid ticks i
    pid=$(field "$file" pid) ticks=$(field "$file" ticks)
    [[ -n $pid && -n $ticks && $(start_time "$pid") == "$ticks" ]] || return 0
    log "$1: process group $pid of the cut attempt is still running: stopping it"
    kill -TERM -- -"$pid" 2> /dev/null
    for ((i = 0; i < 60; i++)); do kill -0 -- -"$pid" 2> /dev/null || return 0; sleep 1; done
    kill -KILL -- -"$pid" 2> /dev/null
}

log() { printf '[%(%F %T)T] plan: %s\n' -1 "$*" | tee -a "$state/plan.log" >&2; }

# write FILE LINES...: replaced by a rename, so that a cut never leaves it half-written.
write() {
    local file=$1; shift
    printf '%s\n' "$@" > "$file.new" && mv -f -- "$file.new" "$file"
}

duration() { printf '%d:%02d:%02d' $(($1 / 3600)) $(($1 / 60 % 60)) $(($1 % 60)); }

write_status() {
    local i id file outcome exit_code tries took note dir now
    now=$(printf '%(%s)T' -1)
    {
        printf '%-24s %-8s %-5s %-8s %-9s %s\n' step state exit attempts duration note
        for i in "${!ids[@]}"; do
            id=${ids[i]} outcome=pending exit_code=- tries=0 took=- note=''
            if [[ -f $state/$id.done ]]; then
                file=$state/$id.done
                outcome=$(field "$file" state) exit_code=$(field "$file" exit) tries=$(field "$file" attempt)
                took=$(duration $(($(field "$file" end) - $(field "$file" start))))
                dir=$(field "$file" dir)
            elif [[ -f $state/$id.running ]]; then
                file=$state/$id.running
                outcome=cut tries=$(field "$file" attempt) dir=$(field "$file" dir)
                [[ $id == "${current:-}" ]] && outcome=running took=$(duration $((now - $(field "$file" start))))
            fi
            [[ -n ${dir:-} && -f $state/$dir/note ]] && note=$(head -1 -- "$state/$dir/note")
            printf '%-24s %-8s %-5s %-8s %-9s %s\n' "$id" "$outcome" "$exit_code" "$tries" "$took" "$note"
            dir=''
        done
    } > "$state/status.txt.new" && mv -f -- "$state/status.txt.new" "$state/status.txt"
}

# finish ID STATE EXIT ATTEMPT START DIR
finish() {
    write "$state/$1.done" "state=$2" "exit=$3" "attempt=$4" "start=$5" "end=$(printf '%(%s)T' -1)" "dir=$6"
    rm -f -- "$state/$1.running"
    current=''
    write_status
}

if ((is_dry)); then
    if [[ -z $window ]]; then echo "no time window"
    elif in_window; then echo "inside the window $window: steps would start"
    else echo "outside the window $window: nothing would start"; fi
    for i in "${!ids[@]}"; do
        id=${ids[i]}
        if [[ -f $state/$id.done ]]; then
            what="done ($(field "$state/$id.done" state), exit $(field "$state/$id.done" exit))"
        elif [[ -f $state/$id.running ]]; then
            tried=$(field "$state/$id.running" attempt)
            what="cut at attempt $tried: would run attempt $((tried + 1))"
            ((tried >= attempts)) && what="cut at attempt $tried: would give up (-a $attempts)"
        elif reason=$(online_guard "${commands[i]}"); then what="would run"
        else what="would be refused: $reason"; fi
        printf '%-24s %s\n    %s\n    %s\n' "$id" "$what" "${descriptions[i]}" "${commands[i]}"
    done
    exit 0
fi

mkdir -p -- "$state" || exit 1
exec {lock_fd}> "$state/lock"
flock -n "$lock_fd" || { echo "plan.sh: another plan.sh holds $state/lock" >&2; exit 1; }
log "start: $plan, ${#ids[@]} steps${window:+, window $window}, $attempts attempts"
current='' pid=''
trap 'log "stopped during ${current:-no step}: it runs again next time"; [[ -n $pid ]] && kill -TERM -- -"$pid" 2> /dev/null; wait; exit 143' TERM INT HUP
write_status
counts=() summary=""
for i in "${!ids[@]}"; do
    id=${ids[i]} attempt=1
    [[ -f $state/$id.done ]] && continue
    if [[ -f $state/$id.running ]]; then
        tried=$(field "$state/$id.running" attempt)
        stop_left_over "$id"
        log "$id: attempt $tried was cut"
        if ((tried >= attempts)); then
            log "$id: gives up after $tried attempts"
            finish "$id" cut - "$tried" "$(field "$state/$id.running" start)" "$(field "$state/$id.running" dir)"
            continue
        fi
        attempt=$((tried + 1))
    fi
    if ! in_window; then
        log "outside the window $window: stopping before $id"
        break
    fi
    dir=$id
    ((attempt > 1)) && dir=$id.attempt$attempt
    started=$(printf '%(%s)T' -1)
    if ! reason=$(online_guard "${commands[i]}"); then
        log "$id: refused, online play: $reason"
        finish "$id" refused - "$attempt" "$started" ''
        continue
    fi
    write "$state/$id.running" "attempt=$attempt" "start=$started" "dir=$dir"
    current=$id
    rm -rf -- "${state:?}/$dir"
    mkdir -p -- "$state/$dir"
    log "$id: attempt $attempt in $dir: ${descriptions[i]}"
    write_status
    # Its own process group (job control on only for the spawn), so that a stop reaches regress.sh
    # and whatever else the step started, and the lock's descriptor closed in it, so that a left-over
    # step never keeps the next runner out.
    set -m
    (
        exec {lock_fd}>&-
        cd -- "$state/$dir" || exit 1
        export STEP_ID=$id STEP_DIR=$state/$dir STEP_ATTEMPT=$attempt
        eval "${commands[i]}"
    ) > "$state/$dir/step.log" 2>&1 < /dev/null &
    pid=$!
    set +m
    write "$state/$id.running" "attempt=$attempt" "start=$started" "dir=$dir" "pid=$pid" "ticks=$(start_time "$pid")"
    wait "$pid"
    code=$?
    pid=''
    outcome=ok
    ((code == 0)) || outcome=failed
    if grep -rqsE --include=log --include='*.log' "^Online: (module |the title's extension .* is on)" -- "$state/$dir"; then
        outcome=online
        log "$id: WARNING: its logs show online play"
    fi
    log "$id: $outcome, exit $code after $(duration $(($(printf '%(%s)T' -1) - started)))"
    finish "$id" "$outcome" "$code" "$attempt" "$started" "$dir"
done
for file in "$state"/*.done; do
    [[ -f $file ]] && counts+=("$(field "$file" state)")
done
((${#counts[@]})) && summary=$(printf '%s\n' "${counts[@]}" | sort | uniq -c | awk '{printf "%s%s %s", sep, $1, $2; sep=", "}')
log "end: ${summary:-no step finished}; $state/status.txt"
