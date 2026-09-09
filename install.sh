#!/bin/sh
# shellcheck shell=dash

REPO="Ground-Zerro/roamd"
RAW="https://raw.githubusercontent.com/$REPO/main"
FEED_BASE="https://github.com/$REPO/releases/download"
FEED_NAME="roamd"
PACKAGES="roamd luci-app-roamd"
PACKAGES_RU="luci-i18n-roamd-ru"

PKG_IS_APK=0
command -v apk >/dev/null 2>&1 && PKG_IS_APK=1

msg() {
	printf '\033[32;1m%s\033[0m\n' "$1"
}

err() {
	printf '\033[31;1m%s\033[0m\n' "$1" >&2
}

fetch() {
	wget -q -O "$2" "$1" || return 1
	[ -s "$2" ]
}

check_system() {
	[ -f /etc/openwrt_release ] || { err "Это не OpenWrt"; exit 1; }

	. /etc/openwrt_release

	BRANCH="${DISTRIB_RELEASE%.*}"
	ARCH="$DISTRIB_ARCH"

	case "$BRANCH" in
		23.05|24.10|25.12) ;;
		*)
			err "OpenWrt $DISTRIB_RELEASE не поддерживается (нужна ветка 23.05, 24.10 или 25.12)"
			exit 1
			;;
	esac

	[ -n "$ARCH" ] || { err "Не удалось определить архитектуру устройства"; exit 1; }

	msg "Устройство: $(cat /tmp/sysinfo/model 2>/dev/null || echo '?')"
	msg "OpenWrt $DISTRIB_RELEASE, архитектура $ARCH"

	space=$(df /overlay 2>/dev/null | awk 'NR==2 { print $4 }')
	[ -n "$space" ] && [ "$space" -lt 2048 ] && {
		err "Мало места во флеш-памяти: $((space / 1024)) МБ, нужно не меньше 2 МБ"
		exit 1
	}

	nslookup downloads.openwrt.org >/dev/null 2>&1 || {
		err "DNS не работает — установка невозможна"
		exit 1
	}
}

feed_url() {
	echo "$FEED_BASE/feed-$BRANCH-$ARCH"
}

install_key_opkg() {
	tmp=/tmp/roamd-feed.pub

	fetch "$RAW/keys/roamd-usign.pub" "$tmp" || {
		err "Не удалось скачать ключ подписи репозитория"
		exit 1
	}

	if command -v usign >/dev/null 2>&1; then
		fp=$(usign -F -p "$tmp" 2>/dev/null)
	else
		fetch "$RAW/keys/roamd-usign.fp" /tmp/roamd-feed.fp && fp=$(cat /tmp/roamd-feed.fp)
		rm -f /tmp/roamd-feed.fp
	fi

	[ -n "$fp" ] || { err "Не удалось определить отпечаток ключа"; rm -f "$tmp"; exit 1; }

	mkdir -p /etc/opkg/keys
	mv "$tmp" "/etc/opkg/keys/$fp"
	msg "Ключ подписи установлен ($fp)"
}

install_key_apk() {
	mkdir -p /etc/apk/keys

	fetch "$RAW/keys/roamd-apk.rsa.pub" /etc/apk/keys/roamd-apk.rsa.pub || {
		err "Не удалось скачать ключ подписи репозитория"
		exit 1
	}

	msg "Ключ подписи установлен"
}

add_feed_opkg() {
	conf=/etc/opkg/customfeeds.conf
	line="src/gz $FEED_NAME $(feed_url)"

	touch "$conf"

	if grep -q "^src/gz[[:space:]]\+$FEED_NAME[[:space:]]" "$conf"; then
		if grep -qxF "$line" "$conf"; then
			msg "Репозиторий уже подключён"
			return 0
		fi

		sed -i "\#^src/gz[[:space:]]\+$FEED_NAME[[:space:]]#d" "$conf"
		msg "Адрес репозитория обновлён"
	fi

	echo "$line" >> "$conf"
	msg "Репозиторий подключён: $(feed_url)"
}

add_feed_apk() {
	conf=/etc/apk/repositories.d/roamd.list
	line="$(feed_url)/packages.adb"

	mkdir -p /etc/apk/repositories.d

	if [ -f "$conf" ] && grep -qxF "$line" "$conf"; then
		msg "Репозиторий уже подключён"
		return 0
	fi

	echo "$line" > "$conf"
	msg "Репозиторий подключён: $line"
}

pkg_update() {
	if [ "$PKG_IS_APK" -eq 1 ]; then
		apk update
	else
		opkg update
	fi
}

pkg_install() {
	if [ "$PKG_IS_APK" -eq 1 ]; then
		apk add "$@"
	else
		opkg install "$@"
	fi
}

pkg_installed() {
	if [ "$PKG_IS_APK" -eq 1 ]; then
		apk list --installed 2>/dev/null | grep -q "^$1-[0-9]"
	else
		opkg list-installed 2>/dev/null | grep -q "^$1 "
	fi
}

ask_ru() {
	pkg_installed "$PACKAGES_RU" && return 0
	[ -c /dev/tty ] || return 0

	printf '\033[32;1m%s\033[0m' "Установить русский язык интерфейса? [Y/n] "
	read -r answer < /dev/tty

	case "$answer" in
		n|N|no|No) return 1 ;;
		*) return 0 ;;
	esac
}

main() {
	check_system

	if [ "${1-}" = "--check" ]; then
		msg "Ветка: $BRANCH, архитектура: $ARCH"
		msg "Репозиторий: $(feed_url)"
		msg "Пакетный менеджер: $([ "$PKG_IS_APK" -eq 1 ] && echo apk || echo opkg)"
		exit 0
	fi

	if [ "$PKG_IS_APK" -eq 1 ]; then
		install_key_apk
		add_feed_apk
	else
		install_key_opkg
		add_feed_opkg
	fi

	pkg_update || { err "Не удалось обновить список пакетов"; exit 1; }

	if pkg_installed roamd; then
		msg "roamd уже установлен — обновляем"
	else
		msg "Устанавливаем roamd"
	fi

	# shellcheck disable=SC2086
	pkg_install $PACKAGES || { err "Установка не удалась"; exit 1; }

	if ask_ru; then
		pkg_install "$PACKAGES_RU" || err "Русский язык поставить не удалось"
	fi

	/etc/init.d/roamd enable >/dev/null 2>&1
	/etc/init.d/roamd restart >/dev/null 2>&1

	msg "Готово. Откройте LuCI → Сеть → Роуминг Wi-Fi"
	msg "Обновления: повторный запуск этого скрипта или $([ "$PKG_IS_APK" -eq 1 ] && echo 'apk upgrade roamd' || echo 'opkg upgrade roamd')"
}

main "$@"
