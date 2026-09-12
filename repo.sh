#!/bin/sh
set -u

ROOT=$(cd "$(dirname "$0")" && pwd)
DIST="$ROOT/dist"
PORT="${PORT:-8079}"
PIDFILE="$ROOT/build/repo.pid"
CERT="$ROOT/build/repo.crt"
KEY="$ROOT/build/repo.key"

usage() {
	cat <<EOF
Локальный opkg-репозиторий с собранными пакетами roamd (на время разработки).

  $0                                построить индексы и запустить раздачу
  $0 index                          только построить индексы Packages
  $0 stop                           остановить раздачу
  $0 index-feed <ветка> <арх> <ключи>  индекс одной пары с подписью (для CI)
  $0 publish <каталог> <версия>     выложить релизы на GitHub (для CI)

Переменные окружения:
  PORT   порт раздачи (сейчас $PORT)

Структура: dist/<ветка>/<архитектура>/{roamd_*.ipk,Packages}
Раздача идёт по HTTPS с самоподписанным сертификатом (build/repo.crt) — контроллер
закрепляет его в /etc/roamd/pkg.crt, как и сертификаты узлов. Обычный http контроллер
отвергает: доставка пакетов даёт root на всех узлах mesh.
EOF
}

control_of() {
	tar -xzOf "$1" ./control.tar.gz 2>/dev/null | tar -xzO ./control 2>/dev/null
}

