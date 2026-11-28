## Command line arguments

	Long form								Short form	Documentation
	--help									-h			Shows the version and these lists of options (from this ReadMe.md next to the program), without any access of the cartridges.
	--version								-			Shows the version of PiCartTool: what the build gave as PICARTTOOL_VERSION (the build script of the cartridge repository gives the C64MegaCart commit, the patches and its own commit; with git: -DPICARTTOOL_VERSION="\"$(git describe --always --dirty)\"") and the build time.
	--waitbutton							-b			Waits for the button on the programmer board to be pressed
	--erase									-e			Erases the whole flash memory, first chip only
	--eraseserial							-es			Erases the whole serial EEPROM memory
	--erasechips <num chips>				-ec			Erases the whole flash memory for the number of chips, in parallel
	--eraseblock <number>					-eb			Erases one block of the flash memory using the number value to choose which block. Each flash chip has 256 blocks (banks)
														This depends on the geometry of the flash memory. For example for the chip M29F160FT55N3E2, using 253 will erase the 8KB block at 0x1fa000
	--write <filename>						-w			Writes the file to the flash memory
	--writeserial <filename>				-ws			Writes the file to the serial EEPROM memory
	--writebyte <address> <byte>			-wb			Write a single byte to the address.
	--read <num banks> <filename>			-r			Reads the whole cartridge for the number of banks and writes the data to the filename as a binary file. Only the first slot.
	--ide64id								-			IDE64 V4.x in the slot of --activeslot: shows the status ($DE32), the number of 16 KB banks, the day and time of its clock and the flash chip. The flash ID can only be read after a reset with the PGM jumper set and the jumper removed again: with the jumper set it waits for the button, remove the jumper, then press it. The IDE64 only drives the data bus while BA is high, and the programmer does not drive BA: put a 1K resistor from BA to +5V (J3 pins 15 and 6 on the five slot programmer), otherwise the reads fail now and then.
	--ide64devices							-			Shows the ATA devices of an IDE64 V4.x (primary and secondary master and slave: CompactFlash, ATA disk or ATAPI device such as a CD-ROM, with model, serial number, firmware and size, from IDENTIFY, which reads nothing from the medium) and whether a 16C550 UART (Silversurfer or a compatible RS232 card) is in the clock port. The clock port test writes the scratch register $DE07 and puts its value back. The PGM jumper is not needed.
	--readide64 <filename>					-			Reads the whole flash of an IDE64 V4.x (all 16 KB banks) into a file, for example as a backup of IDEDOS before an update. The flash is read until two reads in a row are the same (at most 10), otherwise nothing is written. When an IDE64 option fails, PiCartTool exits with 1.
	--readide64setup <filename>				-			Reads the 31 bytes of RAM of the DS1302 clock chip of an IDE64 V4.x into a file and shows them. IDEDOS keeps its setup there (kept by the battery), with a check byte. The PGM jumper is not needed.
	--showide64setup [setup filename]		-			Shows the IDEDOS setup of an IDE64 V4.x by name, as the setup screens of IDEDOS show it (options, colours, device numbers, ATA devices), with its IDEDOS version (0.90 or 0.91, from the check byte). With a file of --readide64setup it shows the file and does not touch the IDE64, without a file name it reads the RAM of the DS1302. The PGM jumper is not needed.
	--writeide64setup <filename>			-			Writes a file of --readide64setup back into the RAM of the DS1302 of an IDE64 V4.x, as IDEDOS saves its setup, and reads it back. The file must have a valid IDEDOS check byte of the IDEDOS version in the flash (0.90 and 0.91 use different ones, 0.91 does not take a setup of 0.90). The clock is not changed, the PGM jumper is not needed.
	--setide64clock <now|"YYYY-MM-DD HH:MM:SS">	-			Sets the DS1302 clock of an IDE64 V4.x to the local time of the Pi ("now") or the given time and reads it back. The setup is not changed, the PGM jumper is not needed.
	--writeide64 <filename>					-			Writes an IDEDOS image (a multiple of 16 KB) into the flash of an IDE64 V4.x from address 0, like the Perom programmer on a C64. Set the PGM jumper before starting PiCartTool, it resets the IDE64, then remove the jumper and press the button when asked. Pages or sectors that are already the same are left out, everything is checked afterwards. The other programmer slots must be empty. --writeide64nowait does the same without waiting for the button.
	--swapide64 [backup filename]			-			Swaps the two IDEDOS images of 64 KB in the 128 KB flash of an IDE64 V4.x, so the one in the upper half starts (the IDE64 V4.1 always starts from the lower half and has no switch for it). The whole flash is saved to the backup file first (without a file name: ide64_backup_<date>_<time>.bin in the current directory), then the upper half is written first, so the IDEDOS that started before is never lost. The PGM jumper as with --writeide64, --swapide64nowait does the same without waiting for the button. Running it again swaps back.
	--detect								-			Names the cartridge type in each of the 5 programmer slots: EasyFlash 3, IDE64 V4.x, the DIY Gmod2 with the reversed flash data bus, EasyFlash 1 (or compatible), C64MegaCart, Megabyter, Gmod2, GMod3, Magic Desk, Ocean type A or B (with the number of banks), Fun Play or Power Play, RGCD, Hucky 64K card (not with an EasyFlash 3 in the programmer), Action Replay 4.2/5/6, Retro Replay, Final Cartridge 1/2, Final Cartridge III, Super Snapshot 5, Comal 80 (not with an EasyFlash 3 in the programmer), C64 Game System, Dinamic, Simons' BASIC, Westermann, Zaxxon, Super Games, 8K, 16K or Ultimax ROM cartridge, empty or unknown. Nothing is erased or written, except the first byte of the RAM of an EasyFlash 3 in $df00. The cartridges are reset at the start and at the end. When a slot pulls _GAME or _EXROM but drives no data (maybe a TeensyROM or Kung Fu Flash, they need a PHI2 clock), it also says whether the Pi has the udev rule of PJRC and teensy_loader_cli for a TeensyROM over USB.
	--probe									-			Shows how the cartridge in each of the 5 programmer slots answers to the flash ID command in several variants, to find out how cartridge types can be detected. Nothing is erased or written.
	--probeio1								-			Writes $de00 in several ways and shows whether a Magic Desk takes the value ($80 turns it off, $01 selects bank 1). Only for a Magic Desk alone in the programmer.
	--testefram								-			Tests the 256 bytes of RAM of an EasyFlash 1 or EasyFlash 3 at $df00-$dfff (IO2): the data lines ($00, $ff, $55, $aa, walking one and zero), the address lines A0-A7 (every byte its own address), March C- and 3 seconds of retention. Shows the wrong data and address bits for each slot and exits with 1 when a slot fails. Tests the programmer slot of --activeslot, with --slots n or --slotlist all of them (every slot gets the same writes). Only these 256 bytes are seen, and the programmer does not have the timing of a C64.
	--ef1id									-			EasyFlash 1: the flash IDs of ROML and ROMH (Am29F040 $01 $a4, M29F040 $20 $e2, MX29F040 $c2 $a4, SST39SF040 $bf $b7) and whether the bank register $de00 takes the writes (the start of ROML and ROMH in all 64 banks). With --slots n for all programmer slots 1 to n.
	--readef1 <filename>					-			Reads an EasyFlash 1 (1MB, 64 banks of ROML and ROMH) as EasyFlash .crt, empty chips are left out. Reads the programmer slot of --activeslot.
	--writeef1 <filename>					-			Writes an EasyFlash .crt (hardware type 32) or a raw file of up to 1MB (each bank 8KB ROML followed by 8KB ROMH) into an EasyFlash 1 and verifies it. Sectors of 64KB that are unchanged are left out, empty ones are not erased; before erasing, reading another bank proves that the bank register works. With --slots n all programmer slots 1 to n get the same data and each one is checked, other programmer slots must be empty.
	--ef3id									-			Reads the flash manufacturer and device ID of an EasyFlash 3 (MX29LV640EB: $c2 $cb). With --slots n for all programmer slots 1 to n.
	--ef3dir								-			Shows the names of the EasyFlash 3 menu directory: slots 1 to 7 (with "(empty)" when bank 0 of the slot is empty), KERNAL 1 to 8 and the freezers. Reads the directory and one bank of each slot of the programmer slot of --activeslot, nothing is written.
	--writeef3slot <slot> <filename>		-			Erases and writes slot 1 to 7 of an EasyFlash 3 and verifies it. The file is an EasyFlash .crt or a raw slot file of up to 1MB (each bank is 8KB ROML followed by 8KB ROMH). With --slots n all programmer slots 1 to n get the same data and each one is checked.
	--ef3slotname <slot> <name>				-			Sets the name of slot 1 to 15 in the EasyFlash 3 menu directory (1 to 16 ASCII characters). Asks for confirmation before writing to slot 0. With --slots n all programmer slots 1 to n get the same data and each one is checked.
	--ef3kernalname <kernal> <name>			-			Same for KERNAL 1 to 8.
	--ef3freezername <freezer> <name>		-			Same for freezer 1 (Retro Replay), 2 (Action Replay), 3 (Super Snapshot 5) and 4 (Final Cartridge III, only with the EF3 menu 1.3.0 or newer).
	--ef3noconfirm							-			The following EasyFlash 3 writes to slot 0 (system area) do not ask for confirmation.
	--readef3 <filename>					-ref3		Reads all 8 slots of an EasyFlash 3 (8MB) and writes them to the filename as a binary file. Each bank is 8KB ROML followed by 8KB ROMH. Reads the programmer slot of --activeslot.
	--dumpef3 <folder>						-			Reads an EasyFlash 3 like --readef3 into <folder>/ef3.bin and then extracts it like --extractef3 into the same folder.
	--extractef3 <filename> <folder>		-			Extracts an EasyFlash 3 image from --readef3: slots 1 to 7 as EasyFlash .crt, the KERNALs and freezers as .bin, named after the EF3 menu directory. Does not access the cartridge.
	--readef3slot <slot> <filename>			-			Reads slot 1 to 7 of an EasyFlash 3 as EasyFlash .crt. Reads the programmer slot of --activeslot.
	--readef3kernal <kernal> <filename>		-			Reads KERNAL 1 to 8 of an EasyFlash 3 (8KB). Reads the programmer slot of --activeslot.
	--readef3freezer <freezer> <filename>	-			Reads freezer 1 to 4 of an EasyFlash 3, without empty 8KB blocks at the end. Reads the programmer slot of --activeslot.
	--writeef3kernal <kernal> <filename>	-			Erases and writes KERNAL 1 to 8 (its 8KB boot sector) and verifies it. Asks for confirmation. With --slots n all programmer slots 1 to n get the same data and each one is checked.
	--writeef3freezer <freezer> <filename>	-			Erases and writes freezer 1 to 4 (its 64KB sectors; 4, the Final Cartridge III of the EF3 menu 1.3.0 and newer, has 256KB) and verifies it. Same file format as --readef3freezer. Asks for confirmation. With --slots n all programmer slots 1 to n get the same data and each one is checked.
	--writeef3 <filename>					-			Writes an image from --readef3 back: slots 1 to 7, the KERNALs, the freezers and the menu directory, not the EF3 menu and EasyProg. Unchanged sectors are skipped. Asks for confirmation. With --slots n all programmer slots 1 to n get the same data and each one is checked.
	--writeef3all <filename>				-			Writes all 8MB of an image from --readef3, including the EF3 menu and EasyProg. Unchanged sectors are skipped. Asks for confirmation (type WRITE ALL). With --slots n all programmer slots 1 to n get the same data and each one is checked.
	--eraseef3slot <slot>					-			Erases slot 1 to 7 of an EasyFlash 3 and sets its menu name back to "Slot n". Asks for confirmation. With --slots n for all programmer slots 1 to n.
	--eraseef3kernal <kernal>				-			Same for KERNAL 1 to 8, the name becomes "KERNAL n".
	--eraseef3freezer <freezer>				-			Same for freezer 1 to 4, the name becomes "RR Slot", "AR Slot", "SS5 Slot" or "FC3 Slot".
	--eraseef3all							-			Erases slots 1 to 7, all KERNALs and all freezers and sets all names back, like a new EasyFlash 3. The EF3 menu and EasyProg stay. Asks for confirmation (type ERASE ALL).
	--ef3resetnames							-			Sets all names in the EasyFlash 3 menu back to "Slot n", "KERNAL n", "RR Slot", "AR Slot" and "SS5 Slot" without erasing anything. Asks for confirmation.
	--readserial <filename>					-rs			Reads the whole serial EEPROM and writes the data to the filename as a binary file. Only the first slot.
	--loop									-l			Loops back to the start of the command line arguments
	--poweron <seconds>						-			Keeps the power of the cartridges on for the number of seconds, out of reset. For cartridges that are updated over USB and only get their power from the slot (TeensyROM, Kung Fu Flash), and for checks by hand (lsusb, the LEDs of the cartridge).
	--run <command>							-			Switches the power of the cartridges on, releases the reset, runs the command (one argument, in quotes if it has spaces) and waits until it ends. If the command fails, PiCartTool exits with 1. The command runs as the user of PiCartTool, usually root. For example: --run "teensy_loader_cli --mcu=TEENSY41 -w -v TeensyROM_0.9_full.hex" or --run "dfu-util -a 0 -s 0x08000000 -D KungFuFlash_v1.55.upd" (with BOOT0 set for the DFU mode). The programmer gets its power from the USB supply of the Pi, some 100 mA more for some minutes are fine. For teensy_loader_cli it says to press the program button of the Teensy (the TeensyROM bootloader does not start with -s) and, when it fails, what to check (2.3 or newer, the USB port of the Pi).
	--dump <address> <num bytes>			-d			Dumps the flash memory from address for the number of bytes
	--dumpserial <address> <num bytes>		-ds			Dumps the serial EEPROM memory from address for the number of bytes
	--slots <num>							-s			The nunber of slots being used, default is 1, from the front slot to the rear slot
	--slotlist <slots>						-			Uses only these programmer slots, for example 1,3,5 (also 1-3-5), when cartridges in a case do not fit next to each other. All slots still get every write at the same time; erasing, the data polling of --write and the other commands for all slots wait for or read only these. The slots between them should be empty. Instead of --slots.
	--activeslot <index>					-as			The active slot (zero based) being used for single slot operations. Slot 0, the front most, is the default.



