
default: $(KERNEL_DIR)/.pre_kernelbuild

include $(BUILD_DIR)/make.common

COMPRESS_OPTION = $(shell which pigz > /dev/null 2>&1 && echo "--use-compress-program=pigz" || echo "-z")

define kernel_cfg_rm_bcm_kf
	sed -i.bak -e "/^CONFIG_BCM_.*=[my]/d" $(KERNEL_DIR)/.config && \
	sed -i.bak -e "/default [my]/d" $(BCM_KF_KCONFIG_FILE) && \
	TOOLCHAIN_TOP= $(MAKE) -C $(BUILD_DIR)/build -f Bcmkernel.mk olddefconfig
endef

define android_kernel_merge_cfg
cd $(KERNEL_DIR); \
ARCH=${KARCH} scripts/kconfig/merge_config.sh -m arch/$(KARCH)/defconfig android/configs/android-base.cfg android/configs/android-recommended.cfg android/configs/android-bcm-recommended.cfg ;
endef

$(KERNEL_DIR)/.pre_kernelbuild: $(BCM_KF_KCONFIG_FILE)
	@echo
	@echo -------------------------------------------
	@echo ... starting kernel build at $(KERNEL_DIR)
	@echo PROFILE_KERNEL_VER is $(PROFILE_KERNEL_VER)
	@echo BCM_KF is $(if $(BCM_KF),,un)defined
	@cd $(INC_KERNEL_BASE); \
	if [ -e $(KERNEL_DIR)/.git_repo ]; then \
		cd $(KERNEL_DIR)/ && chmod +x ./get_kernel && ./get_kernel && cd -; \
	elif [ ! -e $(KERNEL_DIR)/.untar_complete ]; then \
		echo "Untarring original Linux kernel source: $(LINUX_ZIP_FILE)"; \
		(tar $(COMPRESS_OPTION) -xkpf $(LINUX_ZIP_FILE) 2> /dev/null || true); \
		touch $(KERNEL_DIR)/.untar_complete; \
	fi && \
	cat $(HOSTTOOLS_DIR)/scripts/defconfig-bcm.template > $(KERNEL_DIR)/arch/$(KARCH)/defconfig && \
	# $(GENDEFCONFIG_CMD) $(PROFILE_PATH) ${MAKEFLAGS} && \
	# cp -f $(KERNEL_DIR)/arch/$(KARCH)/defconfig $(KERNEL_DIR)/.config && \
	# $(if $(strip $(BRCM_ANDROID)), $(call android_kernel_merge_cfg), true) && \
	$(MAKE) -C $(BUILD_DIR)/build -f Bcmkernel.mk olddefconfig && \
	$(if $(BCM_KF), true, $(call kernel_cfg_rm_bcm_kf)) && \
	touch $(KERNEL_DIR)/.pre_kernelbuild;
