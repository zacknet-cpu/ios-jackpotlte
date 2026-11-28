#!/bin/bash
# SPDX-License-Identifier: MIT
#
# On branch a8-p13-air only: mesa.yml calls this script, and the CI login
# cannot add a workflow of its own (no `workflow` scope), so here it builds
# the P13 AIR targets as well as libOSMesa (tools/mesa-air/mk-mesa-air.sh).
# The production build is this file on a8-mesa-o3.
exec "$(dirname "$0")/../mesa-air/mk-mesa-air.sh" --with-osmesa "$@"