For example, to repeatedly erase and write a file to the cartridge while waiting for the button to be pressed before each erase/write pass:

	--waitbutton --erase --write scrollerbanks.bin --loop


Or for example, to read data from a 256 bank cartridge to a file:

	--read 256 readdata.bin

Or to dump 15 bytes from 0x1fa000, erase the whole block, dump the same bytes to show they are erased, then write a single byte to 0x1fa007, and then dump the bytes to show the single updated byte:

	--dump 0x1fa000 0xf --eraseblock 253 --dump 0x1fa000 0xf  --writebyte 0x1fa007 0x56  --dump 0x1fa000 0xf

## Minimal cartridge boot test

The file CartBoot.bin (assembled from C64MegaCart\CartBoot.a) is a very small minimal cartridge boot which will show a message

	--erase --write CartBoot.bin
	
## Cartridge run EEPROM tester

The file CartBootSimplePRG.bin (assembled from C64MegaCart\CartBootSimplePRG.a) will automatically run an EEPROM tester

	--erase --write CartBootSimplePRG.bin


## LEDs

D1 lights while erasing,
D2 while writing 
D3 while reading.
D3 blinks while --waitbutton waits for the button.
D5 shows that the cartridge slots have power.

If all LEDs rapidly flash on and off this means there was an error. Usually this happens during erase or write.
The tool will wait for a button to be pressed before executing the next command in the command line.


