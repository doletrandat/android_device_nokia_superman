# CC56: f_mass_storage as a module (CONFIG_USB_CONFIGFS_MASS_STORAGE is off in the kernel; libcomposite and
# configfs are built in). Used to present the phone's internal storage READ-ONLY to a PC over USB.
ccflags-y += -I$(srctree)/drivers/usb/gadget -I$(srctree)/drivers/usb/gadget/function
obj-m += usb_f_mass_storage.o
usb_f_mass_storage-y := f_mass_storage.o storage_common.o