index_dir() {
	dir="$1"
	index="$dir/Packages"
	: > "$index"

	for file in "$dir"/*.ipk "$dir"/*.apk; do
		[ -f "$file" ] || continue

		base=$(basename "$file")

		case "$file" in
		*.ipk)
			control_of "$file" | sed '/^$/d' >> "$index"
			;;
		*.apk)
			stem=${base%.apk}
			printf 'Package: %s\nVersion: %s\nArchitecture: %s\n' \
				"$(echo "$stem" | sed -n 's/^\(.*\)-\([0-9].*\)$/\1/p')" \
				"$(echo "$stem" | sed -n 's/^\(.*\)-\([0-9].*\)$/\2/p')" \
				"$(basename "$dir")" >> "$index"
			;;
		esac

		printf 'Filename: %s\nSize: %s\nSHA256sum: %s\n\n' \
			"$base" \
			"$(wc -c < "$file" | tr -d ' ')" \
			"$(sha256sum "$file" | cut -d' ' -f1)" >> "$index"
	done

	gzip -kf "$index"
	echo "  $(basename "$(dirname "$dir")")/$(basename "$dir"): $(grep -c '^Package:' "$index") пакетов"
}

build_index() {
	[ -d "$DIST" ] || { echo "dist/ пуст, сначала ./build.sh" >&2; exit 1; }

	for branch in "$DIST"/*; do
		[ -d "$branch" ] || continue
		for arch in "$branch"/*; do
			[ -d "$arch" ] && index_dir "$arch"
		done
	done
}

sdk_dir() {
	release=$(awk -v b="$1" '$1 == b { print $2 }' "$ROOT/targets/branches.txt")
	target=$(awk -v a="$2" '$1 == a { print $2 }' "$ROOT/targets/$1.txt")

	[ -n "$release" ] || return 1

	dir="$ROOT/build/sdk-$release-$(echo "$target" | tr / -)"
	[ -d "$dir" ] || dir=$(ls -d "$ROOT/build/sdk-$release-"* 2>/dev/null | head -1)

	[ -n "$dir" ] && echo "$dir"
}

index_feed() {
	branch="$1"
	arch="$2"
	keys="$3"
	dir="$DIST/$branch/$arch"
	sdk=$(sdk_dir "$branch" "$arch") || { echo "нет карты для $branch/$arch" >&2; return 1; }

	[ -d "$dir" ] || { echo "нет пакетов в $dir" >&2; return 1; }

	if ls "$dir"/*.apk >/dev/null 2>&1; then
		apk="$sdk/staging_dir/host/bin/apk"
		[ -x "$apk" ] || { echo "в SDK нет apk" >&2; return 1; }

		set -- --allow-untrusted
		[ -f "$keys/roamd.pem" ] && set -- "$@" --sign-key "$keys/roamd.pem"

		( cd "$dir" && "$apk" mkndx "$@" -o packages.adb ./*.apk ) || return 1
		echo "  $branch/$arch: packages.adb$([ -f "$keys/roamd.pem" ] && echo ' (подписан)')"
		return 0
	fi

	index_dir "$dir"

	if [ -f "$keys/roamd.sec" ]; then
		usign="$sdk/staging_dir/host/bin/usign"
		[ -x "$usign" ] || { echo "в SDK нет usign" >&2; return 1; }

		"$usign" -S -m "$dir/Packages" -s "$keys/roamd.sec" -x "$dir/Packages.sig" || return 1
		gzip -kf "$dir/Packages"
		echo "  $branch/$arch: Packages подписан"
	fi
}

release_assets() {
	id=$(gh api "repos/{owner}/{repo}/releases/tags/$1" --jq .id) || return 1
	gh api --paginate "repos/{owner}/{repo}/releases/$id/assets?per_page=100" --jq '.[] | "\(.id) \(.name)"'
}

release_prune() {
	tag="$1"
	keep="$2"
	shift 2

	release_assets "$tag" | while read -r id name; do
		[ -e "$keep/$name" ] && continue

		owned=
		for prefix in "$@"; do
			case "$name" in
			"$prefix"roamd*|"$prefix"luci-*|"$prefix"Packages*|"$prefix"packages.adb) owned=1; break ;;
			esac
		done
		[ -n "$owned" ] || continue

		gh api -X DELETE "repos/{owner}/{repo}/releases/assets/$id" </dev/null >/dev/null || exit 1
		echo "  $tag: удалён $name"
	done
}

publish_feeds() {
	src="$1"
	version="$2"
	all="$src/all"
	prefixes=

	command -v gh >/dev/null 2>&1 || { echo "нужен gh" >&2; return 1; }

	mkdir -p "$all"

	for dir in "$src"/feed-*; do
		[ -d "$dir" ] || continue

		tag=$(basename "$dir")

		gh release view "$tag" >/dev/null 2>&1 ||
			gh release create "$tag" --title "$tag" --notes "Репозиторий пакетов roamd: $tag" --prerelease

		gh release upload "$tag" "$dir"/* --clobber || return 1
		release_prune "$tag" "$dir" "" || return 1
		echo "  $tag"

		prefixes="$prefixes ${tag#feed-}-"
		for pkg in "$dir"/*.ipk "$dir"/*.apk; do
			[ -f "$pkg" ] || continue
			cp "$pkg" "$all/${tag#feed-}-$(basename "$pkg")"
		done
	done

	[ -n "$version" ] || return 0

	tag="v$version"
	if gh release view "$tag" >/dev/null 2>&1; then
		release_prune "$tag" "$all" $prefixes || return 1
	else
		gh release create "$tag" --title "roamd $version" --notes "Пакеты roamd $version для веток 23.05, 24.10 и 25.12."
	fi

	gh release upload "$tag" "$all"/* --clobber || return 1
	echo "  $tag ($(ls "$all" | wc -l) файлов)"
}

stop_serving() {
	[ -f "$PIDFILE" ] || { echo "раздача не запущена"; return 0; }
	kill "$(cat "$PIDFILE")" 2>/dev/null
	rm -f "$PIDFILE"
	echo "раздача остановлена"
}

case "${1-}" in
--help|-h) usage; exit 0 ;;
index) build_index; exit 0 ;;
index-feed) shift; index_feed "$1" "$2" "${3:-/tmp/keys}"; exit $? ;;
publish) shift; publish_feeds "$1" "${2:-}"; exit $? ;;
stop) stop_serving; exit 0 ;;
esac

build_index
stop_serving >/dev/null

mkdir -p "$(dirname "$PIDFILE")"
addr=$(ip -4 -o addr show scope global | awk '{ print $4 }' | cut -d/ -f1 | head -1)

if [ ! -f "$CERT" ] || ! openssl x509 -checkend 86400 -noout -in "$CERT" >/dev/null 2>&1; then
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
		-keyout "$KEY" -out "$CERT" -days 365 -subj "/CN=$addr" \
		-addext "subjectAltName=IP:$addr" >/dev/null 2>&1 || {
		echo "не удалось выпустить сертификат раздачи" >&2
		exit 1
	}
	echo "  выпущен сертификат для $addr: build/repo.crt"
fi

python3 -c "
import http.server, ssl, sys
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain('$CERT', '$KEY')
srv = http.server.ThreadingHTTPServer(('', $PORT), lambda *a: http.server.SimpleHTTPRequestHandler(*a, directory='$DIST'))
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()
" >/dev/null 2>&1 &
echo $! > "$PIDFILE"

echo "раздача на https://$addr:$PORT (pid $(cat "$PIDFILE"))"
echo "на контроллере:"
echo "  scp build/repo.crt root@<контроллер>:/etc/roamd/pkg.crt"
echo "  uci set roamd.mesh.pkg_url='https://$addr:$PORT'; uci commit roamd"
