## Command line arguments

	Long form						Short form	Documentation
	--waitbutton					-b			Waits for the button on the programmer board to be pressed
	--erase							-e			Erases the whole flash memory, first chip only
	--erasechips <num chips>		-ec			Erases the whole flash memory for the number of chips, in parallel
	--eraseblock <number>			-eb			Erases one block of the flash memory using the number value to choose which block. Each flash chip has 256 blocks (banks)
												This depends on the geometry of the flash memory. For example for the chip M29F160FT55N3E2, using 253 will erase the 8KB block at 0x1fa000
	--write <filename>				-w			Writes the file to the flash memory
	--writebyte <address> <byte>	-wb			Write a single byte to the address
	--read <num banks> <filename>	-r			Reads the whole cartridge for the number of banks and writes the data to the filename as a binary file
	--loop							-l			Loops back to the start of the command line arguments
	--dump <address> <num bytes>	-d			Dumps the flash memory from address for the number of bytes
	--slots <num>					-s			The nunber of slots being used, default is 1, from the front slot to the rear slot

For example, to repeatedly erase and write a file to the cartridge while waiting for the button to be pressed before each erase/write pass:

	--waitbutton --erase --write scrollerbanks.bin --loop


Or for example, to read data from a 256 bank cartridge to a file:

	--read 256 readdata.bin

Or to dump 15 bytes from 0x1fa000, erase the whole block, dump the same bytes to show they are erased, then write a single byte to 0x1fa007, and then dump the bytes to show the single updated byte:

	--dump 0x1fa000 0xf --eraseblock 253 --dump 0x1fa000 0xf  --writebyte 0x1fa007 0x56  --dump 0x1fa000 0xf

## Minimal cartridge boot test

The file CartBoot.bin (assembled from C64MegaCart\CartBoot.a) is a very small minimal cartridge boot which will show a message

	--erase --write CartBoot.bin


## Errors

If all LEDs rapidly flash on and off this means there was an error. Usually this happens during erase or write.
The tool will wait for a button to be pressed before executing the next command in the command line.


## Cartridge types

Different cartridges have different requirements for how to write the flash.
C64MegaCart is the default option for this cart tool, so these advanced options are not required, however other cartridges can be configured.
The advanced configuration command line options must be used before the cartridge is accessed.

| Type			| Command line options					| Size (megabytes)	| Typical time to erase and write	|
| ---			| ---									| ---				| ---								|
| C64MegaCart	| --cfwch --cfwdci 0 --crdms 0 --cfwnec	| 2					| 60 seconds						|
| Megabyter		| --cfwcl --cfwdci 4 --crdms 1 --cfwec	| 1					| 44 seconds						|
| Gmod2			| --cfct 1								| 0.5				| 44 seconds						|

### Advanced configuration options

These options alter the timings and other signals expected by different cartridges types.

	Long form			Documentation
	--cfwch				HIROM is used to write to the cartridge flash
	--cfwcl				LOROM is used to write to the cartridge flash
	--cfwdci <number>	Number of times the dot clock signal is toggled during IO/read/write phases
	--crdms <number>	Delay in microseconds
	--cfwec				Use extra clocking for signal generation. This is closer to the standard C64 signal timing, but is slower.
	--cfwnec			No extra clocking for signal generation. This option if faster but some cartridges are unable tolerate the non-standard timings.
	--cfct <number>		0 = M29F160, 1 = M29F040


## Setup software using gcc

Video showing the whole process: https://youtu.be/GBg5k46B5Pw

This software uses: https://github.com/WiringPi/WiringPi

	sudo apt install git
	git clone https://github.com/WiringPi/WiringPi.git

Or if that repository disappears:

	git clone https://github.com/martinpiper/WiringPi.git

Then compile with:

	cd WiringPi
	./build
	cd ..

To compile the programmer tool:

	git clone https://github.com/martinpiper/C64MegaCart.git
	cd C64MegaCart/PiCartTool
	gcc main.cpp -o PiCartTool -lwiringPi -O3

You can then use this command to test erase and write the cartridge:

	./PiCartTool --waitbutton --erase --write scrollerbanks.bin --loop



## Setup software using Microsoft Dev Studio

Usually gcc builds will work fine, this is more for remote development and debugging.

MSDev->Tools->Options->Cross Platform->Connection Manager
Default Raspberry Pi login: user:pi password:raspberry


	cd ~/projects/PiCartTool/bin/ARM/Release/
	time ./PiCartTool.out --erase --write ../../../scrollerbanks.bin
	diff -q ../../../scrollerbanks.bin ../../../readdata.bin
	cmp -l ../../../scrollerbanks.bin ../../../readdata.bin | mawk 'function oct2dec(oct,     dec) {for (i = 1; i <= length(oct); i++) {dec *= 8; dec += substr(oct, i, 1)}; return dec} {printf "%08X %02X %02X\n", $1-1, oct2dec($2), oct2dec($3)}'



RaspberryPi5 erase 2MB + write 2MB + read/verify 2MB cycle = 1m17s
The PiZero took about 2m for the same erase/write
The full read/verify on its own takes 17s
It's probably not needed since the write uses a read to verify the write has completed.
