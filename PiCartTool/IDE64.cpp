#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wiringPi.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <time.h>
#include <string>
#include "Hardware.h"
#include "CartridgePort.h"
#include "CartridgeHardware.h"
#include "ProgrammerHardware.h"
#include <sys/stat.h>
#include <functional>
#include <vector>
#include "EF3.h"
#include "IDE64.h"

// A real IDE64 V4.1 needs more than the 1 us of GetDataROML() until its data is stable on the bus: with 1 us about one
// read of the whole flash in 30 had wrong bytes, with 5 us none in 160
int Ide64ReadBus(int address, void (*select)(void), void (*deselect)(void))
{
	DataLatchOut::SetAddress(address);
	select();
	C64Control::UpdateLatch();
	delayMicroseconds(5);
	int value = GetInputByte();
	deselect();
	C64Control::UpdateLatch();
	return value;
}

int Ide64ReadIO1(int address)
{
	return Ide64ReadBus(0xde00 | (address & 0xff), C64Control::SetIO1, C64Control::ClearIO1);
}

int Ide64ReadROML(int address)
{
	return Ide64ReadBus(0x8000 | (address & 0x1fff), C64Control::SetLowROM, C64Control::ClearLowROM);
}



static const Ide64FlashChip kIde64FlashChips[] = {
	{ "AT29C512", 0x1f, 0x5d, 64 * 1024, true, 0 },
	{ "AT29C010A", 0x1f, 0xd5, 128 * 1024, true, 0 },
	{ "W29C011A", 0xda, 0xc1, 128 * 1024, true, 0 },
	{ "W29C020C", 0xda, 0x45, 256 * 1024, true, 0 },
	{ "SST29EE512", 0xbf, 0x5d, 64 * 1024, true, 0 },
	{ "SST29EE010", 0xbf, 0x07, 128 * 1024, true, 0 },
	{ "AM29F010", 0x01, 0x20, 128 * 1024, false, 16 * 1024 },
	{ "AM29F040", 0x01, 0xa4, 512 * 1024, false, 64 * 1024 },
	{ "M29F010B", 0x20, 0x20, 128 * 1024, false, 16 * 1024 },
	{ "SST39SF010A", 0xbf, 0xb5, 128 * 1024, false, 4 * 1024 },
	{ "SST39SF020A", 0xbf, 0xb6, 256 * 1024, false, 4 * 1024 },
	{ "SST39SF040", 0xbf, 0xb7, 512 * 1024, false, 4 * 1024 },
	{ "A29040B", 0x37, 0x86, 512 * 1024, false, 64 * 1024 },
};

static int sIde64Bank = -1;
// The exit code of PiCartTool: 1 when an IDE64 or EasyFlash 1 option failed
int sExitCode = 0;

void Ide64Cycle(int address, int value, void (*select)(void), void (*deselect)(void))
{
	DataLatchOut::SetAddress(address);
	DataLatchOut::SetData(value);
	C64Control::ClearPHI2();
	C64Control::SetDataLatchOut();
	C64Control::SetWrite();
	C64Control::UpdateLatch();
	C64Control::SetPHI2();
	if (select)
	{
		select();
	}
	C64Control::UpdateLatch();
	delayMicroseconds(1);
	C64Control::ClearPHI2();
	C64Control::UpdateLatch();
	if (deselect)
	{
		deselect();
	}
	C64Control::UpdateLatch();
	C64Control::SetRead();
	C64Control::ClearDataLatchOut();
	C64Control::SetPHI2();
	C64Control::UpdateLatch();
}

void Ide64WriteIO1(int address, int value)
{
	Ide64Cycle(0xde00 | (address & 0xff), value, C64Control::SetIO1, C64Control::ClearIO1);
}

int Ide64Status(void)
{
	return Ide64ReadIO1(0x32);
}

void Ide64SelectBank(int bank)
{
	if (bank != sIde64Bank)
	{
		Ide64WriteIO1(0x60 + bank, 0);
		sIde64Bank = bank;
	}
}

// Reads a flash address, the bank is selected as needed. The 16K configuration maps the flash to $8000-$BFFF.
int Ide64ReadFlash(int address)
{
	Ide64SelectBank(address >> 14);
	int offset = address & 0x3fff;
	return offset < 0x2000 ? Ide64ReadROML(offset)
		: Ide64ReadBus(0xa000 | (offset & 0x1fff), C64Control::SetHighROM, C64Control::ClearHighROM);
}

void Ide64WriteFlash(int address, int value)
{
	Ide64SelectBank(address >> 14);
	Ide64Cycle(0x8000 | (address & 0x3fff), value, nullptr, nullptr);
}

void Ide64FlashCommand(int command)
{
	Ide64WriteFlash(0x5555, 0xaa);
	Ide64WriteFlash(0x2aaa, 0x55);
	Ide64WriteFlash(0x5555, command);
}

// While the flash is busy, bit 6 toggles with every read. Done when three reads in a row give the same value (Perom).
bool Ide64WaitReady(int address, int timeoutMilliseconds)
{
	auto start = std::chrono::steady_clock::now();
	int last = Ide64ReadFlash(address);
	int same = 0;
	while (same < 3)
	{
		int value = Ide64ReadFlash(address);
		same = value == last ? same + 1 : 0;
		last = value;
		if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMilliseconds))
		{
			return false;
		}
	}
	return true;
}

// The IDE64 V4.x and its number of 16 KB banks (Perom: 8 when the status reads $20-$3f, 16 from $40, 32 from $80).
// Returns 0 when no IDE64 V4.x is found: $DE32 must read the same twice, and $DE60-$DEFF must show the ROM of $9E60-$9EFF.
int Ide64Detect(int& status)
{
	// $DEFB bit 0 turns the IDE64 off. The IO clear of ResetCartridge() can leave $ff there, as on a Magic Desk.
	Ide64WriteIO1(0xfb, 0);
	Ide64WriteIO1(0xfc, 0);
	sIde64Bank = -1;
	Ide64SelectBank(0);
	status = Ide64Status();
	if (status != Ide64Status() || (status & 0xe0) == 0 || (status & 0x1c) != 0)
	{
		return 0;
	}
	for (int offset = 0x60; offset < 0x100; offset++)
	{
		if (Ide64ReadIO1(offset) != Ide64ReadFlash(0x1e00 | offset))
		{
			return 0;
		}
	}
	return status >= 0x80 ? 32 : status >= 0x40 ? 16 : 8;
}

