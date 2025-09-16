## Command line arguments

	Long form				Short form	Documentation
	--waitbutton			-b			Waits for the button on the programmer board to be presse
	--erase					-e			Erases the whole flash memor
	--eraseblock <number>	-eb			Erases one block of the flash memory using the number value to choose which block.
										This depends on the geometry of the flash memory. For example for the chip M29F160FT55N3E2, using 253 will erase the 8KB block at $1fa000.
	--write <filename>		-w			Writes the file to the flash memor
	--read <filename>		-r			Reads the whole cartridge and writes the data to the filename as a binary fil
	--loop					-l			Loops back to the start of the command line arguments

For example, to repeatedly erase and write a file to the cartridge while waiting for the button to be poressed before each erase/write pass:

	--waitbutton --erase --write scrollerbanks.bin --loop

## Errors

If all four LEDs rapidly flash on and off this means there was an error. Usually this happens during erase or write.
The tool will wait for a button to be pressed before executing the next command in the command line.


## Setup software using gcc

This software uses: https://github.com/WiringPi/WiringPi

sudo apt install git
git clone https://github.com/WiringPi/WiringPi.git
Or if that repository disappears: git clone https://github.com/martinpiper/WiringPi.git
cd WiringPi
./build
cd ..

git clone https://github.com/martinpiper/C64MegaCart.git
cd C64MegaCart/PiCartTool
gcc main.cpp -o PiCartTool -lwiringPi -O3

You can then use this command to test erase and write the cartridge: ./PiCartTool --waitbutton --erase --write scrollerbanks.bin --loop



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
