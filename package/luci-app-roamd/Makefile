include $(TOPDIR)/rules.mk

LUCI_TITLE:=Wi-Fi roaming configuration
LUCI_DEPENDS:=+luci-base +roamd
LUCI_PKGARCH:=all

PKG_VERSION:=1.1.0-r1
PKG_PO_VERSION:=$(PKG_VERSION)
PKG_RELEASE:=
PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=Ground-Zerro

define Package/luci-app-roamd/postinst
#!/bin/sh
[ -n "$$IPKG_INSTROOT" ] && exit 0
touch /www/luci-static/resources/view/roamd/*.js 2>/dev/null
rm -rf /tmp/luci-modulecache /tmp/luci-indexcache* 2>/dev/null
exit 0
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
