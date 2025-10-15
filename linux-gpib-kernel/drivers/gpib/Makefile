ifneq ($(LINUXINCLUDE),)
	LINUXINCLUDE := -I$(GPIB_TOP_DIR)/compat/include $(LINUXINCLUDE)
endif


subdir-ccflags-y := -I$(src)/include -I$(src)/uapi \
			-DHAVE_DEV_OF_NODE=$(HAVE_DEV_OF_NODE) \
			-DGPIB_CONFIG_PCMCIA=$(GPIB_CONFIG_PCMCIA) \
			-DCLASS_CREATE1ARG=$(CLASS_CREATE1ARG)

ifeq ($(GPIB_CONFIG_KERNEL_DEBUG),1)
	subdir-ccflags-y += -DDEBUG
endif

obj-y += agilent_82350b/
obj-$(CONFIG_USB) += agilent_82357a/
obj-y += cb7210/
obj-y += cec/
obj-$(CONFIG_OF) += eastwood/
obj-y += fmh_gpib/
obj-$(CONFIG_ARCH_BCM2835) += gpio/
obj-y += hp_82335/
obj-y += hp_82341/
obj-y += ines/
obj-$(CONFIG_USB) += lpvo_usb_gpib/
obj-y += nec7210/
obj-$(CONFIG_USB) += ni_usb/
ifeq ($(CONFIG_GPIB_ISA),yes)
	obj-y += pc2/
else
	obj-$(CONFIG_ISA) += pc2/
endif
obj-y += common/
obj-y += tms9914/
obj-y += tnt4882/
obj-y += xyphro/
