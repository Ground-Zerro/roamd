<a id="ru"></a>

# roamd — бесшовный роуминг Wi-Fi для OpenWrt

**RU** | [EN](#en)

Клиент цепляется за дальнюю точку и не уходит на ближнюю, держит 2,4 ГГц рядом с
пустым 5 ГГц, а при переходе рвёт соединение на несколько секунд. `roamd` чинит это
на стандартном OpenWrt: без `usteer`, `dawn` и правки конфигов руками.

## Что это

Два пакета:

* **`roamd`** — демон на C: band steering, 802.11v BSS Transition Management,
  синхронизация 802.11k Neighbor Report, автонастройка 802.11r.
* **`luci-app-roamd`** — страница настройки в LuCI (**Сеть → Роуминг Wi-Fi**),
  русский перевод — отдельным пакетом `luci-i18n-roamd-ru`.

## Что делает

| Механизм | Назначение |
|---|---|
| Band steering | Не отвечает клиенту в непредпочитаемом диапазоне, пока доступен предпочитаемый |
| 802.11v BTM | Просит подключённого клиента перейти в другой диапазон |
| 802.11k | Точки обмениваются Neighbor Report — клиент находит соседа без полного сканирования |
| 802.11k Beacon Request | Демон просит клиента измерить второй диапазон: решение по реальному RSSI |
| 802.11r | Быстрый переход без повторной аутентификации |
| Мастер настройки | Создаёт готовую сеть с роумингом по имени, шифрованию и паролю |
| Автонастройка | Опции 802.11k/v/r прописываются в `/etc/config/wireless` сами |

Пороги подобраны консервативно: демон вмешивается, только когда переход точно не
ухудшит связь.

Кроме роуминга внутри одного роутера, `roamd` объединяет несколько устройств OpenWrt
в **Mesh Wi-Fi-систему** — вкладка **MESH** в LuCI: захват устройства из локальной
сети, раскатка профиля на узлы, общая таблица клиентов и журнал переходов. Узел
становится «тупой» точкой доступа в единой подсети — без своего DHCP и NAT, пакеты
на него контроллер ставит сам, под его ветку и архитектуру. Связь узлов — кабелем
или по скрытой беспроводной транспортной сети. Один бинарник совмещает обе роли.

## Быстрый старт

Нужен OpenWrt ветки 23.05, 24.10 или 25.12 и два радиоинтерфейса разных диапазонов.
На роутере:

```sh
wget -O - https://raw.githubusercontent.com/Ground-Zerro/roamd/main/install.sh | sh
```

Скрипт определит ветку и архитектуру, подключит подписанный репозиторий roamd и
поставит пакеты штатным `opkg` (или `apk` на 25.12). Повторный запуск обновляет.

Дальше ничего делать не нужно: служба встанет в автозагрузку, параметры 802.11k/v/r
пропишутся сами. Если сеть с одинаковым именем на обоих диапазонах уже настроена,
роуминг заработает сразу. Если нет — **Сеть → Роуминг Wi-Fi**, кнопка
**Создать сеть с роумингом…**.

## Лицензия

GPL-3.0-only, см. [LICENSE](LICENSE).

---

<a id="en"></a>

# roamd — seamless Wi-Fi roaming for OpenWrt

[RU](#ru) | **EN**

A client clings to a distant access point instead of moving to the near one, stays on
2.4 GHz next to an idle 5 GHz band, and drops the link for several seconds when it finally
switches. `roamd` fixes that on stock OpenWrt: without `usteer`, `dawn` or hand-edited
configuration files.

## What it is

Two packages:

* **`roamd`** — a daemon written in C: band steering, 802.11v BSS Transition Management,
  802.11k Neighbor Report synchronisation, automatic 802.11r setup.
* **`luci-app-roamd`** — the settings page in LuCI (**Network → Wi-Fi Roaming**);
  the Russian translation ships separately as `luci-i18n-roamd-ru`.

## What it does

| Mechanism | Purpose |
|---|---|
| Band steering | Does not answer a client on the non-preferred band while the preferred one is available |
| 802.11v BTM | Asks a connected client to move to the other band |
| 802.11k | Access points exchange Neighbor Reports — a client finds its neighbour without a full scan |
| 802.11k Beacon Request | The daemon asks the client to measure the other band: the decision is made on real RSSI |
| 802.11r | Fast transition without a full reauthentication |
| Setup wizard | Creates a ready roaming network from a name, an encryption type and a password |
| Automatic configuration | The 802.11k/v/r options are written into `/etc/config/wireless` for you |

Thresholds are deliberately conservative: the daemon steps in only when the transition is
certain not to make the link worse.

Beyond roaming inside a single router, `roamd` joins several OpenWrt devices into a
**Mesh Wi-Fi system** — the **MESH** tab in LuCI: capturing a device from the local network,
rolling the profile out to the nodes, a shared client table and a transition log. A node
becomes a dumb access point in one common subnet — with no DHCP or NAT of its own; the
controller installs the packages on it itself, matching the node's branch and architecture.
Nodes are linked by cable or over a hidden wireless backhaul. One binary covers both roles.

## Quick start

You need OpenWrt 23.05, 24.10 or 25.12 and two radios on different bands. On the router:

```sh
wget -O - https://raw.githubusercontent.com/Ground-Zerro/roamd/main/install.sh | sh
```

The script detects the branch and the architecture, connects the signed roamd repository and
installs the packages with the system's own `opkg` (or `apk` on 25.12). Running it again
upgrades them.

Nothing else has to be done: the service is added to autostart and the 802.11k/v/r options
are written on their own. If a network with the same name is already configured on both
bands, roaming starts working right away. If not — **Network → Wi-Fi Roaming**, the
**Create a roaming network…** button.

## License

GPL-3.0-only, see [LICENSE](LICENSE).
