#!/bin/sh
set -u

RELEASE="${RELEASE:-24.10}"
JOBS="${JOBS:-$(nproc)}"

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD="$ROOT/build"
DIST="$ROOT/dist"
CACHE="$BUILD/feeds-cache-$RELEASE"

PACKAGES="roamd luci-app-roamd luci-i18n-roamd-ru"

case "$RELEASE" in
	*.*.*) ;;
	*)
		RELEASE=$(awk -v b="$RELEASE" '$1 == b { print $2 }' "$ROOT/targets/branches.txt" 2>/dev/null)
		[ -n "$RELEASE" ] || { echo "неизвестная ветка, см. targets/branches.txt" >&2; exit 1; }
		;;
esac

BRANCH="${RELEASE%.*}"
TARGET_MAP="$ROOT/targets/$BRANCH.txt"

TARGETS=$(sed 's#^\([^ ]*\) \(.*\)/\(.*\)$#\1:\2:\3#' "$TARGET_MAP" 2>/dev/null)

usage() {
	cat <<EOF
Сборка пакетов roamd для нескольких архитектур OpenWrt.

  $0                 собрать все архитектуры
  $0 <арх> [<арх>…]  собрать только указанные
  $0 --list          показать список архитектур

Переменные окружения:
  RELEASE   версия OpenWrt (сейчас $RELEASE)
  JOBS      параллельность сборки (сейчас $JOBS)

Результат складывается в dist/<ветка>/<архитектура>/ (ветка — из RELEASE).
Список архитектур берётся из targets/<ветка>.txt, версии SDK — из targets/branches.txt.
EOF
}

list_targets() {
	for entry in $TARGETS; do
		arch=${entry%%:*}
		rest=${entry#*:}
		printf '  %-20s %s/%s\n' "$arch" "${rest%%:*}" "${rest#*:}"
	done
}

known_arch() {
	for entry in $TARGETS; do
		[ "${entry%%:*}" = "$1" ] && return 0
	done
	return 1
}

fetch_sdk() {
	target=$1
	subtarget=$2
	dir=$3
	base="https://downloads.openwrt.org/releases/$RELEASE/targets/$target/$subtarget"

	[ -d "$dir" ] && return 0

	name=$(wget -qO- "$base/" | grep -oE 'openwrt-sdk[^"<]*\.tar\.(zst|xz)' | head -1)
	if [ -z "$name" ]; then
		echo "  SDK не найден: $target/$subtarget ($RELEASE)" >&2
		return 1
	fi

	echo "  скачивание $name"
	( cd "$BUILD" && wget -c -q --tries=0 --timeout=30 --waitretry=15 "$base/$name" ) || {
		echo "  скачивание прервано, файл сохранён для докачки: $BUILD/$name" >&2
		return 1
	}

	want=$(wget --spider -S "$base/$name" 2>&1 | sed -n 's/.*[Cc]ontent-[Ll]ength: *\([0-9]*\).*/\1/p' | tail -1)
	have=$(wc -c < "$BUILD/$name" 2>/dev/null)
	if [ -n "$want" ] && [ "$want" != "$have" ]; then
		echo "  SDK скачан не полностью ($have из $want), повторите запуск" >&2
		return 1
	fi

	mkdir -p "$dir"
	case "$name" in
	*.zst) tar --zstd -xf "$BUILD/$name" -C "$dir" --strip-components=1 ;;
	*.xz)  tar -xJf "$BUILD/$name" -C "$dir" --strip-components=1 ;;
	esac
	rm -f "$BUILD/$name"
}

