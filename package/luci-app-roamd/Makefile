include $(TOPDIR)/rules.mk

LUCI_TITLE:=Wi-Fi roaming configuration
LUCI_DEPENDS:=+luci-base +roamd
LUCI_PKGARCH:=all

PKG_VERSION:=1.0.1-r6
PKG_PO_VERSION:=$(PKG_VERSION)
PKG_RELEASE:=
PKG_LICENSE:=GPL-3.0-only
PKG_MAINTAINER:=Ground-Zerro

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
