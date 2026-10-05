#!/usr/bin/env bash
#
# Installs the templog Home Assistant package and restarts Home Assistant.
#
# What it changes, and nothing else:
#   1. copies templog.yaml into <ha-config>/packages/
#   2. appends a `homeassistant: packages: !include_dir_named packages` block to
#      <ha-config>/configuration.yaml, after backing that file up
#   3. runs Home Assistant's own check_config against the result
#   4. restarts the container, after asking
#
# If check_config fails, the backup is put back and the container is left
# alone, so a YAML mistake cannot take Home Assistant down.
#
# Every step is skipped if it has already been done, so re-running is safe.
# No sudo: configuration.yaml is owned by the invoking user and docker runs
# without it.
#
# Usage:  ./install.sh [--yes] [--dry-run]
#   --yes      restart without asking
#   --dry-run  say what would happen, change nothing
#
# Overridable:  HA_CONFIG=... HA_CONTAINER=... ./install.sh

set -euo pipefail

HA_CONFIG="${HA_CONFIG:-$HOME/certbotazure/homeassistant/config}"
HA_CONTAINER="${HA_CONTAINER:-homeassistant}"
HA_URL="${HA_URL:-http://127.0.0.1:8123/}"
READY_TIMEOUT="${READY_TIMEOUT:-180}"

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_FILE="$SRC_DIR/templog.yaml"
VIEW_FILE="$SRC_DIR/dashboard-view.yaml"

ASSUME_YES=0
DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --yes|-y) ASSUME_YES=1 ;;
        --dry-run|-n) DRY_RUN=1 ;;
        --help|-h)
            # The header comment, up to the first line that is not one.
            awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' \
                "${BASH_SOURCE[0]}"
            exit 0
            ;;
        *) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
    esac
done

CONFIG_YAML="$HA_CONFIG/configuration.yaml"
PACKAGE_DIR="$HA_CONFIG/packages"
PACKAGE_FILE="$PACKAGE_DIR/templog.yaml"

# Set once the backup is made, so a failure only ever restores a backup that
# this run is responsible for.
BACKUP=""

step() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
info() { printf '    %s\n' "$1"; }
warn() { printf '    \033[33m%s\033[0m\n' "$1"; }
die()  { printf '\n\033[31merror: %s\033[0m\n' "$1" >&2; exit 1; }

run() {
    if [ "$DRY_RUN" = 1 ]; then
        info "would run: $*"
    else
        "$@"
    fi
}

restore_backup() {
    if [ -n "$BACKUP" ] && [ -f "$BACKUP" ]; then
        cp "$BACKUP" "$CONFIG_YAML"
        warn "put $(basename "$CONFIG_YAML") back from $(basename "$BACKUP")"
    fi
}

# ---------------------------------------------------------------- preconditions

step "Checking the environment"

command -v docker >/dev/null || die "docker is not on PATH"
[ -f "$SRC_FILE" ] || die "cannot find $SRC_FILE"
[ -d "$HA_CONFIG" ] || die "Home Assistant config directory not found: $HA_CONFIG"
[ -f "$CONFIG_YAML" ] || die "no configuration.yaml in $HA_CONFIG"
[ -w "$CONFIG_YAML" ] || die "$CONFIG_YAML is not writable by $(id -un)"

running="$(docker inspect -f '{{.State.Running}}' "$HA_CONTAINER" 2>/dev/null || echo missing)"
case "$running" in
    true)    info "container '$HA_CONTAINER' is running" ;;
    false)   die "container '$HA_CONTAINER' exists but is not running; start it first" ;;
    *)       die "no container named '$HA_CONTAINER' (set HA_CONTAINER=...)" ;;
esac

# The config directory has to be the one the container actually reads, or
# check_config would validate a different file than the one that was edited.
mounted="$(docker inspect -f \
    '{{range .Mounts}}{{if eq .Destination "/config"}}{{.Source}}{{end}}{{end}}' \
    "$HA_CONTAINER")"
if [ -z "$mounted" ]; then
    die "container '$HA_CONTAINER' has nothing mounted at /config"
elif [ "$mounted" != "$HA_CONFIG" ]; then
    die "container reads /config from '$mounted', not '$HA_CONFIG' (set HA_CONFIG=...)"