prepare_feeds() {
	dir=$1

	if ! grep -q ' luci ' "$dir/feeds.conf" 2>/dev/null; then
		for feed in base luci; do
			awk -v want="$feed" '/^src-git/ {
				for (i = 2; i <= NF; i++)
					if ($i == want) { print; break }
			}' "$dir/feeds.conf.default"
		done > "$dir/feeds.conf"
	fi

	mkdir -p "$dir/feeds"

	if [ -d "$CACHE/base" ]; then
		for entry in "$CACHE"/*; do
			name=$(basename "$entry")
			[ -e "$dir/feeds/$name" ] || cp -al "$entry" "$dir/feeds/$name"
		done
	fi

	( cd "$dir" && ./scripts/feeds update base luci >/dev/null 2>&1 )
	( cd "$dir" && ./scripts/feeds install -a -p luci >/dev/null 2>&1 )
	( cd "$dir" && ./scripts/feeds install -p base libubox libubus libuci ucode >/dev/null 2>&1 )

	for name in base luci; do
		if [ ! -d "$dir/feeds/$name" ] || [ ! -f "$dir/feeds/$name.index" ]; then
			echo "  feed $name не подготовлен" >&2
			return 1
		fi
	done

	if [ ! -d "$CACHE/base" ]; then
		mkdir -p "$CACHE"
		for name in base base.index base.targetindex luci luci.index luci.targetindex; do
			[ -e "$dir/feeds/$name" ] && cp -al "$dir/feeds/$name" "$CACHE/$name"
		done
	fi
}

build_arch() {
	arch=$1
	target=$2
	subtarget=$3
	dir="$BUILD/sdk-$RELEASE-$target-$subtarget"

	fetch_sdk "$target" "$subtarget" "$dir" || return 1
	prepare_feeds "$dir" || { echo "  не удалось подготовить feeds" >&2; return 1; }

	ln -sfn "$ROOT/package/roamd" "$dir/package/roamd"
	ln -sfn "$ROOT/package/luci-app-roamd" "$dir/package/luci-app-roamd"

	rm -rf "$dir/tmp"
	( cd "$dir" && make defconfig >/dev/null 2>&1 ) || return 1

	for pkg in $PACKAGES; do
		grep -q "^CONFIG_PACKAGE_$pkg=m" "$dir/.config" ||
			echo "CONFIG_PACKAGE_$pkg=m" >> "$dir/.config"
	done

	( cd "$dir" && make defconfig >/dev/null 2>&1 ) || return 1

	real=$(grep -m1 '^CONFIG_TARGET_ARCH_PACKAGES=' "$dir/.config" | cut -d'"' -f2)
	if [ -z "$real" ]; then
		echo "  SDK не сообщил архитектуру пакетов" >&2
		return 1
	fi
	if [ "$real" != "$arch" ]; then
		echo "  внимание: SDK даёт $real, в таблице указано $arch" >&2
	fi

	find "$dir/bin/packages" \( -name 'roamd[-_]*' -o -name 'luci-app-roamd[-_]*' \
		-o -name 'luci-i18n-roamd-ru[-_]*' \) -delete 2>/dev/null

	( cd "$dir" && make package/roamd/compile package/luci-app-roamd/compile -j"$JOBS" >"$dir/build.log" 2>&1 ) || {
		echo "  сборка не удалась, журнал: $dir/build.log" >&2
		return 1
	}

	out="$DIST/${RELEASE%.*}/$real"
	mkdir -p "$out"
	rm -f "$out"/*.ipk "$out"/*.apk

	found=0
	for pattern in 'roamd_*.ipk' 'luci-app-roamd_*.ipk' 'luci-i18n-roamd-*.ipk' \
		       'roamd-[0-9]*.apk' 'luci-app-roamd-[0-9]*.apk' 'luci-i18n-roamd-ru-[0-9]*.apk'; do
		for file in $(find "$dir/bin/packages" -name "$pattern" 2>/dev/null); do
			cp "$file" "$out/"
			found=$((found + 1))
		done
	done

	if [ "$found" -lt 3 ]; then
		echo "  собрано пакетов: $found из 3" >&2
		return 1
	fi

	ls "$out" | grep -E '\.(ipk|apk)$' | while read -r name; do
		echo "  dist/${RELEASE%.*}/$real/$name"
	done
}

case "${1-}" in
--list|-l)
	echo "Доступные архитектуры:"
	list_targets
	exit 0
	;;
--help|-h)
	usage
	exit 0
	;;
esac

for arg in "$@"; do
	if ! known_arch "$arg"; then
		echo "Неизвестная архитектура: $arg" >&2
		echo "Доступные:" >&2
		list_targets >&2
		exit 1
	fi
done

mkdir -p "$BUILD" "$DIST"

ok=""
failed=""

for entry in $TARGETS; do
	arch=${entry%%:*}
	rest=${entry#*:}
	target=${rest%%:*}
	subtarget=${rest#*:}

	if [ $# -gt 0 ]; then
		wanted=0
		for arg in "$@"; do
			[ "$arg" = "$arch" ] && wanted=1
		done
		[ "$wanted" = 1 ] || continue
	fi

	echo "==> $arch  ($target/$subtarget)"

	if build_arch "$arch" "$target" "$subtarget"; then
		ok="$ok $arch"
	else
		failed="$failed $arch"
	fi
done

echo
echo "Собрано:${ok:- —}"
[ -n "$failed" ] && echo "Не собрано:$failed" >&2

[ -z "$failed" ]
