The Linux GPIB Package
 -----------------------------------
  (c) 1994-1996 by C.Schroeter
  (c) 2001-2011 by Frank Mori Hess

=====================================================

This is a GPIB/IEEE-488 driver and utility package for LINUX.

This software distribution package linux-gpib-4.3.7.tar.gz contains
this README and two tarballs:

1) kernel modules in linux-gpib-kernel-4.3.7.tar.gz

  This package is only required if your kernel version is < 6.13 or
  you wish to use this package rather than the in-tree kernel drivers.
  Untar the file and see the INSTALL file for detailed instructions on
  building and installing.

    $ tar xzvf linux-gpib-kernel-4.3.7.tar.gz
    $ cd linux-gpib-kernel-4.3.7
    $ make -j4
    $ sudo rm -rf /lib/modules/`uname -r`/gpib
    $ sudo make install
  
  If your kernel version is >= 6.13

    Check whether your distro has compiled the modules:

    $ ls /lib/modules/`uname -r`/kernel/drivers/staging/gpib
    agilent_82350b  agilent_82357a  common  nec7210  ni_usb  tms9914  tnt4882

    If not obtain the kernel sources for your distro.
    On debian use something like: sudo apt install linux-source-$(uname -r)

    $ cd <top_level_dir_kernel_source>
    $ make menuconfig

    Navigate to drivers->staging->gpib
    Select m
    In the follwing menu select the drivers you need
    Exit and save

    $ make -j4 M=drivers/staging/gpib
    $ sudo make modules_install
    $ sudo reboot

    
2) user space software in linux-gpib-user-4.3.7.tar.gz containing the
   config program, library, device scripts, examples and documentation

   Untar the file and see the INSTALL file for detailed instructions on
   building and installing.
   $ tar xzvf linux-gpib-user-4.3.7.tar.gz
   $ cd linux-gpib-user-4.3.7
   $ ./configure --sysconfdir=/etc
   $ make
   $ sudo make install

Send comments, questions and suggestions to to the linux-gpib mailing
list at linux-gpib-general@lists.sourceforge.net

Release Notes for linux-gpib-4.3.7
----------------------------------

Changes since the linux-gpib-4.3.6 release

	Improve device mode support for ni_usb with
	device clear and device trigger events.

        Support for xyphro usb to GPIB converter.

	Changes to the docs and enable generation of manpages.

	ChangeLogs are now specific to user and kernel parts and
	limited to entries since the last release.

	Add support for configuring a minor with gpib_config without
	requiring a corresponding entry in the gpib.conf file.

	Rework of all drivers for addition to the staging directory
	of the standard linux kernels from 6.13 onward. The drivers
	in the linux-gpib-kernel package are the same as those in
	the standard kernel with the addition of the compatibility
	functions to support earlier kernels and version info.

	The gpib_user.h include file has been updated to support
	existing applications using the new kernel coding style
	compliant gpio.h include file.

	Allow secondary address value of 31

        Use lookup tables for gpios in bitbang driver

	Add support for asserting REN conditionally when
      	configuring a board to become system controller

	Enable IB_NO_ERROR for ibdev() and ensure proper error returns

	Support for suspend and resume for agilent and ni usb drivers

	Add support for the IbcUnAddr option in ibconfig to send
	untalk/unlisten	after ibrd*/ibwrt*
	
	See ChangeLog for bug fixes and other changes since 4.3.6
	  
Note: If you have any pre 4.3.7 gpib udev rules files in
      /etc/udev/rules.d/ please backup 98-gpib-generic.rules and remove
      them before installing linux-gpib-user-4.3.7.
      After installation of the user space package re-customize
      98-gpib-generic.rules if needed. This is needed because these files
      are not overwritten upon installation.
      
      The files to remove are:
	   98-gpib-generic.rules
	   99-agilent_82357a.rules
	   99-ni_usb_gpib.rules
	   99-lpvo_usb_gpib.rules

      If you have the pre 4.3.7 modules installed you must remove
      them before installing the 4.3.7 kernel modules since some
      modules have been moved or renamed from the previous version.
      $ sudo rm -rf /lib/modules/`uname -r`/gpib