fi
info "/config is $mounted"
[ "$DRY_RUN" = 1 ] && warn "dry run: nothing will be changed"

# ------------------------------------------------------------------ the package

step "Installing the package file"

run mkdir -p "$PACKAGE_DIR"
if [ -f "$PACKAGE_FILE" ] && cmp -s "$SRC_FILE" "$PACKAGE_FILE"; then
    info "packages/templog.yaml is already identical to the one in the repo"
else
    run cp "$SRC_FILE" "$PACKAGE_FILE"
    info "copied templog.yaml -> $PACKAGE_FILE"
    warn "this is a copy: re-run this script after editing the file in the repo"
fi

# ------------------------------------------------------- enabling packages:

step "Enabling packages in configuration.yaml"

if grep -Eq '^[[:space:]]+packages:' "$CONFIG_YAML"; then
    info "already enabled; leaving configuration.yaml alone"
elif grep -Eq '^homeassistant:' "$CONFIG_YAML"; then
    # Appending a second top-level homeassistant: key would be a duplicate and
    # would stop Home Assistant loading, so this is not something to guess at.
    die "configuration.yaml already has a 'homeassistant:' block.
       Add this line inside it by hand, then re-run:
           packages: !include_dir_named packages"
else
    BACKUP="$CONFIG_YAML.bak-$(date +%Y%m%d-%H%M%S)"
    if [ "$DRY_RUN" = 1 ]; then
        info "would back up to $(basename "$BACKUP") and append the homeassistant: block"
        BACKUP=""
    else
        cp "$CONFIG_YAML" "$BACKUP"
        info "backed up to $(basename "$BACKUP")"
        cat >> "$CONFIG_YAML" <<'YAML'

# Split-out configuration, one file per package. packages/templog.yaml holds the
# MQTT entities for the templog heater controller; its source of truth is
# ~/templog/homeassistant/templog.yaml, so re-run that directory's install.sh
# after changing it.
homeassistant:
  packages: !include_dir_named packages
YAML
        info "appended the homeassistant: block"
    fi
fi

# --------------------------------------------------------------- check_config

step "Checking the configuration"

# Two checks, because neither is enough on its own.
#
# check_config loads configuration.yaml and the packages, which is what catches
# a YAML mistake or a package naming an integration that does not exist. But it
# reports a bad package with the words "Incorrect config" while still exiting 0,
# so its exit status cannot be the only gate - and it does not look inside
# `mqtt:` at all: an invalid device_class passes it as "Successful config".
#
# So the entities are also put through the same per-platform schemas the MQTT
# integration validates them with when it loads them, which is what actually
# catches a misspelled option.

if [ "$DRY_RUN" = 1 ]; then
    info "would run check_config and validate the entity schemas in the container"
else
    if output="$(docker exec "$HA_CONTAINER" \
            python -m homeassistant --script check_config -c /config 2>&1)"; then
        ck_exit=0
    else
        ck_exit=$?
    fi
    # Strip the colour codes, so the output reads the same in a log or a pipe.
    output="$(printf '%s' "$output" | sed 's/\x1b\[[0-9;]*m//g')"
    printf '%s\n' "$output" | sed 's/^/      /'

    if [ "$ck_exit" != 0 ] ||
            printf '%s' "$output" | grep -qE 'Fatal error|Incorrect config'; then
        restore_backup
        die "check_config rejected the configuration. Home Assistant was NOT
       restarted and is still running its previous configuration."
    fi

    # Errors that have nothing to do with this change are easy to misread as
    # something this script broke, so say so explicitly.
    unrelated="$(printf '%s' "$output" | grep -E '^ERROR' | grep -vEi 'templog|packages' || true)"
    if [ -n "$unrelated" ]; then
        warn "the ERROR lines above are pre-existing and unrelated to templog"
        warn "(check_config cannot resolve device triggers); the configuration"
        warn "itself was accepted."
    fi
    info "configuration loads"

    if schema_out="$(docker exec -i "$HA_CONTAINER" \
            python - /config/packages/templog.yaml <<'PY' 2>&1
import importlib
import sys

import voluptuous as vol
import yaml

