ifneq ($(LINUXINCLUDE),)
	LINUXINCLUDE := -I$(GPIB_TOP_DIR)/compat/include $(LINUXINCLUDE)
endif

subdir-ccflags-y := -I$(src)/include -DHAVE_DEV_OF_NODE=$(HAVE_DEV_OF_NODE)

obj-y += agilent_82350b/
obj-$(CONFIG_USB) += agilent_82357a/
obj-y += cb7210/
obj-y += cec/
obj-y += eastwood/
obj-y += fmh_gpib/
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
obj-y += sys/
obj-y += tms9914/
obj-y += tnt4882/