void Ide64Leave(void)
{
	Ide64SelectBank(0);
	Ide64WriteIO1(0xfd, 0);
	sIde64Bank = -1;
}

// The flash ID with $90, then with $80 $60 (Atmel). It must not be the normal data at $0000/$0001.
const Ide64FlashChip* Ide64FlashID(int& manufacturer, int& device)
{
	int normal0 = Ide64ReadFlash(0);
	int normal1 = Ide64ReadFlash(1);
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (attempt == 0)
		{
			Ide64FlashCommand(0x90);
		}
		else
		{
			Ide64FlashCommand(0x80);
			Ide64FlashCommand(0x60);
		}
		delay(10);
		manufacturer = Ide64ReadFlash(0);
		device = Ide64ReadFlash(1);
		Ide64FlashCommand(0xf0);
		delay(10);
		if (manufacturer == normal0 && device == normal1)
		{
			continue;
		}
		for (const Ide64FlashChip& chip : kIde64FlashChips)
		{
			if (chip.manufacturer == manufacturer && chip.device == device)
			{
				return &chip;
			}
		}
	}
	manufacturer = device = -1;
	return nullptr;
}

void Ide64WaitForButton(const char* text)
{
	printf("%s\n", text);
	fflush(stdout);
	while (digitalRead(24) == HIGH)
	{
		delay(100);
		AlternateLED2();
	}
	while (digitalRead(24) == LOW)
	{
		delay(100);
		AlternateLED2();
	}
	InterfaceControl::ClearLED2();
	InterfaceControl::UpdateLatch();
}

// Every write goes to all programmer slots: the others must be empty, or read the same as the active slot (a programmer
// with one slot reads the same cartridge in every slot).
bool Ide64CheckOtherSlots(void)
{
	const int kMaxProgrammerSlots = 5;
	unsigned char active[16];
	EF3SelectProgrammerSlot(theActiveSlot);
	for (int i = 0; i < 16; i++)
	{
		active[i] = (unsigned char)GetDataROML(0x8000 | i);
	}
	bool ok = true;
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		if (programmerSlot == theActiveSlot)
		{
			continue;
		}
		SelectProgrammerSlotForLines(programmerSlot);
		bool empty = digitalRead(26) == 1 && digitalRead(25) == 1;
		bool same = true;
		for (int i = 0; i < 16; i++)
		{
			int value = GetDataROML(0x8000 | i);
			empty = empty && value == 0xff;
			same = same && value == active[i];
		}
		if (!empty && !same)
		{
			printf("Programmer slot %d is not empty, it would get the same writes. Take the cartridge out\n", programmerSlot + 1);
			ok = false;
		}
	}
	EF3SelectProgrammerSlot(theActiveSlot);
	return ok;
}

// The IDE64 does not drive the data bus while BA is low. The programmer does not drive BA, without a pull-up (1K from BA
// to +5V, J3 pins 15 and 6 on the five slot programmer) a real IDE64 V4.1 now and then gives $ff or single bits high
// for a while ($DE32 reads $7e), so the detection is tried several times
bool Ide64Prepare(int& banks, bool quiet)
{
	const int kTries = 5;
	int status = 0;
	banks = 0;
	for (int tries = 0; tries < kTries && !banks; tries++)
	{
		banks = Ide64Detect(status);
	}
	if (!banks)
	{
		printf("No IDE64 V4.x in programmer slot %d ($DE32 reads $%02x, tried %d times)\n", theActiveSlot + 1, status, kTries);
		Ide64Leave();
		return false;
	}
	if (!quiet)
	{
		printf("IDE64 V4.x in programmer slot %d, $DE32 reads $%02x, %d banks of 16 KB\n", theActiveSlot + 1, status, banks);
	}
	return true;
}

// While the PGM jumper is set the IDE64 answers in $DE32, but the flash is not seen (ROML reads $ff)
bool Ide64FlashHidden(int& status)
{
	Ide64WriteIO1(0xfb, 0);
	Ide64WriteIO1(0xfc, 0);
	sIde64Bank = -1;
	Ide64SelectBank(0);
	status = Ide64Status();
	if ((status & 0xe0) == 0 || (status & 0x1c) != 0)
	{
		return false;
	}
	for (int offset = 0x60; offset < 0x100; offset++)
	{
		if (Ide64ReadFlash(0x1e00 | offset) != 0xff)
		{
			return false;
		}
	}
	return true;
}

// The flash ID can only be read when the PGM jumper was set at the reset (at the start of this option) and is removed
// again, so with the jumper set it waits for the button
// The DS1302 real time clock keeps the time and the IDEDOS setup in 31 bytes of RAM, kept by the battery (MAME ide64.cpp,
// VICE ide64.c, IDEDOS ds1302.asm). $DEFB bit 1 is its CE. Every access to $DE5F clocks one bit (bit 0) with SCLK low
// and high again, the least significant bit first: a write gives the next command bit, a read the next data bit.
void Ide64ClockCommand(int command)
{
	Ide64WriteIO1(0xfb, 0x02);
	for (int bit = 0; bit < 8; bit++)
	{
		Ide64WriteIO1(0x5f, (command >> bit) & 1);
	}
}

void Ide64ClockRead(int command, unsigned char* data, int size)
{
	Ide64ClockCommand(command);
	for (int i = 0; i < size; i++)
	{
		int value = 0;
		for (int bit = 0; bit < 8; bit++)
		{
			value |= (Ide64ReadIO1(0x5f) & 1) << bit;
		}
		data[i] = (unsigned char)value;
	}
	Ide64WriteIO1(0xfb, 0);
}

void Ide64ClockWrite(int command, const unsigned char* data, int size)
{
	Ide64ClockCommand(command);
	for (int i = 0; i < size; i++)
	{
		for (int bit = 0; bit < 8; bit++)
		{
			Ide64WriteIO1(0x5f, (data[i] >> bit) & 1);
		}
	}
	Ide64WriteIO1(0xfb, 0);
}

