import os
import struct
from os import * 

f = os.open("/sys/module/blec_usb/parameters/access_count", O_RDONLY)

print 'access_count = ' + os.read(f, 2)

path = '/dev'

devlist = os.listdir(path)

if len(devlist) == 0:
	exit()

for dev in devlist:
	if "lab" in dev:
		print dev

labjack_num = raw_input('which Labjack number in /dev? ')
labjacka = "/dev/lab" + labjack_num + "portA"
labjackb = "/dev/lab" + labjack_num + "portB"
labjackc = "/dev/lab" + labjack_num + "portC"


fda = os.open(labjacka, O_RDONLY)
fdb = os.open(labjackb, O_RDWR)
fdc = os.open(labjackc, O_RDONLY)

if fda == 0:
	print("lab0portA was not opened properly")
	ainfo = -1
else:
	ainfo = os.read(fda, 15)


if fdb == 0:
	print("lab0portB was not opened properly")
	binfo = -1
else:
	binfo, = struct.unpack('B', read(fdb,1))
	os.write(fdb, struct.pack('B', 1))
	raw_input("press enter to continue")


if fdc == 0:
	print("lab0portC was not opened properly")
	cinfo = -1
else:
	cinfo, = struct.unpack('i', os.read(fdc, 4))


print 'port A read value:', ainfo
print 'port B read value:', binfo
print 'port C read value:', cinfo

