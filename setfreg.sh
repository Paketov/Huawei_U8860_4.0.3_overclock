#!/system/bin/sh
#Set max frequrecy

insmod /system/lib/modules/setfreg.ko
cd /proc/overclock
echo 1000 > cur_index
echo 1708800 > clk_khz
echo 1400 > vdd_mv
echo 250 > vdd_raw
echo 5865344 > lpj
echo 89 0 1 0 > pll