// $BF reads the clock registers in a burst: seconds, minutes, hours, date, month, day, year (BCD), write protect
// The day of the week counts from 1 = Sunday, as in IDEDOS (its default 2000-01-01 is day 7)
static const char* const kIde64Days[] = { "?", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

std::string Ide64ClockText(const unsigned char* clock)
{
	int hours = clock[2] & 0x3f;
	const char* ampm = "";
	if (clock[2] & 0x80)
	{
		hours = clock[2] & 0x1f;
		ampm = (clock[2] & 0x20) ? " PM" : " AM";
	}
	char text[64];
	snprintf(text, sizeof(text), "%s 20%02x-%02x-%02x %02x:%02x:%02x%s%s", kIde64Days[clock[5] <= 7 ? clock[5] : 0], clock[6], clock[4],
		clock[3], hours, clock[1], clock[0] & 0x7f, ampm, (clock[0] & 0x80) ? " (halted)" : "");
	return text;
}

void Ide64ShowClock(void)
{
	unsigned char clock[8];
	Ide64ClockRead(0xbf, clock, sizeof(clock));
	printf("Clock: %s\n", Ide64ClockText(clock).c_str());
}

int Ide64Bcd(int value)
{
	return (value / 10) << 4 | (value % 10);
}

int Ide64FromBcd(int value)
{
	return (value >> 4) * 10 + (value & 15);
}

// Sets the clock to "now" (the local time of the Pi) or "YYYY-MM-DD HH:MM:SS": write protect off ($8E $00), then the
// 7 clock registers and the write protect register in a burst ($BE, 24 hour mode, the clock runs), which turns the
// write protect on again. The clock is read back. The setup in the RAM is not changed.
bool Ide64SetClock(const char* when)
{
	bool now = strcasecmp(when, "now") == 0;
	struct tm target = {};
	if (!now)
	{
		if (sscanf(when, "%d-%d-%d %d:%d:%d", &target.tm_year, &target.tm_mon, &target.tm_mday, &target.tm_hour, &target.tm_min, &target.tm_sec) != 6)
		{
			printf("%s is not \"now\" or a time as \"YYYY-MM-DD HH:MM:SS\". Nothing was written\n", when);
			return false;
		}
		target.tm_year -= 1900;
		target.tm_mon -= 1;
		target.tm_isdst = -1;
		struct tm check = target;
		if (mktime(&check) == -1 || check.tm_mday != target.tm_mday || check.tm_hour != target.tm_hour || check.tm_min != target.tm_min)
		{
			printf("%s is not a valid time. Nothing was written\n", when);
			return false;
		}
	}
	int banks;
	if (!Ide64Prepare(banks))
	{
		return false;
	}
	bool same = false;
	unsigned char clock[8], readBack[8];
	for (int attempt = 0; attempt < 3 && !same; attempt++)
	{
		time_t written;
		if (now)
		{
			written = time(nullptr);
			localtime_r(&written, &target);
		}
		else
		{
			written = mktime(&target);
		}
		if (target.tm_year < 100 || target.tm_year > 199)
		{
			Ide64Leave();
			printf("The DS1302 only keeps the years 2000 to 2099. Nothing was written\n");
			return false;
		}
		clock[0] = (unsigned char)Ide64Bcd(target.tm_sec);
		clock[1] = (unsigned char)Ide64Bcd(target.tm_min);
		clock[2] = (unsigned char)Ide64Bcd(target.tm_hour);
		clock[3] = (unsigned char)Ide64Bcd(target.tm_mday);
		clock[4] = (unsigned char)Ide64Bcd(target.tm_mon + 1);
		clock[5] = (unsigned char)(target.tm_wday + 1);
		clock[6] = (unsigned char)Ide64Bcd(target.tm_year - 100);
		clock[7] = 0x80;
		const unsigned char kProtectOff = 0x00;
		auto start = std::chrono::steady_clock::now();
		Ide64ClockWrite(0x8e, &kProtectOff, 1);
		Ide64ClockWrite(0xbe, clock, sizeof(clock));
		Ide64ClockRead(0xbf, readBack, sizeof(readBack));
		// The clock runs on: compare the read time with the written one plus the time since writing
		long elapsed = (long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
		struct tm got = {};
		got.tm_sec = Ide64FromBcd(readBack[0] & 0x7f);
		got.tm_min = Ide64FromBcd(readBack[1]);
		got.tm_hour = Ide64FromBcd(readBack[2] & 0x3f);
		got.tm_mday = Ide64FromBcd(readBack[3]);
		got.tm_mon = Ide64FromBcd(readBack[4]) - 1;
		got.tm_year = Ide64FromBcd(readBack[6]) + 100;
		got.tm_isdst = target.tm_isdst;
		long difference = (long)difftime(mktime(&got), written) - elapsed;
		same = !(readBack[0] & 0x80) && !(readBack[2] & 0x80) && readBack[5] == clock[5] && (readBack[7] & 0x80) && difference >= -1 && difference <= 2;
	}
	Ide64Leave();
	if (!same)
	{
		printf("Setting the IDE64 clock to %s failed, it reads %s\n", Ide64ClockText(clock).c_str(), Ide64ClockText(readBack).c_str());
		return false;
	}
	printf("Set the IDE64 clock to %s, it reads %s\n", Ide64ClockText(clock).c_str(), Ide64ClockText(readBack).c_str());
	return true;
}

// $FF reads the 31 bytes of RAM in a burst. IDEDOS keeps its setup there: 30 bytes and a check byte. All 31 bytes XORed
// give "i" ($49) for IDEDOS 0.90 and "j" ($4A) for 0.91, which does not take a setup of 0.90 and uses its default then.
// Returns the IDEDOS version of the setup, nullptr when the check byte is wrong.
const char* Ide64SetupVersion(const unsigned char* data)
{
	int check = 0;
	for (int i = 0; i < 31; i++)
	{
		check ^= data[i];
	}
	return check == 0x49 ? "0.90" : check == 0x4a ? "0.91" : nullptr;
}

// The IDEDOS version in the flash ("IDEDOS V0.91 BETA ..." in the first 2 KB of bank 0), an empty string if not found.
// Read until two reads in a row are the same, see Ide64Prepare().
std::string Ide64FlashVersion(void)
{
	std::string text, previous;
	for (int reads = 0; reads < 10 && (reads < 2 || text != previous); reads++)
	{
		previous.swap(text);
		text.clear();
		for (int address = 0; address < 0x800; address++)
		{
			text += (char)Ide64ReadFlash(address);
		}
	}
	size_t at = text.find("IDEDOS V");
	return at == std::string::npos || at + 12 > text.size() ? "" : text.substr(at + 8, 4);
}

// Reads the RAM until two reads in a row are the same (see Ide64Prepare()), returns the number of reads or 0
int Ide64ReadSetupBytes(unsigned char* data)
{
	const int kMaxReads = 10;
	unsigned char previous[31];
	memset(data, 0, 31);
	int reads = 0;
	do
	{
		memcpy(previous, data, sizeof(previous));
		Ide64ClockRead(0xff, data, 31);
		reads++;
	} while (reads < kMaxReads && (reads < 2 || memcmp(data, previous, sizeof(previous)) != 0));
	return memcmp(data, previous, sizeof(previous)) == 0 ? reads : 0;
}

void Ide64ShowSetup(const unsigned char* data)
{
	for (int i = 0; i < 31; i++)
	{
		printf("%02x%s", data[i], (i % 16 == 15 || i == 30) ? "\n" : " ");
	}
	const char* version = Ide64SetupVersion(data);
	if (version)
	{
		printf("IDEDOS %s setup (the check byte matches)\n", version);
	}
	else
	{
		printf("No IDEDOS setup check byte found: IDEDOS would use its default setup\n");
	}
}

// Reads a file of --readide64setup (31 bytes), failText ends the message when it is not
bool Ide64LoadSetupFile(const char* filename, unsigned char* data, const char* failText)
{
	FILE* fp = fopen(filename, "rb");
	if (!fp)
	{
		printf("Cannot open %s\n", filename);
		return false;
	}
	size_t got = fread(data, 1, 31, fp);
	bool longer = fgetc(fp) != EOF;
	fclose(fp);
	if (got != 31 || longer)
	{
		printf("%s is not 31 bytes long (a file of --readide64setup)%s\n", filename, failText);
		return false;
	}
	return true;
}

const char* Ide64ColourName(int colour)
{
	static const char* const kNames[16] = { "Black", "White", "Red", "Cyan", "Purple", "Green", "Blue", "Yellow",
		"Orange", "Brown", "L.red", "D.gray", "Med.gray", "L.green", "L.blue", "L.gray" };
	return kNames[colour & 15];
}

// A drive number of bytes 26-30: 0 is none, otherwise the value + 7
std::string Ide64DriveName(int value)
{
	return value ? std::to_string(value + 7) : "--";
}

// The setup by name, as the setup screens of IDEDOS show it. The layout is the same in 0.90 and 0.91 except for
// ShortBus I/O 2 (byte 4 bit 2, only 0.91) and the drive numbers in bytes 26-30 (see patches/picarttool-ide64.md).
void Ide64ShowSetupNames(const unsigned char* data, const char* version)
{
	bool v091 = strcmp(version, "0.91") == 0;
	static const char* const kStartBoot[4] = { "Disabled", "Enabled", "Power on", "Always" };
	struct Option { const char* name; int byte; int bit; };
	static const Option kOptions[] = {
		{ "Floppy speeder", 3, 5 },{ "Set basic clock", 3, 4 },{ "Keyboard repeat", 3, 3 },{ "Lowercase chars", 3, 2 },
		{ "Use DOS wedge", 3, 1 },{ "C128 keyboard", 3, 0 },{ "CMD emulation", 4, 7 },{ "Function keys", 4, 6 },
		{ "Control stops", 4, 5 },{ "Blocks free hack", 4, 4 },{ "Fast detection", 4, 3 },{ "ShortBus I/O 2", 4, 2 } };
	printf("Standard setup\n");
	printf("  %-18s: %s\n", "Start boot file", kStartBoot[data[2] >> 6]);
	for (const Option& option : kOptions)
	{
		if (option.byte == 4 && option.bit == 2 && !v091)
		{
			continue;
		}
		printf("  %-18s: %s\n", option.name, (data[option.byte - 1] >> option.bit) & 1 ? "Enabled" : "Disabled");
	}

	// Manager colours: the low nibbles of bytes 15-21, then the high nibbles of bytes 15-20
	static const char* const kManager[13] = { "Paper", "Border", "Panel", "Act.panel", "Dir", "File", "Sel.dir",
		"Sel.file", "Filebar", "Misc", "Window", "Menubar", "Input" };
	printf("Manager colors\n");
	for (int i = 0; i < 13; i++)
	{
		int value = data[14 + i % 7];
		printf("  %-18s: %s\n", kManager[i], Ide64ColourName(i < 7 ? value : value >> 4));
	}
	printf("System colors\n");
	printf("  %-18s: %s\n", "Paper", Ide64ColourName(data[21]));
	printf("  %-18s: %s\n", "Border", Ide64ColourName(data[21] >> 4));
	printf("  %-18s: %s\n", "Char", Ide64ColourName(data[1]));
	printf("Monitor colors\n");
	printf("  %-18s: %s\n", "Paper", Ide64ColourName(data[22]));
	printf("  %-18s: %s\n", "Char", Ide64ColourName(data[22] >> 4));

	printf("Device numbers\n");
	printf("  %-18s: %d\n", "System drive", data[0]);
	for (int i = 0; i < 4; i++)
	{
		printf("  Serial drive %-5d: %s\n", 8 + i, Ide64DriveName(data[26 + i] >> 4).c_str());
	}
	if (v091)
	{
		for (int i = 0; i < 5; i++)
		{
			printf("  IDEDOS drive %-5d: %s\n", 1 + i, Ide64DriveName(data[25 + i] & 15).c_str());
		}
	}
	else
	{
		for (int i = 0; i < 4; i++)
		{
			printf("  IDE %-14d: %s\n", i, Ide64DriveName(data[25 + i] & 15).c_str());
		}
		printf("  %-18s: %s\n", "PCLink", Ide64DriveName(data[25] >> 4).c_str());
	}

	// ATA devices: primary master, primary slave, secondary master, secondary slave. A nibble each in bytes 5-6 (bit 3
	// retry on error, bit 2 write cache, bit 1 read look-ahead, bit 0 slow down CDROM) and 24-25 (power management).
	static const char* const kDevice[4] = { "Primary master", "Primary slave", "Second. master", "Second. slave" };
	static const char* const kPower[16] = { "Default", "Disabled", "1 Min", "2 Min", "3 Min", "4 Min", "5 Min", "6 Min",
		"7 Min", "8 Min", "10 Min", "15 Min", "20 Min", "30 Min", "1 Hour", "2 Hour" };
	printf("ATA devices         %-16s%-16s%-16s%s\n", kDevice[0], kDevice[1], kDevice[2], kDevice[3]);
	auto row = [&](const char* name, std::function<std::string(int)> text)
		{
			printf("  %-18s", name);
			for (int device = 0; device < 4; device++)
			{
				printf(device < 3 ? "%-16s" : "%s\n", text(device).c_str());
			}
		};
	auto nibble = [&](int byte, int device) { return (data[byte - 1 + device / 2] >> (device % 2 * 4)) & 15; };
	auto flag = [&](int bit) { return [&, bit](int device) { return std::string((nibble(5, device) >> bit) & 1 ? "Enabled" : "Disabled"); }; };
	row("Power management", [&](int device) { return std::string(kPower[nibble(24, device)]); });
	row("Retry on error", flag(3));
	row("Write cache", flag(2));
	row("Read look-ahead", flag(1));
	row("Slow down CDROM", flag(0));
	row("Linear read max", [&](int device) { return std::to_string(data[6 + device] + 1); });
	row("Linear write max", [&](int device) { return std::to_string(data[10 + device] + 1); });
}

// --showide64setup: the setup by name, from a file of --readide64setup or, without a file name, read from the IDE64
bool Ide64ShowSetupByName(const char* filename)
{
	unsigned char data[31];
	if (filename)
	{
		if (!Ide64LoadSetupFile(filename, data, ""))
		{
			return false;
		}
		printf("IDE64 setup from %s:\n", filename);
	}
	else
	{
		int banks;
		if (!Ide64Prepare(banks))
		{
			return false;
		}
		int reads = Ide64ReadSetupBytes(data);
		Ide64Leave();
		if (!reads)
		{
			printf("Reading the IDE64 setup gave other data each time\n");
			return false;
		}
	}
	Ide64ShowSetup(data);
	const char* version = Ide64SetupVersion(data);
	if (!version)
	{
		return false;
	}
	Ide64ShowSetupNames(data, version);
	return true;
}

bool Ide64ReadSetup(const char* filename)
{
	int banks;
	if (!Ide64Prepare(banks))
	{
		return false;
	}
	unsigned char data[31];
	int reads = Ide64ReadSetupBytes(data);
	Ide64Leave();
	if (!reads)
	{
		printf("Reading the IDE64 setup gave other data each time, nothing was written to %s\n", filename);
		return false;
	}
	Ide64ShowSetup(data);
	if (!WriteFile(filename, data, (int)sizeof(data)))
	{
		return false;
	}
	printf("Read the 31 bytes of the IDE64 setup (DS1302 RAM) to %s (the same twice in a row, %d reads)\n", filename, reads);
	return true;
}

// Writes a setup file of --readide64setup back into the RAM of the DS1302, as IDEDOS saves its setup: write protect off
// ($8E $00), the 31 bytes in a burst ($FE), write protect on ($8E $80). The file must have a valid check byte of the
// IDEDOS version in the flash. The clock is not changed.
bool Ide64WriteSetup(const char* filename)
{
	unsigned char file[31];
	if (!Ide64LoadSetupFile(filename, file, ". Nothing was written"))
	{
		return false;
	}
	const char* version = Ide64SetupVersion(file);
	if (!version)
	{
		printf("%s has no IDEDOS setup check byte. Nothing was written\n", filename);
		return false;
	}
	int banks;
	if (!Ide64Prepare(banks))
	{
		return false;
	}
	std::string flashVersion = Ide64FlashVersion();
	if (flashVersion != version)
	{
		Ide64Leave();
		printf("%s is a setup of IDEDOS %s, the flash holds %s: IDEDOS would not take it. Nothing was written\n", filename, version,
			flashVersion.empty() ? "no known IDEDOS" : ("IDEDOS " + flashVersion).c_str());
		return false;
	}
	unsigned char data[31];
	if (Ide64ReadSetupBytes(data) && memcmp(data, file, sizeof(file)) == 0)
	{
		Ide64Leave();
		printf("The IDE64 setup is already the same as %s, nothing was written\n", filename);
		return true;
	}
	const unsigned char kProtectOff = 0x00, kProtectOn = 0x80;
	bool same = false;
	for (int attempt = 0; attempt < 3 && !same; attempt++)
	{
		Ide64ClockWrite(0x8e, &kProtectOff, 1);
		Ide64ClockWrite(0xfe, file, sizeof(file));
		Ide64ClockWrite(0x8e, &kProtectOn, 1);
		same = Ide64ReadSetupBytes(data) && memcmp(data, file, sizeof(file)) == 0;
	}
	Ide64Leave();
	if (!same)
	{
		printf("Writing the IDE64 setup failed, it reads:\n");
		Ide64ShowSetup(data);
		return false;
	}
	printf("Wrote the 31 bytes of %s to the IDE64 setup (DS1302 RAM), read back the same\n", filename);
	return true;
}

bool Ide64ID(void)
{
	int status;
	if (Ide64FlashHidden(status))
	{
		printf("$DE32 reads $%02x, the flash is not seen: the PGM jumper is set\n", status);
		Ide64WaitForButton("Remove the PGM jumper now, then press the button");
	}
	int banks;
	if (!Ide64Prepare(banks))
	{
		return false;
	}
	Ide64ShowClock();
	int manufacturer, device;
	const Ide64FlashChip* chip = Ide64FlashID(manufacturer, device);
	if (chip)
	{
		printf("Flash ID $%02x $%02x: %s, %d KB, %s\n", manufacturer, device, chip->name, chip->size / 1024, chip->pageWrite ? "128 byte pages" : "byte program, sector erase");
	}
	else
	{
		printf("No flash ID: the flash is write protected. Set the PGM jumper, start PiCartTool (it resets the IDE64), remove the jumper when asked\n");
	}
	Ide64Leave();
	return true;
}

// The ATA registers of an IDE64 V4.x (MAME ide64.cpp, VICE ide64.c): a write to $DE28 selects the primary, to $DE29 the
// secondary interface. $DE20-$DE27 are the command block (data, error, sector count, LBA 0-7, 8-15, 16-23, device, status
// or command), $DE2E the alternate status. A read of $DE20 gives the low byte of the 16 bit data and keeps the high byte
// for $DE31. Without a device the registers read $7e on the real IDE64 V4.1 in the programmer.
static const char* const kIde64AtaDevices[4] = { "Primary master", "Primary slave", "Second. master", "Second. slave" };

// A register read until two reads in a row are the same, see Ide64Prepare(). Not for the data register.
int Ide64AtaRegister(int reg)
{
	int value = Ide64ReadIO1(0x20 + reg), previous;
	int reads = 1;
	do
	{
		previous = value;
		value = Ide64ReadIO1(0x20 + reg);
	} while (value != previous && ++reads < 10);
	return value;
}

// Waits until the device is not busy (status bit 7), returns the status. $ff is an interface without devices.
int Ide64AtaWait(int timeoutMilliseconds)
{
	auto start = std::chrono::steady_clock::now();
	int status;
	while (((status = Ide64AtaRegister(7)) & 0x80) && status != 0xff &&
		std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeoutMilliseconds))
	{
		delayMicroseconds(100);
	}
	return status;
}

void Ide64AtaSelect(int device)
{
	Ide64WriteIO1(0x28 + device / 2, 0);
	Ide64WriteIO1(0x26, device % 2 ? 0xb0 : 0xa0);
}

// IDENTIFY DEVICE ($EC) or IDENTIFY PACKET DEVICE ($A1): false when the device aborts it or has no data (status DRQ)
bool Ide64AtaIdentify(int device, int command, unsigned short* words)
{
	Ide64AtaSelect(device);
	Ide64WriteIO1(0x27, command);
	delay(1);
	if ((Ide64AtaWait(5000) & 0x89) != 0x08)
	{
		return false;
	}
	for (int i = 0; i < 256; i++)
	{
		int low = Ide64ReadIO1(0x20);
		words[i] = (unsigned short)(Ide64ReadIO1(0x31) << 8 | low);
	}
	return true;
}

// A text of the IDENTIFY data: two characters per word, the first in the high byte
std::string Ide64AtaText(const unsigned short* words, int first, int count)
{
	std::string text;
	for (int i = first; i < first + count; i++)
	{
		text += (char)(words[i] >> 8);
		text += (char)(words[i] & 0xff);
	}
	for (char& c : text)
	{
		if (c < 32 || c > 126)
		{
			c = '?';
		}
	}
	size_t begin = text.find_first_not_of(' ');
	return begin == std::string::npos ? "" : text.substr(begin, text.find_last_not_of(' ') - begin + 1);
}

// Names the device of the IDENTIFY data: type, model, serial number, firmware, size
std::string Ide64AtaName(const unsigned short* words, bool packet)
{
	static const char* const kPacketTypes[32] = { "Direct access ATAPI device", "Tape", "Printer", "Processor", "Write once device",
		"CD-ROM", "Scanner", "Optical disk", "Medium changer", "Communications device" };
	std::string type;
	if (packet)
	{
		const char* name = kPacketTypes[(words[0] >> 8) & 31];
		type = name ? name : "ATAPI device";
	}
	else
	{
		type = words[0] == 0x848a ? "CompactFlash" : (words[0] & 0x80) ? "Removable ATA device" : "ATA disk";
	}
	std::string text = type + " \"" + Ide64AtaText(words, 27, 20) + "\", serial \"" + Ide64AtaText(words, 10, 10) +
		"\", firmware \"" + Ide64AtaText(words, 23, 4) + "\"";
	if (!packet)
	{
		unsigned long long sectors = words[60] | (unsigned long long) words[61] << 16;
		if ((words[83] & 0x400) && (words[100] | words[101] | words[102] | words[103]))
		{
			sectors = words[100] | (unsigned long long) words[101] << 16 | (unsigned long long) words[102] << 32 |
				(unsigned long long) words[103] << 48;
		}
		if (!sectors)
		{
			sectors = (unsigned long long) words[1] * words[3] * words[6];
		}
		text += ", " + std::to_string(sectors) + " sectors (" + std::to_string(sectors / 2048) + " MB)";
	}
	return text;
}

// One look at the ATA devices and the clock port, a line for each. A device is there when it keeps values in the
// sector count and LBA 0-7 registers. The clock port at $DE00-$DE0F: a 16C550 UART (Silversurfer and compatible RS232
// cards) keeps values in its scratch register ($DE07, saved and written back) and shows an idle transmitter in its line
// status ($DE05).
std::vector<std::string> Ide64ScanDevices(bool& ok)
{
	std::vector<std::string> lines;
	char line[256];
	ok = true;
	for (int device = 0; device < 4; device++)
	{
		Ide64AtaSelect(device);
		// A device that is starting reads busy, it does not take writes to the registers then
		int status = Ide64AtaWait(10000);
		bool present = !(status & 0x80);
		for (int value : { 0x55, 0xaa })
		{
			Ide64WriteIO1(0x22, value);
			Ide64WriteIO1(0x23, value ^ 0xff);
			present = present && Ide64AtaRegister(2) == value && Ide64AtaRegister(3) == (value ^ 0xff);
		}
		unsigned short words[256];
		bool packet = false;
		if (!present)
		{
			bool busy = (status & 0x80) && status != 0xff;
			snprintf(line, sizeof(line), busy ? "%-16s: busy for 10 seconds (status $%02x)" : "%-16s: -", kIde64AtaDevices[device], status);
			ok = ok && !busy;
		}
		else if (Ide64AtaIdentify(device, 0xec, words) ||
			// An ATAPI device aborts IDENTIFY DEVICE and shows its signature $14 $EB in LBA 8-15 and 16-23
			(Ide64AtaRegister(4) == 0x14 && Ide64AtaRegister(5) == 0xeb && (packet = Ide64AtaIdentify(device, 0xa1, words))))
		{
			snprintf(line, sizeof(line), "%-16s: %s", kIde64AtaDevices[device], Ide64AtaName(words, packet).c_str());
		}
		else
		{
			snprintf(line, sizeof(line), "%-16s: a device that does not answer IDENTIFY (status $%02x, error $%02x)", kIde64AtaDevices[device],
				Ide64AtaRegister(7), Ide64AtaRegister(1));
			ok = false;
		}
		lines.push_back(line);
	}
	int saved = Ide64ReadIO1(0x07);
	bool uart = (Ide64ReadIO1(0x05) & 0x60) == 0x60;
	for (int value : { 0x55, 0xaa, 0x00, 0xff })
	{
		Ide64WriteIO1(0x07, value);
		uart = uart && Ide64ReadIO1(0x07) == value;
	}
	Ide64WriteIO1(0x07, saved);
	snprintf(line, sizeof(line), "%-16s: %s", "Clock port", uart ? "16C550 UART (Silversurfer or a compatible RS232 card)" : "no 16C550 UART found");
	lines.push_back(line);
	return lines;
}

// --ide64devices: the ATA devices (IDENTIFY, which reads nothing from the medium) and the clock port, looked at until
// two looks in a row give the same (see Ide64Prepare()). A look without devices is short enough to fall into one time
// the IDE64 does not drive the bus, so $DE32 must read a status (bits 7-5 %001) before and after a look.
bool Ide64Devices(void)
{
	int banks;
	if (!Ide64Prepare(banks))
	{
		return false;
	}
	std::vector<std::string> lines, previous;
	bool ok = false;
	int looks = 0;
	do
	{
		previous.swap(lines);
		bool driven = (Ide64Status() & 0xe0) == 0x20;
		lines = Ide64ScanDevices(ok);
		if (!driven || (Ide64Status() & 0xe0) != 0x20)
		{
			lines.clear();
		}
		looks++;
	} while (looks < 20 && (lines.empty() || lines != previous));
	Ide64Leave();
	if (lines.empty())
	{
		printf("The IDE64 did not drive the data bus during %d looks at its devices\n", looks);
		return false;
	}
	if (lines != previous)
	{
		printf("The IDE64 devices looked different each time (%d looks), the last one:\n", looks);
		ok = false;
	}
	for (const std::string& text : lines)
	{
		printf("%s\n", text.c_str());
	}
	return ok;
}

// The whole flash is read until two reads in a row are the same, see Ide64Prepare(). False when it never was.
bool Ide64ReadAll(int banks, std::vector<unsigned char>& data, int& reads)
{
	const int kMaxReads = 10;
	std::vector<unsigned char> previous;
	data.assign(banks * 0x4000, 0);
	reads = 0;
	while (reads < kMaxReads && (reads < 2 || data != previous))
	{
		previous.swap(data);
		data.resize(banks * 0x4000);
		for (size_t address = 0; address < data.size(); address++)
		{
			data[address] = (unsigned char)Ide64ReadFlash((int)address);
		}
		reads++;
	}
	return data == previous;
}

bool Ide64Read(const char* filename)
{
	int banks;
	if (!Ide64Prepare(banks))
	{
		return false;
	}
	std::vector<unsigned char> data;
	int reads;
	bool stable = Ide64ReadAll(banks, data, reads);
	Ide64Leave();
	if (!stable)
	{
		printf("Reading the IDE64 flash gave other data each time (%d reads), nothing was written to %s\n", reads, filename);
		return false;
	}
	if (!WriteFile(filename, data.data(), (int)data.size()))
	{
		return false;
	}
	printf("Read %d KB of the IDE64 flash to %s (the same twice in a row, %d reads)\n", (int)data.size() / 1024, filename, reads);
	return true;
}

// Writes an image from flash address 0. Pages or sectors that are already the same are left out. Every page or sector
// is checked right after writing, and the whole image at the end. With prepare, the image is made after the IDE64 can
// be read (--swapide64), it returns false when nothing is to be written. With upperFirst the second half of the image
// is written before the first.
bool Ide64WriteImage(std::vector<unsigned char> file, const char* filename, bool waitForJumper, const std::function<bool(int, std::vector<unsigned char>&)>& prepare, bool upperFirst)
{
	if (!Ide64CheckOtherSlots())
	{
		return false;
	}
	if (waitForJumper)
	{
		Ide64WaitForButton("The IDE64 was reset with the PGM jumper set. Remove the PGM jumper now, then press the button");
	}
	int banks;
	if (!Ide64Prepare(banks))
	{
		return false;
	}
	if (prepare && !prepare(banks, file))
	{
		Ide64Leave();
		return false;
	}
	int manufacturer, device;
	const Ide64FlashChip* chip = Ide64FlashID(manufacturer, device);
	if (!chip)
	{
		printf("No flash ID: the flash is write protected. Set the PGM jumper before PiCartTool resets the IDE64 and remove it when asked. Nothing was written\n");
		Ide64Leave();
		return false;
	}
	int flashSize = std::min(chip->size, banks * 0x4000);
	if ((int)file.size() > flashSize)
	{
		printf("%s has %d KB, the flash of this IDE64 only %d KB. Nothing was written\n", filename, (int)file.size() / 1024, flashSize / 1024);
		Ide64Leave();
		return false;
	}
	printf("Writing %d KB to the %s\n", (int)file.size() / 1024, chip->name);
	// The same way as the Perom programmer 2.0a:
	// - A page (AT29C, W29C, SST29EE) is compared 6 times with 3 reads of every byte and left out when it is the same.
	//   Otherwise it is written as a whole, about 2 ms later polled until it reads stable and compared again, up to
	//   10 times.
	// - A sector (the byte program chips) is compared with one read of every byte and left out when it is the same. It
	//   is erased only when a bit must go from 0 to 1, then about 40 ms later polled. Every byte that differs is
	//   programmed and polled, a second time when its missing bits can still be programmed. Then the sector is compared
	//   3 times. Up to 3 attempts.
	int blockSize = chip->pageWrite ? 128 : chip->sectorSize;
	int written = 0, skipped = 0;
	auto matches3 = [&](int address)
		{
			int value = file[address];
			return Ide64ReadFlash(address) == value && Ide64ReadFlash(address) == value && Ide64ReadFlash(address) == value;
		};
	int numBlocks = ((int)file.size() + blockSize - 1) / blockSize;
	for (int n = 0; n < numBlocks; n++)
	{
		int index = upperFirst ? (n + numBlocks / 2) % numBlocks : n;
		int block = index * blockSize;
		int size = std::min(blockSize, (int)file.size() - block);
		bool ok = false;
		if (chip->pageWrite)
		{
			auto pageSame = [&](void)
				{
					for (int pass = 0; pass < 6; pass++)
					{
						for (int i = 0; i < size; i++)
						{
							if (!matches3(block + i))
							{
								return false;
							}
						}
					}
					return true;
				};
			if (pageSame())
			{
				skipped++;
				continue;
			}
			for (int attempt = 0; attempt < 10 && !ok; attempt++)
			{
				// The command and the whole page, at most 150 us from byte to byte, then the flash writes it
				Ide64FlashCommand(0xa0);
				for (int i = 0; i < size; i++)
				{
					Ide64WriteFlash(block + i, file[block + i]);
				}
				delayMicroseconds(2000);
				Ide64WaitReady(block + size - 1, 100);
				ok = pageSame();
			}
		}
		else
		{
			// Whether the sector is the same, and whether a bit must go from 0 to 1 (erase)
			auto sectorSame = [&](bool& erase)
				{
					bool same = true;
					erase = false;
					for (int i = 0; i < size; i++)
					{
						int flash = Ide64ReadFlash(block + i);
						if (flash != file[block + i])
						{
							same = false;
							erase = erase || ((flash ^ file[block + i]) & file[block + i]) != 0;
						}
					}
					return same;
				};
			bool erase;
			if (sectorSame(erase))
			{
				skipped++;
				continue;
			}
			for (int attempt = 0; attempt < 3 && !ok; attempt++)
			{
				if (erase)
				{
					Ide64FlashCommand(0x80);
					Ide64WriteFlash(0x5555, 0xaa);
					Ide64WriteFlash(0x2aaa, 0x55);
					Ide64WriteFlash(block, 0x30);
					delay(40);
					Ide64WaitReady(block, 2000);
				}
				for (int i = 0; i < size; i++)
				{
					int value = file[block + i];
					for (int program = 0; program < 2 && Ide64ReadFlash(block + i) != value; program++)
					{
						Ide64FlashCommand(0xa0);
						Ide64WriteFlash(block + i, value);
						Ide64WaitReady(block + i, 10);
						if ((Ide64ReadFlash(block + i) & value) != value)
						{
							break;	// a bit that must be 1 is 0, only an erase helps
						}
					}
				}
				ok = sectorSame(erase) && sectorSame(erase) && sectorSame(erase);
			}
		}
		if (!ok)
		{
			printf("Writing the flash at $%05x failed. The IDE64 can be written again (with Perom on a C64 as well)\n", block);
			Ide64Leave();
			return false;
		}
		written++;
		if (written % 64 == 0)
		{
			printf(".");
			fflush(stdout);
		}
	}
	for (int address = 0; address < (int)file.size(); address++)
	{
		if (Ide64ReadFlash(address) != file[address])
		{
			printf("\nThe flash differs from the file at $%05x after writing\n", address);
			Ide64Leave();
			return false;
		}
	}
	printf("\n%d %s written, %d already the same, the flash is the same as %s\n", written, chip->pageWrite ? "pages" : "sectors", skipped, filename);
	Ide64Leave();
	return true;
}

bool Ide64Write(const char* filename, bool waitForJumper)
{
	FILE* fp = fopen(filename, "rb");
	if (!fp)
	{
		printf("Cannot open %s\n", filename);
		return false;
	}
	std::vector<unsigned char> file;
	unsigned char buffer[65536];
	size_t got;
	while ((got = fread(buffer, 1, sizeof(buffer), fp)) > 0)
	{
		file.insert(file.end(), buffer, buffer + got);
	}
	fclose(fp);
	if (file.empty() || file.size() % 0x4000 != 0)
	{
		printf("%s has %d bytes, an IDEDOS image is a multiple of 16 KB\n", filename, (int)file.size());
		return false;
	}
	return Ide64WriteImage(file, filename, waitForJumper);
}

// The IDE64 V4.1 always starts from the lower 64 KB of its 128 KB flash, the upper 64 KB can hold another IDEDOS (it
// has no switch for the IDEDOS bank). This swaps the two halves, so the other IDEDOS starts. The whole flash is read
// and saved to the backup file first. The upper half is written first: if the writing stops, the IDEDOS that started
// before is still in one half, and the other one in the backup file.
bool Ide64Swap(const char* backupFile, bool waitForJumper)
{
	auto prepare = [&](int banks, std::vector<unsigned char>& file)
		{
			const int kHalf = 0x10000;
			if (banks * 0x4000 != 2 * kHalf)
			{
				printf("The flash of this IDE64 has %d KB, --swapide64 needs 128 KB (two IDEDOS of 64 KB). Nothing was written\n", banks * 16);
				return false;
			}
			int reads;
			if (!Ide64ReadAll(banks, file, reads))
			{
				printf("Reading the IDE64 flash gave other data each time (%d reads). Nothing was written\n", reads);
				return false;
			}
			if (!WriteFile(backupFile, file.data(), (int)file.size()))
			{
				printf("Cannot write the backup %s. Nothing was written\n", backupFile);
				return false;
			}
			printf("Saved the whole flash (%d KB) to %s\n", (int)file.size() / 1024, backupFile);
			auto empty = [&](int start) { return std::all_of(file.begin() + start, file.begin() + start + kHalf, [](unsigned char b) { return b == 0xff; }); };
			if (empty(kHalf))
			{
				printf("The upper 64 KB are empty, there is no other IDEDOS to start. Nothing was written\n");
				return false;
			}
			if (std::equal(file.begin(), file.begin() + kHalf, file.begin() + kHalf))
			{
				printf("Both halves are the same, there is nothing to swap. Nothing was written\n");
				return false;
			}
			std::rotate(file.begin(), file.begin() + kHalf, file.end());
			printf("Swapping the two halves of 64 KB, the upper half first\n");
			return true;
		};
	std::string label = std::string(backupFile) + " with the halves swapped";
	return Ide64WriteImage(std::vector<unsigned char>(), label.c_str(), waitForJumper, prepare, true);
}