config = yaml.safe_load(open(sys.argv[1]))["mqtt"]
bad = 0
for platform, entities in config.items():
    schema = importlib.import_module(
        f"homeassistant.components.mqtt.{platform}"
    ).PLATFORM_SCHEMA_MODERN
    for entity in entities:
        try:
            schema(dict(entity))
        except vol.Invalid as err:
            bad += 1
            print(f"{platform}/{entity.get('name')}: {err}")
print(f"checked {sum(len(v) for v in config.values())} entities")
sys.exit(1 if bad else 0)
PY
    )"; then
        printf '%s\n' "$schema_out" | sed 's/^/      /'
        info "every entity matches the schema its platform will validate it with"
    else
        printf '%s\n' "$schema_out" | sed 's/^/      /'
        restore_backup
        die "an entity in templog.yaml is not valid. Home Assistant was NOT
       restarted and is still running its previous configuration."
    fi
fi

# -------------------------------------------------------------------- restart

step "Restarting Home Assistant"

if [ "$DRY_RUN" = 1 ]; then
    info "would restart container '$HA_CONTAINER'"
elif [ "$ASSUME_YES" = 1 ]; then
    info "restarting (--yes)"
else
    printf '    Restart the container now? Home Assistant will be unavailable\n'
    printf '    for a minute or so. [y/N] '
    reply=""
    read -r reply || true
    case "$reply" in
        y|Y|yes|YES) ;;
        *)
            warn "left running. The entities appear after a restart:"
            warn "  docker restart $HA_CONTAINER"
            exit 0
            ;;
    esac
fi

if [ "$DRY_RUN" != 1 ]; then
    restarted_at="$(date -u +%Y-%m-%dT%H:%M:%S)"
    docker restart "$HA_CONTAINER" >/dev/null
    info "restarted; waiting up to ${READY_TIMEOUT}s for it to answer on $HA_URL"

    ready=0
    for _ in $(seq 1 "$READY_TIMEOUT"); do
        if curl -fsS -o /dev/null --max-time 2 "$HA_URL" 2>/dev/null; then
            ready=1
            break
        fi
        sleep 1
    done
    if [ "$ready" = 1 ]; then
        info "Home Assistant is answering again"
    else
        warn "still not answering after ${READY_TIMEOUT}s - check 'docker logs $HA_CONTAINER'"
    fi

    # ------------------------------------------------------------- verification

    step "Looking for problems"

    problems="$(docker logs --since "$restarted_at" "$HA_CONTAINER" 2>&1 |
        grep -Ei 'templog|packages|invalid config' |
        grep -Ei 'error|warning|invalid' || true)"
    if [ -n "$problems" ]; then
        warn "the log mentions:"
        printf '%s\n' "$problems" | sed 's/^/      /'
    else
        info "nothing about templog or packages in the log"
    fi

    # The entity registry is written with a delay, so a miss here is not
    # necessarily a failure - hence the poll and the soft wording.
    registry="$HA_CONFIG/.storage/core.entity_registry"
    found=""
    for _ in $(seq 1 30); do
        if [ -r "$registry" ]; then
            found="$(python3 - "$registry" <<'PY' || true
import json, sys
try:
    data = json.load(open(sys.argv[1]))
except Exception:
    raise SystemExit(0)
ids = sorted(
    e["entity_id"]
    for e in data.get("data", {}).get("entities", [])
    if str(e.get("unique_id", "")).startswith("templog_")
)
print("\n".join(ids))
PY
)"
        fi
        [ -n "$found" ] && break
        sleep 2
    done
    if [ -n "$found" ]; then
        info "entities created:"
        printf '%s\n' "$found" | sed 's/^/      /'
    else
        warn "no templog_* entities in the registry yet. It is written with a"
        warn "delay, so check Developer Tools -> States for 'templog' before"
        warn "concluding anything is wrong."
    fi
fi

# ----------------------------------------------------------------- next steps

step "Still to do by hand"

cat <<EOF
    1. The dashboard view cannot be scripted: these dashboards are in UI
       storage mode. Open the dashboard, pencil icon, three-dot menu ->
       "Raw configuration editor", and paste the view from:
           $VIEW_FILE
       into its 'views:' list.

    2. The two threshold tiles stay 'unavailable' until firmware that
       publishes temp/1/heater/on_c and /off_c is installed:
           templogctl ota-update --tag latest
       or the firmware card in the Android app. Everything else works on the
       firmware already running.
EOF