## Cartridge types

Different cartridges have different requirements for how to write the flash.
C64MegaCart is the default option for this cart tool, so these advanced options are not required, however other cartridges can be configured.
The advanced configuration command line options must be used before the cartridge is accessed.

| Type			| Command line options					| Size (megabytes)	| Typical time to erase and write	|
| ---			| ---									| ---				| ---								|
|    			|    									|    				| Raspberry Pi 5					|
| C64MegaCart	| None needed, use default config		| 2					| 43 seconds						|
| Megabyter		| --cfwcl --cfwdci 4 --crdms 1 --cfwec	| 1					| 44 seconds						|
| Gmod2			| --cfwec --cfct 1						| 0.5 + 2KB EEPROM	| 20 seconds						|
|    			|    									|    				| Raspberry Pi Zero 2W				|
| C64MegaCart	| None needed, use default config		| 2					| 99 seconds						|

Default options are: --cfwch --cfwdci 0 --crdms 0 --cfwnec

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
	--cfrev				The flash commands are sent with the bits reversed (D0 to D7 ... D7 to D0), for the DIY Gmod2 of Daniel Mantione with the reversed data bus (flasher-c64-rev.prg). The data is written as it is.
	--cfub				--write uses the unlock bypass of the flash: the unlock cycles once, then every byte needs only the A0 command and the byte. Only for flash chips that have it (AMD and compatible, the MX29F800 of the Megabyter has it).
	--cfrdd <number>	Delay in microseconds before each read of --read, --dump and the data polling of --write. The DIY Gmod2 needs 1 us or more: without it D0 often reads 1.
	--gpiofast			On by default (the option is kept for older command lines): reads the data pins D0-D7 (GPIO 0-7) with one access of the GPIO level register and writes the output byte (GPIO 8-15) with one access of the set and one of the clear register, instead of 8 calls of digitalRead() or digitalWrite(). Works on the BCM2835 to BCM2711 (Pi 1 to 4, Zero 2W) and the RP1 (Pi 5). The wait of 200 ns for the data bus before each read uses the counter of the ARM generic timer instead of clock_gettime() (about 365 ns instead of 490 ns on the Zero 2W), not on the ARMv6 of the Pi 1 and Zero. The latch pulses stay as they are. For all cartridge types. Tested on a Pi Zero 2W with Megabyter, Gmod2, EasyFlash 3 and EasyFlash 1 (reads about twice as fast). Without mapped registers PiCartTool uses digitalRead()/digitalWrite().
	--gpioslow			The data pins and the output byte with 8 calls of digitalRead() and digitalWrite() each, as before --gpiofast was on by default. Before the first access of the cartridges.
	--gpiotest <number>	Measures the GPIO accesses the given number of times, with and without --gpiofast, and checks that both ways give the same levels on the output pins, also the wait for the data bus with the ARM timer. The cartridges stay switched off, only the output byte changes and no latch takes it.


