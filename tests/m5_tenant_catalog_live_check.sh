#!/usr/bin/env bash
# tests/m5_tenant_catalog_live_check.sh — M5.2a's ordinary-tenant live arm.
#
# This is the named entry point for the tenant profile in
# tests/m5_catalog_live_check.sh. It intentionally forces M52_PROFILE=tenant,
# builds WITHOUT e7_gate, creates environment index 3 as bob, and asserts the
# same four CAT_EXEC outcomes as the gate arm plus the absence of [E7-GATE].
# It does not register a checkpoint task or claim arbitrary tenant Linux
# execution; the probe is the existing short-lived M5.2 fixture.
set -u
cd "$(dirname "$0")/.." || exit 2
export M52_PROFILE=tenant
exec bash tests/m5_catalog_live_check.sh "$@"
