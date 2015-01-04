#_DEPRECATED_
Due to my employer's intellectual property policy I can't work on this project anymore. I recommend forking it if you would like updates.

---


Tomtom cardio watch reader / driver for linux.
==============================================

Linux application that reads activity files directly from the Tomtom multisport cardio watches.
The activity files will be written to the current folder using the 'YYYY-MM-DD_HH:MM::SS.ttbin' filename pattern.
TTBIN files can be converted using the Tomtom software or you can check my other repository for code snippets to decode them.

Usage:
------
 sudo ./extract_files
### Options:
 * --all Extract all the files instead of just the activity files.
 * --vid=... To change the vendor id used to find the watch.
 * --pid=... To change the product id used to find the watch.

Notes:
------
0. The error handling is limited as I don't completely understand the protocol used. Unplugging the watch and re-pluggin it usually fixes things.
1. Running this application requires writing to the USB device, so usually 'sudo' is needed. This can be avoided by copying the 99-tomtom.rules files to '/etc/udev/rules.d'. Check the comments in the file for more details.

Prerequisites:
--------------
libusb 1.0

To compile:
-----------
g++ extract_files.c -I/usr/include/libusb-1.0/ -lusb /usr/lib64/libusb-1.0.so --std=c++11 -o extract_files