## Setup software using gcc

Video showing the whole process: https://youtu.be/GBg5k46B5Pw

Video showing the five slot programmer: https://youtu.be/I6Ru7vbInTw

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
	gcc main.cpp Hardware.cpp CartridgePort.cpp ProgrammerHardware.cpp CartridgeHardware.cpp EF3.cpp IDE64.cpp EasyFlash1.cpp EFRam.cpp -o PiCartTool -lstdc++ -lwiringPi -O3

You can then use this command to test erase and write the cartridge:

	./PiCartTool --waitbutton --erase --write scrollerbanks.bin --loop



## Setup software using Microsoft Dev Studio

Usually gcc builds will work fine, this is more for remote development and debugging.

MSDev->Tools->Options->Cross Platform->Connection Manager
Default Raspberry Pi login: user:pi password:raspberry
Configuration Manager: ARM64

	cd ~/projects/PiCartTool/bin/ARM64/Release/
	time ./PiCartTool.out --erase --write ../../../scrollerbanks.bin
	diff -q ../../../scrollerbanks.bin ../../../readdata.bin
	cmp -l ../../../scrollerbanks.bin ../../../readdata.bin | mawk 'function oct2dec(oct,     dec) {for (i = 1; i <= length(oct); i++) {dec *= 8; dec += substr(oct, i, 1)}; return dec} {printf "%08X %02X %02X\n", $1-1, oct2dec($2), oct2dec($3)}'
