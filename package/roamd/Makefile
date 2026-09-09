include $(TOPDIR)/rules.mk

PKG_NAME:=roamd
PKG_VERSION:=1.0.0-r51
PKG_RELEASE:=

PKG_LICENSE:=GPL-3.0-only
PKG_MAINTAINER:=Ground-Zerro

PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/roamd
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=wireless
  TITLE:=Wi-Fi band steering and fast roaming daemon
  DEPENDS:=+libubox +libubus +libuci +libblobmsg-json
  URL:=https://github.com/Ground-Zerro/roamd
endef

define Package/roamd/description
  Seamless Wi-Fi roaming for OpenWrt. Steers dual-band clients between the
  2.4 GHz and 5 GHz interfaces of the same SSID using 802.11v BSS Transition
  Management, keeps 802.11k neighbour reports in sync between the local BSSes and
  provisions 802.11r fast transition in the wireless configuration.
  BSS Transition needs 802.11v (WNM), which the basic and mini wpad builds are
  compiled without: hostapd rejects the bss_transition and wnm_sleep_mode options
  and leaves every BSS disabled. The daemon therefore installs a full wpad build
  itself (deps-ensure.sh, run on service start and during node enrollment),
  keeping the TLS backend the device already uses.
endef

define Package/roamd/conffiles
/etc/config/roamd
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Package/roamd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/roamd $(1)/usr/sbin/roamd
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/roamd.config $(1)/etc/config/roamd
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/roamd.init $(1)/etc/init.d/roamd
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./files/roamd.acl.json $(1)/usr/share/rpcd/acl.d/roamd.json
	$(INSTALL_DIR) $(1)/usr/libexec/roamd
	$(INSTALL_DATA) ./files/mesh-lib.sh $(1)/usr/libexec/roamd/mesh-lib.sh
	$(INSTALL_DATA) ./files/mesh-rpc.sh $(1)/usr/libexec/roamd/mesh-rpc.sh
	$(INSTALL_BIN) ./files/mesh-discover.sh $(1)/usr/libexec/roamd/mesh-discover.sh
	$(INSTALL_BIN) ./files/mesh-acquire.sh $(1)/usr/libexec/roamd/mesh-acquire.sh
	$(INSTALL_BIN) ./files/mesh-update.sh $(1)/usr/libexec/roamd/mesh-update.sh
	$(INSTALL_BIN) ./files/mesh-profile.sh $(1)/usr/libexec/roamd/mesh-profile.sh
	$(INSTALL_BIN) ./files/mesh-poll-all.sh $(1)/usr/libexec/roamd/mesh-poll-all.sh
	$(INSTALL_BIN) ./files/mesh-selfcheck.sh $(1)/usr/libexec/roamd/mesh-selfcheck.sh
	$(INSTALL_BIN) ./files/deps-ensure.sh $(1)/usr/libexec/roamd/deps-ensure.sh
	$(INSTALL_BIN) ./files/net-apply.sh $(1)/usr/libexec/roamd/net-apply.sh
	$(INSTALL_BIN) ./files/mesh-release.sh $(1)/usr/libexec/roamd/mesh-release.sh
endef

define Package/roamd/postinst
#!/bin/sh
rm -f "$${IPKG_INSTROOT}/etc/roamd/mesh_log.jsonl" "$${IPKG_INSTROOT}/etc/roamd/clients.jsonl"
exit 0
endef

$(eval $(call BuildPackage,roamd))
