#!/bin/sh

export SDCARD_PATH=/mnt/SDCARD
export PATH=$SDCARD_PATH/system/bin:$PATH
export LD_LIBRARY_PATH=$SDCARD_PATH/system/lib:$LD_LIBRARY_PATH
export trimui_show=0 # disable stock low battery indicator

# update
if [ -f $SDCARD_PATH/system.zip ]; then
	cd $SDCARD_PATH
	rm -rf system
	unzip -q system.zip
	rm -f system.zip
fi

# update bootlogo if changed
bootlogo

# frontend
ui
