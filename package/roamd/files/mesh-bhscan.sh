#!/bin/sh
OUT="$1"
WANT="$2"
SAMPLE_GAP=3

mkdir -p "${OUT%/*}"

scan() {
	local seen=" " dev phy

	for dev in /sys/class/net/*; do
		[ -e "$dev/phy80211" ] || continue
		phy=$(basename "$(readlink "$dev/phy80211")")
		case "$seen" in *" $phy "*) continue ;; esac
		seen="$seen$phy "
		ubus call iwinfo scan "{\"device\":\"${dev##*/}\"}" 2>/dev/null
	done
}

{
	scan
	sleep "$SAMPLE_GAP"
	scan
} | awk -v want="$WANT" '
	BEGIN { want = toupper(want) }
	/"bssid":/ { gsub(/[",]/, ""); b = toupper($2) }
	/"signal":/ { gsub(/[",]/, ""); if (b != "" && index(want, b)) print b, $2 }' > "$OUT.tmp"

mv "$OUT.tmp" "$OUT"
