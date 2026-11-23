#!/bin/bash
# SPDX-License-Identifier: MIT
# Two-slot BOOT images for the Galaxy A8 2018 (Phase 2, loader.c slots).
#   make-slots.sh boot KERNEL STOCK_BOOT.img OUT.img "ARGS_A" ["ARGS_B"]
#       slot 0 = KERNEL with ARGS_A (the default), slot 1 = KERNEL with
#       ARGS_B (untested; empty if ARGS_B is not given)
#   make-slots.sh slot KERNEL OUT.slot "ARGS" [NAME]
#       a slot image for the console's "bput" (tools/jackpotlte/a8slot.py)
# The ADT comes from tools/mkadt_jackpotlte.py as in make-boot.sh.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
mode=${1:-}
shift || true
work=$repo/shim/jackpotlte/out
mkdir -p "$work"

check_args() {
	if [ "${#1}" -gt 607 ]; then
		echo "error: boot-args are ${#1} characters, the limit is 607" >&2
		exit 1
	fi
	case " $1 " in
	*" cpus=8 "*) ;;
	*) echo "error: boot-args lack cpus=8 (the kernel would start one core)" >&2; exit 1 ;;
	esac
	case " $1 " in
	*" -noprogress "*) ;;
	*) echo "error: boot-args lack -noprogress (pe_init.c:314 panic)" >&2; exit 1 ;;
	esac
}

payload() {     # KERNEL ARGS OUT
	python3 "$repo/tools/mkpayload.py" "$1" "$work/jackpotlte.adt" "$3" --cmdline "$2"
}

case "$mode" in
boot)
	[ $# -ge 4 ] || { echo "usage: make-slots.sh boot KERNEL STOCK OUT ARGS_A [ARGS_B]" >&2; exit 1; }
	kernel=$(realpath "$1"); stock=$(realpath "$2"); out=$(realpath -m "$3")
	check_args "$4"
	python3 "$repo/tools/mkadt_jackpotlte.py" "$work/jackpotlte.adt"
	payload "$kernel" "$4" "$work/payload-a.bin"
	set -- "$@" ""
	if [ -n "$5" ]; then
		check_args "$5"
		payload "$kernel" "$5" "$work/payload-b.bin"
		python3 "$repo/tools/jackpotlte/mkslots.py" container "$work/slots.bin" \
		    --a "$work/payload-a.bin" --a-name "$(basename "$out" .img)-A" \
		    --b "$work/payload-b.bin" --b-name "$(basename "$out" .img)-B"
	else
		python3 "$repo/tools/jackpotlte/mkslots.py" container "$work/slots.bin" \
		    --a "$work/payload-a.bin" --a-name "$(basename "$out" .img)-A"
	fi
	bash "$repo/shim/jackpotlte/build.sh" "$stock" "$work/slots.bin" "$out"
	;;
slot)
	[ $# -ge 3 ] || { echo "usage: make-slots.sh slot KERNEL OUT.slot ARGS [NAME]" >&2; exit 1; }
	kernel=$(realpath "$1"); out=$(realpath -m "$2")
	check_args "$3"
	python3 "$repo/tools/mkadt_jackpotlte.py" "$work/jackpotlte.adt"
	payload "$kernel" "$3" "$work/payload-slot.bin"
	python3 "$repo/tools/jackpotlte/mkslots.py" blob "$out" "$work/payload-slot.bin" \
	    --name "${4:-$(basename "$out" .slot)}"
	;;
*)
	sed -n '3,9p' "$0" >&2
	exit 1
	;;
esac
