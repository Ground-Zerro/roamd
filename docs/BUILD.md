# Сборка и выпуск

> Документация для версии **roamd 1.1.0-r1** (веб-интерфейс **luci-app-roamd 1.1.0-r1**).
>
> Проект передан [freenetic](https://github.com/unisequence/freenetic) — дальнейшее
> развитие идёт в его рамках.

Пакеты собираются штатным OpenWrt SDK. Сборку под матрицу веток и архитектур выполняет
`build.sh`, индексы и публикацию — `repo.sh`, выпуск — GitHub Actions.

## Пакеты

| Пакет | Содержимое | Зависимости |
|---|---|---|
| `roamd` | демон, init-скрипт, конфигурация, ACL канала управления | `libubox`, `libubus`, `libuci`, `libblobmsg-json`, `libuclient` |
| `luci-app-roamd` | страницы «Роуминг Wi-Fi» и «MESH», ACL LuCI, меню | `luci-base`, `roamd` |
| `luci-i18n-roamd-ru` | русский перевод | `luci-app-roamd` |

Отдельные `libnl`, `libiwinfo` и zlib не нужны: сигнал клиентов берётся у hostapd, радио-данные
— из ubus-объекта `iwinfo`, индексы apk разбирает собственный распаковщик DEFLATE.

Во время работы демон пользуется тем, что уже есть в OpenWrt или ставит сам: hostapd с ubus
(полный `wpad` ставится автоматически), `dropbear` (SSH-клиент и `dropbearkey`), `px5g` на узле
(обычно уже есть в образе с `luci-ssl`, иначе ставится при захвате вариант под TLS-библиотеку
узла). HTTPS — на `libuclient`, она есть почти в любом образе, её тянет `uclient-fetch`.

## Зависимости хоста

На Debian/Ubuntu хватает стандартного набора для сборки OpenWrt:

```sh
apt-get install build-essential gawk gettext git libncurses-dev libssl-dev \
                python3 rsync unzip wget xsltproc zlib1g-dev zstd
```

## build.sh

```sh
./build.sh                                   # все архитектуры ветки по умолчанию (24.10)
RELEASE=25.12 ./build.sh aarch64_cortex-a53  # одна архитектура другой ветки
RELEASE=23.05.6 ./build.sh mipsel_24kc       # точная версия SDK
./build.sh --list                            # архитектуры текущей ветки
JOBS=8 ./build.sh                            # параллельность
```

Результат — `dist/<ветка>/<архитектура>/`, по три пакета. С ветки 25.12 пакеты имеют формат
`.apk`, в более ранних — `.ipk`.

Что делает скрипт:

1. Скачивает SDK нужного выпуска и цели с `downloads.openwrt.org`.
2. Настраивает feeds `base` и `luci` (нужен `luci.mk`). Клоны кешируются в
   `build/feeds-cache-<версия>/` и подключаются к SDK жёсткими ссылками.
3. Подключает `package/roamd` и `package/luci-app-roamd` символическими ссылками.
4. Собирает пакеты и раскладывает по `dist/<ветка>/<архитектура>/`.

Архитектура проверяется по `CONFIG_TARGET_ARCH_PACKAGES` собранного SDK — при расхождении с
таблицей скрипт предупредит. Сбой одной архитектуры не останавливает остальные; в конце
печатается сводка, код возврата ненулевой, если хоть одна не собралась.

Кеш feeds для новой точки той же ветки не нужно набирать с нуля: теги соседних выпусков уже
лежат в клоне, `./scripts/feeds update` переключится на нужный.

```sh
cp -al build/feeds-cache-24.10.6 build/feeds-cache-24.10.8
```

## Матрица веток и архитектур

| Файл | Формат | Назначение |
|---|---|---|
| `targets/branches.txt` | `<ветка> <версия SDK>` | поддерживаемые ветки и точка, чьим SDK они собираются |
| `targets/<ветка>.txt` | `<архитектура> <цель>/<подцель>` | архитектуры пакетов ветки и цель SDK, из которой каждая собирается |

Поддерживаются ветки **23.05, 24.10, 25.12** и все архитектуры пакетов, которые в них есть:
31, 34 и 33 соответственно — 98 сборок.

Собирается **одна сборка на пару «ветка × архитектура»**. Внутри ветки ABI библиотек, от которых
зависит `roamd`, не меняется, и пакет, собранный SDK одной точки, ставится на любую точку этой
ветки. Между ветками ABI различаются — там нужна отдельная сборка.

Чтобы добавить архитектуру, впишите строку в `targets/<ветка>.txt`. Соответствие цели и
архитектуры задают поля `ARCH`, `CPU_TYPE` и `CPU_SUBTYPE` в `target/linux/<цель>/Makefile`
дерева OpenWrt. Архитектура устройства: `. /etc/openwrt_release; echo "$DISTRIB_ARCH"`.

## Сборка вручную

```sh
tar --zstd -xf openwrt-sdk-24.10.8-ramips-mt7621_gcc-*_musl.Linux-x86_64.tar.zst
cd openwrt-sdk-24.10.8-*

cat > feeds.conf <<'EOF'
src-git-full base https://git.openwrt.org/openwrt/openwrt.git;v24.10.8
src-git luci https://git.openwrt.org/project/luci.git
EOF

./scripts/feeds update base luci
./scripts/feeds install -a -p luci

ln -s /путь/к/roamd/package/roamd package/roamd
ln -s /путь/к/roamd/package/luci-app-roamd package/luci-app-roamd

make defconfig
echo CONFIG_PACKAGE_roamd=m >> .config
echo CONFIG_PACKAGE_luci-app-roamd=m >> .config
echo CONFIG_PACKAGE_luci-i18n-roamd-ru=m >> .config
make defconfig

make package/roamd/compile package/luci-app-roamd/compile -j$(nproc)
```

Сканер пакетов OpenWrt ищет в Makefile строку `call BuildPackage`. У приложения LuCI её
выполняет `luci.mk`, поэтому в конце его Makefile стоит строка-сигнатура
`# call BuildPackage - OpenWrt buildroot signature`.

## Версии пакетов

Номер выпуска держится целиком в `PKG_VERSION` в формате `X.Y.Z-rN`, `PKG_RELEASE` пуст:

| Файл | Строка |
|---|---|
| `package/roamd/Makefile` | `PKG_VERSION:=1.1.0-r1` |
| `package/luci-app-roamd/Makefile` | `PKG_VERSION:=1.1.0-r1`, `PKG_PO_VERSION:=$(PKG_VERSION)` |

* Пакеты нумеруются независимо: изменился демон — поднимается версия `roamd`,
  изменился интерфейс — `luci-app-roamd`.
* Мелкая правка — `N+1`; новая функция — `Z+1` и `N` заново. Версия только растёт.
* После дефиса обязательно `r` и число: `apk` другого вида не принимает.
* `PKG_RELEASE` пуст, потому что иначе итоговая версия пакета выглядит по-разному в 23.05 и
  24.10+, а версия пакета интерфейса на 23.05 не меняется вовсе.
* `PKG_PO_VERSION` задан явно, иначе `luci.mk` подставит пакету перевода собственную версию.
* Метки вроде `rc`, `beta` в версию пакета не попадают: `apk` отвергает такую версию, а `opkg`
  считает `1.0.0-rc1` новее `1.0.0`. Им место только в теге выпуска.

Устройство решает, есть ли обновление, только по версии пакета: изменённый код без поднятой
версии на роутеры не приедет. Контроллер Mesh Wi-Fi-системы сравнивает полные версии, поэтому
смена только `N` тоже видна.

## Репозиторий проекта

GitHub не даёт каталогов внутри релиза, а `opkg` и `apk` требуют индекс по фиксированному
адресу. Поэтому на каждую пару «ветка × архитектура» заведён **свой релиз с постоянным тегом**
`feed-<ветка>-<архитектура>`, и его адрес — адрес репозитория:

```
https://github.com/Ground-Zerro/roamd/releases/download/feed-24.10-mipsel_24kc
        └─ Packages, Packages.gz, Packages.sig, roamd_*.ipk, luci-app-roamd_*.ipk, …

https://github.com/Ground-Zerro/roamd/releases/download/feed-25.12-aarch64_cortex-a53
        └─ packages.adb, roamd-*.apk, luci-app-roamd-*.apk, …
```

Эти релизы перезаписываются при каждом выпуске, тег не меняется — адрес у пользователей остаётся
прежним. Рядом создаётся версионный релиз `v<версия>` со всеми пакетами сразу; имена файлов
начинаются с ветки и архитектуры (`25.12-x86_64-roamd-1.1.0-r1.apk`).

При публикации устаревшие пакеты удаляются: в релизе фида — **после** загрузки нового индекса
(пока он не загружен, роутеры читают старый, и его пакеты должны быть на месте), в версионном
релизе — до загрузки и только для пар текущего запуска.

### Подпись

Проверка подписи в OpenWrt включена по умолчанию. Индекс подписывается `usign` для веток 23.05
и 24.10 и ключом RSA для 25.12. Публичные ключи лежат в `keys/`, оттуда их берёт `install.sh`.

Ключи для своего форка:

```sh
usign -G -s roamd.sec -p keys/roamd-usign.pub -c "roamd feed"
usign -F -p keys/roamd-usign.pub > keys/roamd-usign.fp

openssl genrsa -out roamd.pem 2048
openssl rsa -in roamd.pem -pubout -out keys/roamd-apk.rsa.pub
```

Содержимое `roamd.sec` и `roamd.pem` кладётся в секреты репозитория `USIGN_SECRET` и
`APK_SIGN_KEY`; сами файлы в репозиторий не попадают.

### GitHub Actions

`.github/workflows/release.yml` запускается по тегу `v*` или вручную (можно указать ветки через
пробел).

| Задача | Действие |
|---|---|
| `matrix` | строит матрицу из `targets/branches.txt` и `targets/<ветка>.txt` |
| `build` | на каждую пару: SDK, `build.sh`, индекс с подписью (`repo.sh index-feed`) |
| `publish` | обновляет релизы `feed-*` и версионный релиз (`repo.sh publish`) |

Запуск с частью веток дополняет релиз, а не заменяет: пары, не попавшие в запуск, остаются
нетронутыми.

Те же шаги работают локально:

```sh
RELEASE=24.10 ./build.sh mipsel_24kc
./repo.sh index-feed 24.10 mipsel_24kc <каталог с ключами>
```

## Установка на устройстве

`install.sh` определяет ветку и архитектуру, проверяет свободное место и DNS, кладёт публичный
ключ, добавляет репозиторий в `/etc/opkg/customfeeds.conf` или
`/etc/apk/repositories.d/roamd.list` без дублей и ставит пакеты штатным `opkg install` /
`apk add`. `install.sh --check` только показывает ветку, архитектуру, адрес репозитория и
пакетный менеджер.

## Свой репозиторий

Контроллер Mesh Wi-Fi-системы берёт пакеты для узлов и собственного обновления из
`roamd.mesh.pkg_url`. Пусто — репозиторий проекта (адрес зашит в демоне).

| Вид адреса | Пример | Куда обращается контроллер |
|---|---|---|
| с подстановкой | `https://example.org/releases/download/feed-%b-%a` | `%b` — ветка, `%a` — архитектура |
| обычный | `https://192.168.1.10:8079` | `<адрес>/<ветка>/<архитектура>/` |

В каталоге контроллер ищет `Packages`, а если его нет — `packages.adb`, и сам файл пакета.
Принимается только `https://`. Сертификат своего репозитория кладётся на контроллер в
`/etc/roamd/pkg.crt`, и контроллер проверяет сервер по нему. Репозиторий проекта проверяется по
системным корневым сертификатам, файл для него не нужен. Возвращаясь к репозиторию проекта,
удалите `/etc/roamd/pkg.crt`: пока файл лежит, контроллер проверяет по нему любой репозиторий.

Для локальной раздачи собранного `dist/` есть `repo.sh`:

```sh
./repo.sh              # построить индексы Packages и поднять раздачу dist/ по HTTPS
./repo.sh index        # только индексы
./repo.sh stop         # остановить раздачу
PORT=8080 ./repo.sh    # другой порт (по умолчанию 8079)
```

Раздача идёт по HTTPS с самоподписанным сертификатом `build/repo.crt` (`CN=<адрес>`),
выпускаемым при первом запуске. Скрипт печатает команды для контроллера:

```sh
scp build/repo.crt root@<контроллер>:/etc/roamd/pkg.crt
uci set roamd.mesh.pkg_url='https://<адрес>:8079'
uci commit roamd
```
