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

// --probe: shows how the cartridge in each programmer slot answers, to find out how cartridge types can be detected.
// It only sends the autoselect command (flash ID) in several variants and resets the flash with $f0 after each one.
// Nothing is erased or programmed.
struct ProbeMethod
{
	const char* name;
	bool phi2Edge;		// The write with a PHI2 edge of the EF3 code, otherwise SendChipCommand() with the settings below
	bool high;			// ROMH instead of ROML
	int dotClockIterations;
	int registerDelay;
	bool extraClocking;
};

static void ProbeSend(const ProbeMethod& method, int address, int data)
{
	if (method.phi2Edge)
	{
		if (method.high)
		{
			SetDataROMHWithPHI2Edge(address, data);
		}
		else
		{
			SetDataROMLWithPHI2Edge(address, data);
		}
		return;
	}
	flashWriteCommandIsHiROM = method.high;
	flashWriteToggleDotClockIterations = method.dotClockIterations;
	RegisterDelayMicroSeconds = method.registerDelay;
	flashWriteCommandExtraClocking = method.extraClocking;
	SendChipCommand(address, data);
}

static std::string ProbeReadBytes(int programmerSlot, int count)
{
	EF3SelectProgrammerSlot(programmerSlot);
	std::string text;
	char hex[8];
	for (int high = 0; high < 2; high++)
	{
		text += high ? " ROMH" : "ROML";
		for (int address = 0; address < count; address++)
		{
			snprintf(hex, sizeof(hex), " %02x", high ? GetDataROMH(address) : GetDataROML(address));
			text += hex;
		}
	}
	return text;
}

static void Probe(void)
{
	const int kMaxProgrammerSlots = 5;
	// The settings of the existing write options for Megabyter and Gmod2 from ReadMe.md. The C64MegaCart default settings
	// are left out: they select ROMH while R/W stays read, so any other cartridge drives the data bus at the same time.
	const ProbeMethod methods[] = {
		{ "PHI2 edge, ROML", true, false, 0, 0, false },
		{ "PHI2 edge, ROMH", true, true, 0, 0, false },
		{ "Megabyter settings", false, false, 4, 1, true },
		{ "Gmod2 settings", false, true, 0, 0, true },
	};
	struct Addresses { const char* name; int first; int second; };
	const Addresses addressSets[] = { { "$aaa/$555", 0xaaa, 0x555 }, { "$555/$2aa", 0x555, 0x2aa } };

	// Keep the settings of the command line
	bool savedHigh = flashWriteCommandIsHiROM;
	int savedDotClock = flashWriteToggleDotClockIterations;
	int savedDelay = RegisterDelayMicroSeconds;
	bool savedExtra = flashWriteCommandExtraClocking;
	bool savedReversed = flashCommandsReversed;
	flashCommandsReversed = false;

	printf("Probing programmer slots 1 to %d, only the flash ID is read, nothing is erased or written\n", kMaxProgrammerSlots);
	std::string before[kMaxProgrammerSlots];
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		SelectProgrammerSlotForLines(programmerSlot);
		int game = digitalRead(26);
		int exrom = digitalRead(25);
		before[programmerSlot] = ProbeReadBytes(programmerSlot, 16);
		EF3SelectProgrammerSlot(programmerSlot);
		int io1Bank = GetDataIO1(0);
		int io1Slot = GetDataIO1(1);
		printf("Programmer slot %d: _GAME=%d _EXROM=%d, $de00 reads $%02x, $de01 reads $%02x\n", programmerSlot + 1, game, exrom, io1Bank, io1Slot);
		printf("  data      %s\n", before[programmerSlot].c_str());
	}

	for (const Addresses& addresses : addressSets)
	{
		for (const ProbeMethod& method : methods)
		{
			printf("Flash ID with %s, %s:\n", addresses.name, method.name);
			for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
			{
				// Autoselect again for every programmer slot: reading one slot can end it in all of them
				// (on a Gmod2 the ROMH read after the first slot did)
				ProbeSend(method, addresses.first, 0xaa);
				ProbeSend(method, addresses.second, 0x55);
				ProbeSend(method, addresses.first, 0x90);
				printf("  slot %d   %s\n", programmerSlot + 1, ProbeReadBytes(programmerSlot, 4).c_str());
				// Back to read mode with the same method, and with both select lines
				ProbeSend(method, 0, 0xf0);
				SetDataROMLWithPHI2Edge(0, 0xf0);
				SetDataROMHWithPHI2Edge(0, 0xf0);
			}
		}
	}

	// The GMod2 only writes to its flash with $de00 bit 7 (write enable) and bit 6 set, the commands go over ROMH
	// (gmod2.c of VICE). The register is written with all write methods and set back to 0 the same way afterwards:
	// on a Megabyter $c0 selects another bank, and on a Magic Desk bit 7 turns the cartridge off until then.
	auto setIO1 = [&](int value)
	{
		SetDataIO1WithPHI2Edge(0, value);
		flashWriteToggleDotClockIterations = 0;
		SetDataIO1(0, value);
		flashWriteToggleDotClockIterations = 4;
		SetDataIO1(0, value);
	};
	const ProbeMethod gmod2Methods[] = {
		{ "Gmod2 settings", false, true, 0, 0, true },
		{ "PHI2 edge, ROMH", true, true, 0, 0, false },
	};
	for (const ProbeMethod& method : gmod2Methods)
	{
		printf("Flash ID with $de00 = $c0, $555/$2aa, %s:\n", method.name);
		for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
		{
			setIO1(0xc0);
			ProbeSend(method, 0x555, 0xaa);
			ProbeSend(method, 0x2aa, 0x55);
			ProbeSend(method, 0x555, 0x90);
			printf("  slot %d   %s\n", programmerSlot + 1, ProbeReadBytes(programmerSlot, 4).c_str());
			ProbeSend(method, 0, 0xf0);
			SetDataROMLWithPHI2Edge(0, 0xf0);
			SetDataROMHWithPHI2Edge(0, 0xf0);
			setIO1(0x00);
		}
	}

	flashWriteCommandIsHiROM = savedHigh;
	flashWriteToggleDotClockIterations = savedDotClock;
	RegisterDelayMicroSeconds = savedDelay;
	flashWriteCommandExtraClocking = savedExtra;
	flashCommandsReversed = savedReversed;

	// The cartridges must be back in read mode
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		std::string after = ProbeReadBytes(programmerSlot, 16);
		printf("Programmer slot %d: data afterwards %s\n", programmerSlot + 1, after == before[programmerSlot] ? "the same as before" : ("DIFFERENT: " + after).c_str());
	}
	EF3SelectProgrammerSlot(theActiveSlot);
}

// --probeio1: tries several ways to write $de00 and shows whether a Magic Desk takes the value. $80 turns a Magic
// Desk off (_EXROM=1), bank 1 shows other data, $00 turns it on again. Two of the ways keep IO1 active while PHI2 is
// low, then an EasyFlash 3 drives the data bus too, so this is only for a Magic Desk alone in the programmer.
static void ProbeIO1WriteC64Cycle(int address, int data, bool phi2FallsDuringIO1, bool withPHI2)
{
	DataLatchOut::SetAddress(address);
	DataLatchOut::SetData(data);
	C64Control::ClearPHI2();
	C64Control::SetDataLatchOut();
	C64Control::SetWrite();
	C64Control::UpdateLatch();
	if (withPHI2)
	{
		C64Control::SetPHI2();
	}
	C64Control::SetIO1();
	C64Control::UpdateLatch();
	delayMicroseconds(2);
	if (phi2FallsDuringIO1)
	{
		// Like the C64: PHI2 falls first, IO1 and the data stay a little longer
		C64Control::ClearPHI2();
		C64Control::UpdateLatch();
		delayMicroseconds(2);
	}
	C64Control::ClearIO1();
	C64Control::UpdateLatch();
	delayMicroseconds(2);
	C64Control::SetRead();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();
}

static void ProbeIO1(void)
{
	struct Method { const char* name; int kind; };
	const Method methods[] = {
		{ "SetDataIO1, dot clock 0", 0 },
		{ "SetDataIO1, dot clock 4", 1 },
		{ "PHI2 edge", 2 },
		{ "PHI2 high, PHI2 falls during IO1", 3 },
		{ "PHI2 low during IO1", 4 },
	};
	auto write = [](int kind, int value)
	{
		int savedDotClock = flashWriteToggleDotClockIterations;
		switch (kind)
		{
		case 0: flashWriteToggleDotClockIterations = 0; SetDataIO1(0, value); break;
		case 1: flashWriteToggleDotClockIterations = 4; SetDataIO1(0, value); break;
		case 2: SetDataIO1WithPHI2Edge(0, value); break;
		case 3: ProbeIO1WriteC64Cycle(0, value, true, true); break;
		case 4: ProbeIO1WriteC64Cycle(0, value, false, false); break;
		}
		flashWriteToggleDotClockIterations = savedDotClock;
	};
	auto state = [](void)
	{
		SelectProgrammerSlotForLines(theActiveSlot);
		char text[64];
		snprintf(text, sizeof(text), "_EXROM=%d ROML %02x %02x %02x %02x", digitalRead(25), GetDataROML(0), GetDataROML(1), GetDataROML(2), GetDataROML(3));
		return std::string(text);
	};

	printf("Writing $de00 in programmer slot %d, only for a Magic Desk alone in the programmer\n", theActiveSlot + 1);
	printf("Before:  %s\n", state().c_str());
	for (const Method& method : methods)
	{
		printf("%s:\n", method.name);
		write(method.kind, 0x80);
		printf("  $80 -> %s\n", state().c_str());
		write(method.kind, 0x00);
		printf("  $00 -> %s\n", state().c_str());
		write(method.kind, 0x01);
		printf("  $01 -> %s\n", state().c_str());
		write(method.kind, 0x00);
		printf("  $00 -> %s\n", state().c_str());
	}
}

// --detect: names the cartridge type in each programmer slot, with the rules found with --probe.
// Every write goes to all programmer slots, and the select lines too, so a read in one slot is also a read in all others.
// Each test is sent again for every slot, and a reset at the start and at the end puts every cartridge into its state
// after power on. The tests were developed with the programmer simulator of the cartridge repository (sim/).
struct DetectID
{
	const char* type;
	const char* chip;
	ProbeMethod method;
	int first;
	int second;
	int manufacturer;
	int deviceOffset;
	int device;
};

static const int kDetectBytes = 64;
static const int kDetectMaxProgrammerSlots = 5;

static void DetectReadROML(int programmerSlot, int count, unsigned char* data)
{
	EF3SelectProgrammerSlot(programmerSlot);
	for (int address = 0; address < count; address++)
	{
		data[address] = (unsigned char) GetDataROML(address);
	}
}

static void DetectReadBoth(int programmerSlot, unsigned char data[2][kDetectBytes])
{
	EF3SelectProgrammerSlot(programmerSlot);
	for (int address = 0; address < kDetectBytes; address++)
	{
		data[0][address] = (unsigned char) GetDataROML(address);
		data[1][address] = (unsigned char) GetDataROMH(address);
	}
}

// Writes $de00 (or another IO1 address) only with a PHI2 edge. This write keeps the data on the bus until IO1 ends, so
// a latch that takes the value at the start and one that takes it at the end of IO1 both get it, and the EF3 CPLD too.
// SetDataIO1() takes the data off the bus before IO1 ends: a Magic Desk got $ff from it (turned off), and the
// c64-uni-cart Magic Desk clone then even locks itself until a reset.
static void DetectSetIO1(int address, int value)
{
	SetDataIO1WithPHI2Edge(address, value);
}

// A reset puts every cartridge into its state after power on: bank 0, read mode, Magic Desk turned on.
// The IO clear of ResetCartridge() before --detect can leave $ff in the registers (see DetectSetIO1()).
static void DetectReset(void)
{
	InterfaceControl::SetReset();
	InterfaceControl::UpdateLatch();
	delay(10);
	InterfaceControl::ClearReset();
	InterfaceControl::UpdateLatch();
	delay(10);
}

// Writes IO2 like SetDataIO1WithPHI2Edge(): the data stays on the bus until IO2 ends. SetDataIO2() takes it off before,
// a Super Games could then get $ff, which turns it off and locks its register until a reset.
static void DetectWriteIO2(int address, int value)
{
	DataLatchOut::SetAddress(address);
	DataLatchOut::SetData(value);
	C64Control::ClearPHI2();
	C64Control::SetDataLatchOut();
	C64Control::SetWrite();
	C64Control::UpdateLatch();
	C64Control::SetPHI2();
	C64Control::SetIO2();
	C64Control::UpdateLatch();
	delayMicroseconds(1);
	C64Control::ClearIO2();
	C64Control::UpdateLatch();
	C64Control::SetRead();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();
}

static int DetectReadIO2(int address)
{
	DataLatchOut::SetAddress(address);
	C64Control::SetIO2();
	C64Control::UpdateLatch();
	delayMicroseconds(1);
	int value = GetInputByte();
	C64Control::ClearIO2();
	C64Control::UpdateLatch();
	return value;
}

static void Detect(void)
{
	const int kMaxProgrammerSlots = kDetectMaxProgrammerSlots;
	const DetectID ids[] = {
		{ "EasyFlash 3", "MX29LV640EB", { "PHI2 edge, ROML", true, false, 0, 0, false }, 0xaaa, 0x555, 0xc2, 2, 0xcb },
		{ "C64MegaCart (top)", "M29F160FT",{ "C64MegaCart settings", false, true, 0, 0, true }, 0xaaa, 0x555, 0x01, 2, 0xd2 },
		{ "C64MegaCart (bottom)", "M29F160FB",{ "C64MegaCart settings", false, true, 0, 0, true }, 0xaaa, 0x555, 0x01, 2, 0xd8 },
		{ "Megabyter", "MX29F800", { "Megabyter settings", false, false, 4, 1, true }, 0xaaa, 0x555, 0xc2, 2, 0x58 },
		{ "Gmod2", "Am29F040", { "Gmod2 settings", false, true, 0, 0, true }, 0x555, 0x2aa, 0x01, 1, 0xa4 },
	};

	bool savedHigh = flashWriteCommandIsHiROM;
	int savedDotClock = flashWriteToggleDotClockIterations;
	int savedDelay = RegisterDelayMicroSeconds;
	bool savedExtra = flashWriteCommandExtraClocking;
	bool savedReversed = flashCommandsReversed;
	flashCommandsReversed = false;

	printf("Detecting the cartridges in programmer slots 1 to %d, nothing is erased or written\n", kMaxProgrammerSlots);
	// No IO1 read before the bank tests: the c64-uni-cart takes the value of the open bus with every IO1 access
	DetectReset();
	unsigned char before[kMaxProgrammerSlots][2][kDetectBytes];
	int game[kMaxProgrammerSlots], exrom[kMaxProgrammerSlots];
	// An Ultimax cartridge has its vectors at the end of ROMH ($fffa-$ffff), its first bytes can all be $ff
	bool vectorsEmpty[kMaxProgrammerSlots];
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		SelectProgrammerSlotForLines(programmerSlot);
		game[programmerSlot] = digitalRead(26);
		exrom[programmerSlot] = digitalRead(25);
		DetectReadBoth(programmerSlot, before[programmerSlot]);
		vectorsEmpty[programmerSlot] = true;
		for (int address = 0x1ffa; address < 0x2000; address++)
		{
			vectorsEmpty[programmerSlot] = vectorsEmpty[programmerSlot] && GetDataROMH(address) == 0xff;
		}
	}

	const DetectID* found[kMaxProgrammerSlots] = {};
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		for (const DetectID& id : ids)
		{
			ProbeSend(id.method, id.first, 0xaa);
			ProbeSend(id.method, id.second, 0x55);
			ProbeSend(id.method, id.first, 0x90);
			unsigned char data[4];
			DetectReadROML(programmerSlot, 4, data);
			ProbeSend(id.method, 0, 0xf0);
			SetDataROMLWithPHI2Edge(0, 0xf0);
			SetDataROMHWithPHI2Edge(0, 0xf0);
			// The ID must not be the normal data at these addresses
			const unsigned char* normal = before[programmerSlot][0];
			if (data[0] == id.manufacturer && data[id.deviceOffset] == id.device && (normal[0] != data[0] || normal[id.deviceOffset] != data[id.deviceOffset]))
			{
				found[programmerSlot] = &id;
				break;
			}
		}
	}

	// The DIY Gmod2 of Daniel Mantione (https://www.freepascal.org/~daniel/gmod2/): one board revision has the data bus
	// of the flash reversed (D0 to DQ7 ... D7 to DQ0, flasher-c64-rev.prg), so the Gmod2 commands above give no ID. The
	// reversed commands do, the ID reads reversed at ROML ($80 $25 for an Am29F040). The chips of flasher.c; the
	// SST39SF040, SST39VF040 and W39L040 need their commands at $5555/$2aaa, which --cfct 1 does not send.
	struct ReversedGmod2Chip { int manufacturer, device; const char* name; bool shortCommands; };
	const ReversedGmod2Chip reversedGmod2Chips[] = {
		{ 0x01, 0xa4, "Am29F040", true }, { 0x37, 0x86, "A29040B", true }, { 0x1c, 0x04, "EN29F040", true },
		{ 0xad, 0x40, "HY29F040", true }, { 0x20, 0xe2, "M29F040", true }, { 0x04, 0xa4, "MBM29F040", true },
		{ 0xc2, 0xa4, "MX29F040", true }, { 0xbf, 0xb7, "SST39SF040", false }, { 0xbf, 0xd7, "SST39VF040", false },
		{ 0xda, 0xd6, "W39L040", false },
	};
	const ReversedGmod2Chip* reversedGmod2[kMaxProgrammerSlots] = {};
	const ProbeMethod gmod2Settings = { "Gmod2 settings", false, true, 0, 0, true };
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		if (found[programmerSlot])
		{
			continue;
		}
		ProbeSend(gmod2Settings, 0x555, ReverseBits(0xaa));
		ProbeSend(gmod2Settings, 0x2aa, ReverseBits(0x55));
		ProbeSend(gmod2Settings, 0x555, ReverseBits(0x90));
		unsigned char data[4];
		DetectReadROML(programmerSlot, 4, data);
		ProbeSend(gmod2Settings, 0, ReverseBits(0xf0));
		const unsigned char* normal = before[programmerSlot][0];
		for (const ReversedGmod2Chip& chip : reversedGmod2Chips)
		{
			if (ReverseBits(data[0]) == chip.manufacturer && ReverseBits(data[1]) == chip.device && (normal[0] != data[0] || normal[1] != data[1]))
			{
				reversedGmod2[programmerSlot] = &chip;
			}
		}
	}

	// EasyFlash 1 and its clones: two 8 bit flash chips, one on ROML and one on ROMH. The autoselect goes to $5555/$2aaa
	// of the chip (bank 2 $1555, bank 1 $0aaa, as eapi-sst39sf040.s): an SST39SF040 needs it, the Am29F040, M29F040B and
	// MX29F040 only look at $555/$2aa. Both must give the same ID that is not the normal data. A Gmod2 (found above) has
	// no flash on ROMH, an EasyFlash 3 needs $aaa/$555. Clones use other flash chips, so any ID is taken. The writes use
	// a PHI2 edge: the data stays on the bus until the select line ends, R/W stays low and PHI2 high during the write.
	int ef1ID[kMaxProgrammerSlots][2][2] = {};
	bool ef1[kMaxProgrammerSlots] = {};
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		if (found[programmerSlot])
		{
			continue;
		}
		for (int high = 0; high < 2; high++)
		{
			auto send = high ? SetDataROMHWithPHI2Edge : SetDataROMLWithPHI2Edge;
			SetDataIO1WithPHI2Edge(0, 2);
			send(0x1555, 0xaa);
			SetDataIO1WithPHI2Edge(0, 1);
			send(0x0aaa, 0x55);
			SetDataIO1WithPHI2Edge(0, 2);
			send(0x1555, 0x90);
			SetDataIO1WithPHI2Edge(0, 0);
			SelectProgrammerSlotForLines(programmerSlot);
			ef1ID[programmerSlot][high][0] = high ? GetDataROMH(0) : GetDataROML(0);
			ef1ID[programmerSlot][high][1] = high ? GetDataROMH(1) : GetDataROML(1);
			SetDataROMLWithPHI2Edge(0, 0xf0);
			SetDataROMHWithPHI2Edge(0, 0xf0);
		}
		const int* l = ef1ID[programmerSlot][0];
		const int* h = ef1ID[programmerSlot][1];
		ef1[programmerSlot] = l[0] == h[0] && l[1] == h[1] && !(l[0] == 0xff && l[1] == 0xff)
			&& (l[0] != before[programmerSlot][0][0] || l[1] != before[programmerSlot][0][1])
			&& (h[0] != before[programmerSlot][1][0] || h[1] != before[programmerSlot][1][1]);
	}
	auto flashChip = [](int manufacturer, int device)
	{
		struct Chip { int manufacturer, device; const char* name; };
		const Chip chips[] = { { 0x01, 0xa4, "Am29F040" }, { 0x20, 0xe2, "M29F040B" }, { 0xc2, 0xa4, "MX29F040" }, { 0xbf, 0xb7, "SST39SF040" } };
		for (const Chip& chip : chips)
		{
			if (chip.manufacturer == manufacturer && chip.device == device)
			{
				return chip.name;
			}
		}
		return "unknown chip";
	};

	flashWriteCommandIsHiROM = savedHigh;
	flashWriteToggleDotClockIterations = savedDotClock;
	RegisterDelayMicroSeconds = savedDelay;
	flashWriteCommandExtraClocking = savedExtra;
	flashCommandsReversed = savedReversed;

	// Bank switching in $de00 (Magic Desk, Ocean, GMod3): bank 1 must show other data than bank 0. Only bits 0-5 are
	// used, bit 6 and 7 stay clear: bit 7 turns a Magic Desk off, bits 6+7 switch a Gmod2 into flash write mode.
	auto differs = [&](int programmerSlot, unsigned char data[2][kDetectBytes], bool high)
	{
		return memcmp(data[high ? 1 : 0], before[programmerSlot][high ? 1 : 0], kDetectBytes) != 0;
	};
	bool bankChanges[kMaxProgrammerSlots] = {};
	bool romhChanges[kMaxProgrammerSlots] = {};
	bool sameHalves[kMaxProgrammerSlots] = {};
	int banks[kMaxProgrammerSlots] = {};
	DetectSetIO1(0, 1);
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		unsigned char data[2][kDetectBytes];
		DetectReadBoth(programmerSlot, data);
		bankChanges[programmerSlot] = differs(programmerSlot, data, false);
		romhChanges[programmerSlot] = differs(programmerSlot, data, true);
		// An Ocean type A shows the same 8 KB bank in ROML and ROMH, a Comal 80 the two halves of a 16 KB bank
		sameHalves[programmerSlot] = memcmp(data[0], data[1], kDetectBytes) == 0;
	}
	// The number of banks: the first bank 2, 4 ... 32 that reads like bank 0 is a mirror, the upper bank bits are not
	// connected. Without a mirror there are 64 banks or more.
	for (int bank = 2; bank <= 64; bank *= 2)
	{
		if (bank < 64)
		{
			DetectSetIO1(0, bank);
		}
		for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
		{
			if (!bankChanges[programmerSlot] || banks[programmerSlot])
			{
				continue;
			}
			unsigned char data[2][kDetectBytes];
			DetectReadBoth(programmerSlot, data);
			if (bank == 64 || !differs(programmerSlot, data, false))
			{
				banks[programmerSlot] = bank;
			}
		}
	}
	// Fun Play / Power Play: the bank bits in $de00 are xx210xx3, $08 is bank 1 and $01 bank 8. So $02 selects bank 0
	// and looks like a mirror after 2 banks, but $10 (bank 2) and $08 (bank 1) show other data than bank 0. On a Magic
	// Desk or an Ocean with 2 banks, $10 and $08 both select bank 0.
	bool funplay[kMaxProgrammerSlots] = {};
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		funplay[programmerSlot] = bankChanges[programmerSlot] && banks[programmerSlot] == 2;
	}
	for (int value : { 0x10, 0x08 })
	{
		DetectSetIO1(0, value);
		for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
		{
			if (!funplay[programmerSlot])
			{
				continue;
			}
			unsigned char data[2][kDetectBytes];
			DetectReadBoth(programmerSlot, data);
			funplay[programmerSlot] = differs(programmerSlot, data, false);
		}
	}
	// Bit 7 turns a Magic Desk off (_EXROM goes high), an Ocean ignores it. A c64-uni-cart in Magic Desk mode
	// locks itself with bit 7 until the reset at the end.
	DetectSetIO1(0, 0x80);
	bool turnsOff[kMaxProgrammerSlots] = {};
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		SelectProgrammerSlotForLines(programmerSlot);
		turnsOff[programmerSlot] = exrom[programmerSlot] == 0 && digitalRead(25) == 1;
	}
	DetectReset();

	// The bank register of a Comal 80 is a 74LS161 clocked by the inverted PHI2: it only takes a value on the falling
	// edge of PHI2 while IO1 and R/W are low, as in a C64 cycle. The writes above have no falling PHI2 edge during IO1.
	// The C64 cycle keeps IO1 active while PHI2 is low, then an EasyFlash 3 drives the data bus too. So it is only
	// written when no programmer slot holds an EasyFlash 3. $01 selects bank 1 and keeps a Magic Desk turned on.
	bool anyEF3 = false;
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		anyEF3 = anyEF3 || (found[programmerSlot] && strcmp(found[programmerSlot]->type, "EasyFlash 3") == 0);
	}
	// The Hucky 64K card has the same kind of register (74LS173, inverted PHI2, without R/W): bits 0-2 select the bank,
	// $02 and $04 find the size of the EPROM, bit 3 turns it off (_EXROM high) until the reset.
	bool fallingEdgeChanges[kMaxProgrammerSlots] = {};
	bool fallingEdgeSameHalves[kMaxProgrammerSlots] = {};
	int fallingEdgeBanks[kMaxProgrammerSlots] = {};
	bool fallingEdgeOff[kMaxProgrammerSlots] = {};
	if (!anyEF3)
	{
		ProbeIO1WriteC64Cycle(0, 0x01, true, true);
		C64Control::SetPHI2();
		C64Control::UpdateLatch();
		for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
		{
			unsigned char data[2][kDetectBytes];
			DetectReadBoth(programmerSlot, data);
			fallingEdgeChanges[programmerSlot] = !bankChanges[programmerSlot] && differs(programmerSlot, data, false);
			fallingEdgeSameHalves[programmerSlot] = memcmp(data[0], data[1], kDetectBytes) == 0;
		}
		for (int bank : { 2, 4 })
		{
			ProbeIO1WriteC64Cycle(0, bank, true, true);
			C64Control::SetPHI2();
			C64Control::UpdateLatch();
			for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
			{
				if (!fallingEdgeChanges[programmerSlot] || fallingEdgeBanks[programmerSlot])
				{
					continue;
				}
				unsigned char data[2][kDetectBytes];
				DetectReadBoth(programmerSlot, data);
				if (!differs(programmerSlot, data, false))
				{
					fallingEdgeBanks[programmerSlot] = bank;
				}
			}
		}
		ProbeIO1WriteC64Cycle(0, 0x08, true, true);
		C64Control::SetPHI2();
		C64Control::UpdateLatch();
		for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
		{
			SelectProgrammerSlotForLines(programmerSlot);
			fallingEdgeOff[programmerSlot] = exrom[programmerSlot] == 0 && digitalRead(25) == 1;
		}
		DetectReset();
	}

	// A C64 Game System takes the address bits A0-A5 of every IO1 access as the bank, read or write, the data is not
	// used. $00 written to $de01 selects bank 1 there, while a Magic Desk, an Ocean, a Gmod2 and a Megabyter stay in
	// bank 0 and the EF3 in its slot 0. The addresses $de02 ... $de20 then find the first mirror of bank 0, like above.
	bool addressChanges[kMaxProgrammerSlots] = {};
	int addressBanks[kMaxProgrammerSlots] = {};
	DetectSetIO1(1, 0);
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		unsigned char data[2][kDetectBytes];
		DetectReadBoth(programmerSlot, data);
		addressChanges[programmerSlot] = !bankChanges[programmerSlot] && differs(programmerSlot, data, false);
	}
	for (int bank = 2; bank <= 64; bank *= 2)
	{
		if (bank < 64)
		{
			DetectSetIO1(bank, 0);
		}
		for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
		{
			if (!addressChanges[programmerSlot] || addressBanks[programmerSlot])
			{
				continue;
			}
			unsigned char data[2][kDetectBytes];
			DetectReadBoth(programmerSlot, data);
			if (bank == 64 || !differs(programmerSlot, data, false))
			{
				addressBanks[programmerSlot] = bank;
			}
		}
	}
	DetectReset();

	// Tests with reads and IO2:
	// - Simons' BASIC: writing IO1 switches it to 16K, reading IO1 to 8K (_GAME)
	// - Dinamic: reading $de01 selects bank 1, a write does not (so a C64 Game System is already found above)
	// - Westermann: reading IO2 switches it from 16K to 8K
	// - Zaxxon: reading ROML $9000 selects the other ROMH bank
	// - Super Games: the bank register is in $df00, $01 selects bank 1. It also goes into the RAM of an EasyFlash 3,
	//   which keeps nothing without power anyway.
	// Reading IO1 changes a c64-uni-cart, the reset at the end sets every cartridge back. Simons' BASIC and Westermann
	// must show their ROM at ROML: a Kung Fu Flash on the programmer drives no data, but changed _GAME in this test.
	bool readChanges[kMaxProgrammerSlots] = {};
	bool simons[kMaxProgrammerSlots] = {};
	bool westermann[kMaxProgrammerSlots] = {};
	bool zaxxon[kMaxProgrammerSlots] = {};
	bool superGames[kMaxProgrammerSlots] = {};
	int gameAfterWrite[kMaxProgrammerSlots];
	DetectSetIO1(0, 0);
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		SelectProgrammerSlotForLines(programmerSlot);
		gameAfterWrite[programmerSlot] = digitalRead(26);
	}
	GetDataIO1(1);
	int gameBeforeIO2[kMaxProgrammerSlots];
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		SelectProgrammerSlotForLines(programmerSlot);
		gameBeforeIO2[programmerSlot] = digitalRead(26);
		simons[programmerSlot] = gameAfterWrite[programmerSlot] == 0 && gameBeforeIO2[programmerSlot] == 1 && !IsEmpty(before[programmerSlot][0], kDetectBytes);
		unsigned char data[2][kDetectBytes];
		DetectReadBoth(programmerSlot, data);
		readChanges[programmerSlot] = !bankChanges[programmerSlot] && !addressChanges[programmerSlot] && differs(programmerSlot, data, false);
	}
	DetectReadIO2(0);
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		SelectProgrammerSlotForLines(programmerSlot);
		westermann[programmerSlot] = !simons[programmerSlot] && gameBeforeIO2[programmerSlot] == 0 && digitalRead(26) == 1 && !IsEmpty(before[programmerSlot][0], kDetectBytes);
	}
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		// Only ROMH after the ROML read, a ROML read from $8000 would select ROMH bank 0 again
		SelectProgrammerSlotForLines(programmerSlot);
		GetDataROML(0x1000);
		unsigned char romh[kDetectBytes];
		for (int address = 0; address < kDetectBytes; address++)
		{
			romh[address] = (unsigned char) GetDataROMH(address);
		}
		GetDataROML(0);
		zaxxon[programmerSlot] = memcmp(romh, before[programmerSlot][1], kDetectBytes) != 0;
	}
	DetectWriteIO2(0, 1);
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		unsigned char data[2][kDetectBytes];
		DetectReadBoth(programmerSlot, data);
		superGames[programmerSlot] = !bankChanges[programmerSlot] && differs(programmerSlot, data, false) && differs(programmerSlot, data, true);
	}
	DetectReset();

	// Freezers show a part of their ROM in the IO area:
	// - Action Replay 4.2/5/6: IO2 reads the ROM at $1f00 of the bank, $08 in $de00 selects bank 1. R/W is not decoded,
	//   so the $de01 read below writes the open bus ($ff) into the register and turns it off until the reset.
	// - Retro Replay: like the Action Replay, but $de00/$de01 read back the bank bits (bit 3 after $08) and the flash
	//   jumper (bit 0).
	// - Final Cartridge III: IO1 reads the ROM at $1e00, IO2 at $1f00. $41 in $dfff selects bank 1 (16K, no NMI).
	// - Super Snapshot 5: IO1 reads the ROM at $1e00. After a reset RAM is at ROML, $02 in $de00 maps the ROM (bank 0),
	//   $06 selects bank 1.
	// A mirror only counts when the ROM data there is varied, not one byte repeated.
	auto readBlock = [&](int programmerSlot, int (*read)(int), int start, unsigned char* data)
	{
		SelectProgrammerSlotForLines(programmerSlot);
		for (int i = 0; i < kDetectBytes; i++)
		{
			data[i] = (unsigned char) read(start + i);
		}
	};
	auto varied = [](const unsigned char* data)
	{
		bool seen[256] = {};
		int distinct = 0;
		for (int i = 0; i < kDetectBytes; i++)
		{
			distinct += seen[data[i]] ? 0 : 1;
			seen[data[i]] = true;
		}
		return distinct >= 4;
	};
	bool io2Mirror[kMaxProgrammerSlots] = {};
	bool io1Mirror[kMaxProgrammerSlots] = {};
	bool arBank[kMaxProgrammerSlots] = {};
	bool fc3Bank[kMaxProgrammerSlots] = {};
	bool ss5Bank[kMaxProgrammerSlots] = {};
	int rrStatus[kMaxProgrammerSlots];
	bool ide64[kMaxProgrammerSlots] = {};
	int ide64Status[kMaxProgrammerSlots] = {};
	static unsigned char pre[kMaxProgrammerSlots][kDetectBytes];
	static unsigned char rom[kMaxProgrammerSlots][kDetectBytes];
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		unsigned char io2[kDetectBytes];
		readBlock(programmerSlot, GetDataROML, 0x1f00, rom[programmerSlot]);
		readBlock(programmerSlot, DetectReadIO2, 0, io2);
		io2Mirror[programmerSlot] = memcmp(io2, rom[programmerSlot], kDetectBytes) == 0 && varied(io2);
	}
	// $08 also turns an RGCD cartridge off (_EXROM high, until the reset), a Magic Desk selects bank 8 and stays on
	DetectSetIO1(0, 0x08);
	bool off08[kMaxProgrammerSlots] = {};
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		readBlock(programmerSlot, GetDataROML, 0, pre[programmerSlot]);
		arBank[programmerSlot] = memcmp(pre[programmerSlot], before[programmerSlot][0], kDetectBytes) != 0;
		SelectProgrammerSlotForLines(programmerSlot);
		off08[programmerSlot] = exrom[programmerSlot] == 0 && digitalRead(25) == 1;
	}
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		SelectProgrammerSlotForLines(programmerSlot);
		rrStatus[programmerSlot] = GetDataIO1(1);
	}
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		readBlock(programmerSlot, GetDataROML, 0, pre[programmerSlot]);
	}
	DetectWriteIO2(0xff, 0x41);
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		unsigned char data[kDetectBytes];
		readBlock(programmerSlot, GetDataROML, 0, data);
		fc3Bank[programmerSlot] = memcmp(data, pre[programmerSlot], kDetectBytes) != 0;
	}
	DetectReset();
	DetectSetIO1(0, 0x02);
	// All ROML reads first: an IO1 read changes the bank of a C64 Game System or a Dinamic in every slot
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		readBlock(programmerSlot, GetDataROML, 0x1e00, rom[programmerSlot]);
		readBlock(programmerSlot, GetDataROML, 0, pre[programmerSlot]);
	}
	// IDE64 V4.x: $DE32 reads its status (bit 5 V4.x, bank 0 after the reset), $DE60-$DEFF show the ROM of $9E60-$9EFF.
	// Read with the 5 us of Ide64ReadBus(), with 1 us a real IDE64 V4.1 was taken for an EasyFlash 1 now and then.
	static unsigned char romIde64[kMaxProgrammerSlots][kDetectBytes];
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		readBlock(programmerSlot, Ide64ReadROML, 0x1e60, romIde64[programmerSlot]);
	}
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		unsigned char io1Data[kDetectBytes];
		readBlock(programmerSlot, GetDataIO1, 0, io1Data);
		io1Mirror[programmerSlot] = memcmp(io1Data, rom[programmerSlot], kDetectBytes) == 0 && varied(io1Data);
		SelectProgrammerSlotForLines(programmerSlot);
		// The IDE64 decodes the whole address: $DE60 is flash $1E60 in the bank, $0060 would be flash $0060
		ide64Status[programmerSlot] = Ide64ReadIO1(0x32);
		readBlock(programmerSlot, Ide64ReadIO1, 0x60, io1Data);
		ide64[programmerSlot] = (ide64Status[programmerSlot] & 0xfc) == 0x20 && memcmp(io1Data, romIde64[programmerSlot], kDetectBytes) == 0 && varied(io1Data);
	}
	// Final Cartridge 1 and 2: the IO1 reads turned it off (_GAME and _EXROM high), an IO2 read turns it on (16K)
	bool fc1[kMaxProgrammerSlots] = {};
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		SelectProgrammerSlotForLines(programmerSlot);
		fc1[programmerSlot] = digitalRead(26) == 1 && digitalRead(25) == 1;
	}
	DetectReadIO2(0);
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		SelectProgrammerSlotForLines(programmerSlot);
		fc1[programmerSlot] = fc1[programmerSlot] && digitalRead(26) == 0 && digitalRead(25) == 0;
	}
	DetectSetIO1(0, 0x06);
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		unsigned char data[kDetectBytes];
		readBlock(programmerSlot, GetDataROML, 0, data);
		ss5Bank[programmerSlot] = memcmp(data, pre[programmerSlot], kDetectBytes) != 0;
	}
	DetectReset();

	// A GMod3 returns the low 8 bank bits when $de00-$de07 is read, and bank bits 8-10 (the address bits of the write)
	// when $de08 is read. The other cartridges here have no register to read back: the open bus reads $ff.
	// Only addresses with A1 clear are written: A1 selects the mode register of a Megabyter. $de04 also goes to the
	// bank register of a Magic Desk, an Ocean, a Gmod2 and the EF3, with bits 6 and 7 clear.
	int io1[kMaxProgrammerSlots][4];
	const int pattern[2][2] = { { 0x04, 0x15 }, { 0x00, 0x2a } };
	for (int step = 0; step < 2; step++)
	{
		DetectSetIO1(pattern[step][0], pattern[step][1]);
		for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
		{
			SelectProgrammerSlotForLines(programmerSlot);
			io1[programmerSlot][step * 2] = GetDataIO1(0);
			SelectProgrammerSlotForLines(programmerSlot);
			io1[programmerSlot][step * 2 + 1] = GetDataIO1(8);
		}
	}
	// The IO1 reads changed the bank of a c64-uni-cart, the reset sets every cartridge back
	DetectReset();
	auto isGmod3 = [&](int programmerSlot)
	{
		const int* r = io1[programmerSlot];
		return r[0] == pattern[0][1] && (r[1] & 7) == pattern[0][0] && r[2] == pattern[1][1] && (r[3] & 7) == pattern[1][0];
	};

	bool ss5[kMaxProgrammerSlots] = {};
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		bool allEmpty = true;
		bool io1Open = true;
		for (int i = 0; i < kDetectBytes; i++)
		{
			allEmpty = allEmpty && before[programmerSlot][0][i] == 0xff && before[programmerSlot][1][i] == 0xff;
		}
		allEmpty = allEmpty && vectorsEmpty[programmerSlot];
		for (int i = 0; i < 4; i++)
		{
			io1Open = io1Open && io1[programmerSlot][i] == 0xff;
		}
		int g = game[programmerSlot], e = exrom[programmerSlot];
		char size[48] = "";
		int numBanks = banks[programmerSlot] ? banks[programmerSlot] : addressBanks[programmerSlot];
		if (numBanks > 0)
		{
			snprintf(size, sizeof(size), ", %s%d banks", numBanks == 64 ? "at least " : "", numBanks);
		}
		printf("Programmer slot %d: ", programmerSlot + 1);
		if (found[programmerSlot])
		{
			printf("%s (flash ID $%02x $%02x, %s)\n", found[programmerSlot]->type, found[programmerSlot]->manufacturer, found[programmerSlot]->device, found[programmerSlot]->chip);
		}
		else if (reversedGmod2[programmerSlot])
		{
			const ReversedGmod2Chip& chip = *reversedGmod2[programmerSlot];
			printf("Gmod2 (DIY by Daniel Mantione, the data bus of the flash reversed: flash ID $%02x $%02x reversed, %s), %s\n", chip.manufacturer, chip.device, chip.name,
				chip.shortCommands ? "write it with --cfwec --cfct 1 --cfrev --cfrdd 2" : "its flash needs the commands at $5555/$2aaa, which --cfct 1 does not send");
		}
		else if (ef1[programmerSlot])
		{
			const int* id = ef1ID[programmerSlot][0];
			// An EasyFlash never has a 128 KB chip, the Am29F010 is the flash of a Retro Replay
			if (id[0] == 0x01 && id[1] == 0x20)
			{
				printf("Retro Replay with the flash jumper set (flash ID $01 $20 in ROML and ROMH, Am29F010)\n");
			}
			else
			{
				printf("EasyFlash 1 or compatible (flash ID $%02x $%02x in ROML and ROMH, %s)\n", id[0], id[1], flashChip(id[0], id[1]));
			}
		}
		else if (ide64[programmerSlot])
		{
			printf("IDE64 V4.x ($DE32 reads $%02x, $DE60-$DEFF show the ROM, --ide64id shows more)\n", ide64Status[programmerSlot]);
		}
		else if (isGmod3(programmerSlot))
		{
			printf("GMod3 (the bank register in $de00 and $de08 reads back)\n");
		}
		else if (arBank[programmerSlot] && (rrStatus[programmerSlot] & 0xb8) == 0x08)
		{
			printf("Retro Replay or Nordic Replay (Action Replay compatible, $de01 reads back the bank%s)\n", (rrStatus[programmerSlot] & 1) ? ", flash jumper set" : "");
		}
		else if (io2Mirror[programmerSlot] && arBank[programmerSlot] && rrStatus[programmerSlot] == 0xff)
		{
			printf("Action Replay 4.2, 5 or 6 or compatible (IO2 shows the ROM, the bank is in $de00)\n");
		}
		else if (fc3Bank[programmerSlot] && io1Mirror[programmerSlot] && io2Mirror[programmerSlot])
		{
			printf("Final Cartridge III (IO1 and IO2 show the ROM, the bank is in $dfff)\n");
		}
		else if (fc1[programmerSlot] && io1Mirror[programmerSlot] && io2Mirror[programmerSlot] && !fc3Bank[programmerSlot])
		{
			printf("Final Cartridge 1 or 2 (IO1 and IO2 show the ROM, IO1 turns it off, IO2 on)\n");
		}
		else if (ss5Bank[programmerSlot] && io1Mirror[programmerSlot] && !io2Mirror[programmerSlot])
		{
			ss5[programmerSlot] = true;
			printf("Super Snapshot 5 (IO1 shows the ROM, the bank is in $de00)\n");
		}
		else if (addressChanges[programmerSlot] && g == 1 && e == 0)
		{
			printf("C64 Game System (8K, no flash ID, the address of the IO1 access selects the bank%s)\n", size);
		}
		else if (readChanges[programmerSlot] && g == 1 && e == 0)
		{
			printf("Dinamic (8K, no flash ID, reading $de0n selects bank n, writing does not)\n");
		}
		else if (simons[programmerSlot])
		{
			printf("Simons' BASIC (reading $de00 switches it to 8K, writing $de00 to 16K)\n");
		}
		else if (westermann[programmerSlot])
		{
			printf("Westermann (16K, reading $df00 switches it to 8K)\n");
		}
		else if (zaxxon[programmerSlot] && g == 0 && e == 0)
		{
			printf("Zaxxon (16K, reading ROML $9000 selects the other ROMH bank)\n");
		}
		else if (superGames[programmerSlot] && g == 0 && e == 0)
		{
			printf("Super Games (16K, no flash ID, the bank is in $df00)\n");
		}
		else if (readChanges[programmerSlot])
		{
			printf("unknown cartridge whose bank changes when $de01 is read (_GAME=%d _EXROM=%d), --probe shows more\n", g, e);
		}
		else if (addressChanges[programmerSlot])
		{
			printf("unknown cartridge whose bank is the address of the IO1 access (_GAME=%d _EXROM=%d%s), --probe shows more\n", g, e, size);
		}
		else if (bankChanges[programmerSlot] && g == 1 && e == 0 && off08[programmerSlot])
		{
			// Bank 8 of the bank count test turned it off, only the mirrors at 2 and 4 count
			int rgcdBanks = (banks[programmerSlot] == 2 || banks[programmerSlot] == 4) ? banks[programmerSlot] : 8;
			printf("RGCD or compatible (8K, no flash ID, $de00 bits 0-2 select the bank, bit 3 turns it off until a reset, %d banks)\n", rgcdBanks);
		}
		else if (funplay[programmerSlot] && g == 1 && e == 0)
		{
			printf("Fun Play or Power Play (8K, no flash ID, the bank bits in $de00 are xx210xx3, $08 selects bank 1)\n");
		}
		else if (bankChanges[programmerSlot] && g == 1 && e == 0 && turnsOff[programmerSlot])
		{
			printf("Magic Desk (no flash ID, the data changes with the bank in $de00, bit 7 turns it off%s)\n", size);
		}
		else if (bankChanges[programmerSlot] && g == 1 && e == 0)
		{
			printf("Ocean type B (8K, no flash ID, the data changes with the bank in $de00, bit 7 does not turn it off%s)\n", size);
		}
		else if (bankChanges[programmerSlot] && g == 0 && e == 0 && romhChanges[programmerSlot] && sameHalves[programmerSlot])
		{
			printf("Ocean type A (16K, no flash ID, the data changes with the bank in $de00, ROML and ROMH show the same bank%s)\n", size);
		}
		else if (fallingEdgeChanges[programmerSlot] && g == 0 && e == 0 && !fallingEdgeSameHalves[programmerSlot])
		{
			printf("Comal 80 (16K, no flash ID, the bank in $de00 only changes on a falling PHI2 edge as in a C64 cycle)\n");
		}
		else if (fallingEdgeChanges[programmerSlot] && g == 1 && e == 0 && fallingEdgeOff[programmerSlot])
		{
			int huckyBanks = fallingEdgeBanks[programmerSlot] ? fallingEdgeBanks[programmerSlot] : 8;
			printf("Hucky 64K card or compatible (8K, no flash ID, the bank in $de00 only changes on a falling PHI2 edge as in a C64 cycle, bit 3 turns it off until a reset, %d banks)\n", huckyBanks);
		}
		else if (fallingEdgeChanges[programmerSlot])
		{
			printf("unknown cartridge with banks in $de00 that only change on a falling PHI2 edge (_GAME=%d _EXROM=%d), --probe shows more\n", g, e);
		}
		else if (bankChanges[programmerSlot])
		{
			printf("unknown cartridge with banks in $de00 (_GAME=%d _EXROM=%d%s), --probe shows more\n", g, e, size);
		}
		else if (!allEmpty && g == 1 && e == 0 && anyEF3)
		{
			printf("8K ROM cartridge (normal 8K, or a Magic Desk or Ocean whose bank in $de00 does not change the data, or a Hucky 64K card: with an EasyFlash 3 in the programmer its bank can not be switched)\n");
		}
		else if (!allEmpty && g == 1 && e == 0)
		{
			printf("8K ROM cartridge (normal 8K, or a Magic Desk or Ocean whose bank in $de00 does not change the data)\n");
		}
		else if (!allEmpty && g == 0 && e == 0 && anyEF3)
		{
			printf("16K ROM cartridge (normal 16K, or a Comal 80: with an EasyFlash 3 in the programmer its bank can not be switched)\n");
		}
		else if (!allEmpty && g == 0 && e == 0)
		{
			printf("16K ROM cartridge (normal 16K, the bank in $de00 does not change the data)\n");
		}
		else if (!allEmpty && g == 0 && e == 1)
		{
			printf("Ultimax cartridge (the bank in $de00 does not change the data)\n");
		}
		else if (allEmpty && io1Open && g == 1 && e == 1)
		{
			printf("empty\n");
		}
		else if (allEmpty && io1Open)
		{
			// A cartridge with a microcontroller (Kung Fu Flash, TeensyROM) or the GMod4 needs a PHI2 clock of about
			// 1 MHz to answer, the programmer has none: it may pull _GAME/_EXROM, but drives no data
			printf("empty or no data (_GAME=%d _EXROM=%d)%s\n", g, e,
				g == 0 || e == 0 ? ": pulls _GAME or _EXROM, but drives no data, maybe a cartridge that needs a PHI2 clock (Kung Fu Flash, TeensyROM, GMod4)" : "");
		}
		else
		{
			printf("unknown (_GAME=%d _EXROM=%d, $de00 reads $%02x, ROML %02x %02x %02x %02x), --probe shows more\n", g, e, io1[programmerSlot][2], before[programmerSlot][0][0], before[programmerSlot][0][1], before[programmerSlot][0][2], before[programmerSlot][0][3]);
		}
	}

	// After the reset the cartridges must read as at the start. A Super Snapshot 5 has RAM at ROML after a reset, the
	// flash commands of the tests above were written into it.
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		unsigned char after[2][kDetectBytes];
		DetectReadBoth(programmerSlot, after);
		if (!ss5[programmerSlot] && memcmp(after, before[programmerSlot], sizeof(after)) != 0)
		{
			printf("WARNING: programmer slot %d reads other data than before the detection\n", programmerSlot + 1);
			for (int high = 0; high < 2; high++)
			{
				for (int line = 0; line < kDetectBytes; line += 16)
				{
					if (memcmp(after[high] + line, before[programmerSlot][high] + line, 16) == 0)
					{
						continue;
					}
					printf("  %s $%02x before", high ? "ROMH" : "ROML", line);
					for (int i = 0; i < 16; i++)
					{
						printf(" %02x", before[programmerSlot][high][line + i]);
					}
					printf("\n  %s $%02x after ", high ? "ROMH" : "ROML", line);
					for (int i = 0; i < 16; i++)
					{
						printf(" %02x", after[high][line + i]);
					}
					printf("\n");
				}
			}
		}
	}
	SelectProgrammerSlotForLines(theActiveSlot);
}

int main(int argc, char** argv)
{
	unsigned char bankData[8192];
	FILE* fp;

	auto start = std::chrono::steady_clock::now();

	HardwareInit();
	InitCartTool();

	int argPos = 1;
	while (argPos < argc)
	{
		// Parse cartridge options
		if (strcasecmp(argv[argPos], "--cfwch") == 0)
		{
			argPos++;
			flashWriteCommandIsHiROM = true;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfwcl") == 0)
		{
			argPos++;
			flashWriteCommandIsHiROM = false;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfwdci") == 0)
		{
			argPos++;
			flashWriteToggleDotClockIterations = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			continue;
		}
		if (strcasecmp(argv[argPos], "--crdms") == 0)
		{
			argPos++;
			RegisterDelayMicroSeconds = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfwec") == 0)
		{
			argPos++;
			flashWriteCommandExtraClocking = true;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfwnec") == 0)
		{
			argPos++;
			flashWriteCommandExtraClocking = false;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfrev") == 0)
		{
			argPos++;
			flashCommandsReversed = true;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfub") == 0)
		{
			argPos++;
			flashUnlockBypass = true;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfrdd") == 0)
		{
			argPos++;
			readDelayMicroSeconds = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfct") == 0)
		{
			argPos++;
			flashChipType = (FlashChipType) std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			continue;
		}

		HardwareInit();
		InitCartTool();

		InterfaceControl::ClearRelay1();
		InterfaceControl::UpdateLatch();

		if (strcasecmp(argv[argPos], "--waitbutton") == 0 || strcasecmp(argv[argPos], "-b") == 0)
		{
			argPos++;
			printf("Waiting for button...\n");
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
			continue;
		}

		// Default to the first slot...
		InterfaceControl::SetActiveSlot(theActiveSlot);
		InterfaceControl::UpdateLatch();

		// Enable power
		InterfaceControl::SetRelay1();
		InterfaceControl::UpdateLatch();
		delay(100); // A small delay to allow the relay to physically switch on the cartridge power

		ResetCartridge();

		if (strcasecmp(argv[argPos], "--erasechips") == 0 || strcasecmp(argv[argPos], "-ec") == 0)
		{
			argPos++;
			int numChips = std::stoi(argv[argPos], nullptr, 0);
			argPos++;

			InterfaceControl::SetLED0();
			InterfaceControl::UpdateLatch();

			for (int chip = 0; chip < numChips; chip++)
			{
				printf("Erasing chip %d\n" , chip);
				SetDataIO2(chip);
				SendChipCommandErase();
			}

			for (int slot = 0; slot < activeSlots; slot++)
			{
				InterfaceControl::SetActiveSlot(slot);

				for (int chip = 0; chip < numChips; chip++)
				{
					printf("Waiting for chip %d\n", chip);
					SetDataIO2(chip);
					WaitForStatusRegisterEqual(0xff);
				}

				SetDataIO2(0);
			}

			InterfaceControl::ClearLED0();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--erase") == 0 || strcasecmp(argv[argPos], "-e") == 0)
		{
			argPos++;

			InterfaceControl::SetLED0();
			InterfaceControl::UpdateLatch();

			printf("Erasing...\n");
			SendChipCommandErase();

			for (int slot = 0; slot < activeSlots; slot++)
			{
				InterfaceControl::SetActiveSlot(slot);
				WaitForStatusRegisterEqual(0xff);
			}

			InterfaceControl::ClearLED0();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--write") == 0 || strcasecmp(argv[argPos], "-w") == 0)
		{
			argPos++;

			InterfaceControl::SetLED1();
			InterfaceControl::UpdateLatch();

			printf("Writing...\n");
			// Write some data to the flash, using the program command sequence
			fp = fopen(argv[argPos], "rb");
			argPos++;
			if (fp == 0)
			{
				printf("Error reading input file\n");
				continue;
			}

			bool gotError = false;
			int bank = 0;
			if (flashUnlockBypass)
			{
				SendChipCommandUnlockBypass();
			}
			while (!feof(fp))
			{
				size_t numBytes = fread(bankData, sizeof(bankData[0]), sizeof(bankData), fp);
				if (gotError)
				{
					break;
				}
				if (numBytes == 0)
				{
					break;
				}
				printf("Got bytes %d for bank %d\n", numBytes, bank);

				// Set the bank register
				SetDataIO1(0, bank);
				SetDataIO2(bank >> 8);

				for (int address = 0; address < (int)sizeof(bankData); address++)
				{
					if (gotError)
					{
						break;
					}

					if ((address & 0xff) == 0)
					{
						printf(".");
						fflush(stdout);
					}

					// If it's going to be the same as an erased flash byte then skip it :)
					if (bankData[address] == 0xff)
					{
						continue;
					}

					// Program commands
					if (flashUnlockBypass)
					{
						// At the address of the byte, so the address latches need no update for it
						SendChipCommand(address, 0xa0);
					}
					else
					{
						SendChipCommandProgram();
					}

					// Program command4 (the actual byte)
					DataLatchOut::SetAddress(address);
					int theWriteValue = bankData[address];
					DataLatchOut::SetData(theWriteValue);

					PerformCartridgeWriteCycle();

					// Data polling of all slots in passes: one pass sets ROML low once and reads every slot that is not done
					// yet. With ROML low every slot drives its data, so the next slot only needs the latch update of the slot
					// select (the data bus follows it in less than 100ns, the wait in GetInputByte() covers it).
					int slotsNotDone = (1 << activeSlots) - 1;
					int iterations = 0;
					while (slotsNotDone != 0)
					{
						C64Control::SetLowROM();
						C64Control::UpdateLatch();
						//				delay(0);	// Certainly more than the 20ns for a bus read
						if (iterations > 50)
						{
							delayMicroseconds(1);
						}
						for (int slot = 0; slot < activeSlots; slot++)
						{
							if ((slotsNotDone & (1 << slot)) == 0)
							{
								continue;
							}
							InterfaceControl::SetActiveSlot(slot);
							// Without it the data polling of the DIY Gmod2 can read DQ7 (on D0) as done too early
							if (readDelayMicroSeconds > 0)
							{
								delayMicroseconds(readDelayMicroSeconds);
							}
							if (GetInputByte() == theWriteValue)
							{
								slotsNotDone &= ~(1 << slot);
							}
						}
						C64Control::ClearLowROM();
						C64Control::UpdateLatch();
						if (slotsNotDone != 0 && iterations++ > 100)
						{
							for (int slot = 0; slot < activeSlots; slot++)
							{
								if ((slotsNotDone & (1 << slot)) != 0)
								{
									printf("There seems to be a problem verifying the byte at address $%04x slot %d\n", address, slot+1);
								}
							}
							ReportCartridgeError();
							gotError = true;
							break;
						}
					}
					InterfaceControl::SetActiveSlot(theActiveSlot);
				}

				if (gotError)
				{
					break;
				}

				printf("\nBank done\n");
				bank++;
			}
			if (flashUnlockBypass)
			{
				SendChipCommandUnlockBypassReset();
			}

			InterfaceControl::ClearLED1();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--eraseblock") == 0 || strcasecmp(argv[argPos], "-eb") == 0)
		{
			argPos++;

			InterfaceControl::SetLED0();
			InterfaceControl::UpdateLatch();

			int bank = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			printf("Erasing one block at bank %d\n" , bank);
			// Write some data to the flash, using the erase block command sequence
			DataLatchOut::SetAddress(0);
			SetDataIO1(0, bank);
			SetDataIO2(bank >> 8);

			SendChipCommandBlockErase();

			for (int slot = 0; slot < activeSlots; slot++)
			{
				InterfaceControl::SetActiveSlot(slot);
				WaitForStatusRegisterEqual(0xff);
			}

			InterfaceControl::SetActiveSlot(theActiveSlot);

			InterfaceControl::ClearLED0();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--read") == 0 || strcasecmp(argv[argPos], "-r") == 0)
		{
			argPos++;
			int numBanks = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			printf("Reading...\n");
			int maxDelayNeeded = 0;
			fp = fopen(argv[argPos], "wb");
			argPos++;

			for (int bank = 0; bank < numBanks; bank++)
			{
				printf("Bank %d\n", bank);
				SetDataIO1(0, bank);
				SetDataIO2(bank >> 8);

				for (int address = 0; address < (int)sizeof(bankData); address++)
				{
					/*
								if ((address & 0x3ff) == 0)
								{
									printf(".");
									fflush(stdout);
								}
					*/

					DataLatchOut::SetAddress(address);
					C64Control::ClearFlashWrite();
					C64Control::ClearDataLatchOut();

					// Tries two reads without any delay, then progressively increases the delay until we get two reads that are the same
					int gotPrevious = -1;
					int gotNow = -2;
					int progressiveDelay = 0;
					while (gotPrevious != gotNow)
					{
						gotPrevious = gotNow;

						C64Control::SetLowROM();
						C64Control::UpdateLatch();
						if (readDelayMicroSeconds > 0)
						{
							delayMicroseconds(readDelayMicroSeconds);
						}
						if (progressiveDelay >= 2)
						{
							delayMicroseconds(progressiveDelay / 2);
							maxDelayNeeded = std::max(maxDelayNeeded, progressiveDelay / 2);
						}
						gotNow = (unsigned char)GetInputByte();
						C64Control::ClearLowROM();
						C64Control::UpdateLatch();
						progressiveDelay++;
					}
					bankData[address] = (unsigned char)gotNow;
				}

				fwrite(bankData, sizeof(bankData[0]), sizeof(bankData), fp);
				//		printf("\nBank done\n");
			}
			fclose(fp);
			printf("maxDelayNeeded = %d\n", maxDelayNeeded);

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--ef3id") == 0)
		{
			argPos++;
			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			// Autoselect, the IDs are read from every programmer slot
			EF3FlashCommand(0x90);
			for (int programmerSlot : EF3ProgrammerSlots())
			{
				EF3SelectProgrammerSlot(programmerSlot);
				int manufacturer = GetDataROML(0);
				int device = GetDataROML(2);
				printf("Programmer slot %d: EasyFlash 3 flash manufacturer $%02x device $%02x, %s\n", programmerSlot + 1, manufacturer, device,
					manufacturer == 0xc2 && device == 0xcb ? "MX29LV640EB found" : "expected MX29LV640EB (manufacturer $c2 device $cb)");
			}
			// Back to read mode
			SetDataROMLWithPHI2Edge(0, 0xf0);

			// The slot register $de01 is read back before every write, so check that it works: write 0 to 7 and read it back
			std::vector<int> programmerSlots = EF3ProgrammerSlots();
			std::vector<std::string> readBack(8);
			std::vector<bool> slotRegisterWorks(8, true);
			for (int slot = 0; slot < 8; slot++)
			{
				SetDataIO1WithPHI2Edge(1, slot);
				for (int programmerSlot : programmerSlots)
				{
					EF3SelectProgrammerSlot(programmerSlot);
					int got = GetDataIO1(1);
					char text[8];
					snprintf(text, sizeof(text), " $%02x", got);
					readBack[programmerSlot] += text;
					if ((got & 0x07) != slot)
					{
						slotRegisterWorks[programmerSlot] = false;
					}
				}
			}
			SetDataIO1WithPHI2Edge(1, 0);
			for (int programmerSlot : programmerSlots)
			{
				printf("Programmer slot %d: slot register $de01 %s, writing 0 to 7 reads%s\n", programmerSlot + 1,
					slotRegisterWorks[programmerSlot] ? "works" : "DOES NOT WORK", readBack[programmerSlot].c_str());
			}
			EF3SelectProgrammerSlot(theActiveSlot);

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--writeef3slot") == 0)
		{
			argPos++;
			int slot = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			const char* filename = argv[argPos];
			argPos++;

			// Slot 0 holds the menu, the KERNALs and the freezers
			if (slot < 1 || slot > 7)
			{
				printf("Slot %d cannot be written, only slots 1 to 7\n", slot);
				continue;
			}

			static unsigned char slotData[kEF3SlotSize];
			if (!EF3LoadSlotFile(filename, slotData))
			{
				continue;
			}

			InterfaceControl::SetLED1();
			InterfaceControl::UpdateLatch();

			printf("Writing EasyFlash 3 slot %d\n", slot);
			bool gotError = !EF3CheckChips();
			bool anyChanged = false;

			// The slot has 16 sectors of 64KB, each one is 8 banks of either ROML or ROMH
			for (int sector = 0; sector < 16 && !gotError; sector++)
			{
				bool high = (sector & 1) != 0;
				auto target = [&](int bank) { return (const unsigned char*)slotData + (bank * 2 + (high ? 1 : 0)) * kEF3BankSize; };
				bool changed = false;
				bool skipped = false;
				gotError = !EF3WriteSector(slot, (sector / 2) * 8, 8, high, target, changed, skipped);
				anyChanged = anyChanged || changed;
			}

			if (gotError)
			{
				// Back to read mode
				SetDataROMLWithPHI2Edge(0, 0xf0);
				printf("Writing slot %d failed%s\n", slot, anyChanged ? "" : ", nothing was changed");
			}
			else
			{
				printf("Slot %d written and verified\n", slot);
			}

			InterfaceControl::ClearLED1();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--ef3slotname") == 0 || strcasecmp(argv[argPos], "--ef3kernalname") == 0 || strcasecmp(argv[argPos], "--ef3freezername") == 0)
		{
			const char* option = argv[argPos];
			argPos++;
			int number = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			const char* name = argv[argPos];
			argPos++;

			if (strcasecmp(option, "--ef3slotname") == 0)
			{
				if (number < 1 || number > 15)
				{
					printf("Slot %d has no name in the directory, only slots 1 to 15\n", number);
					continue;
				}
				EF3SetName(number, ("Slot " + std::to_string(number)).c_str(), name);
				continue;
			}

			EF3Area area;
			static unsigned char directory[kEF3BankSize];
			if (EF3GetArea(strcasecmp(option, "--ef3kernalname") == 0, number, area) && EF3ReadDirectory(directory) && EF3CheckFC3(directory, area))
			{
				EF3SetName(area.directoryIndex, (std::string(area.type) + " " + std::to_string(number)).c_str(), name);
			}

			continue;
		}

		if (strcasecmp(argv[argPos], "--ef3noconfirm") == 0)
		{
			argPos++;
			ef3NoConfirm = true;
			continue;
		}

		if (strcasecmp(argv[argPos], "--writeef3") == 0 || strcasecmp(argv[argPos], "--writeef3all") == 0)
		{
			bool all = strcasecmp(argv[argPos], "--writeef3all") == 0;
			argPos++;
			const char* filename = argv[argPos];
			argPos++;

			EF3WriteImage(filename, all);

			continue;
		}

		if (strcasecmp(argv[argPos], "--eraseef3slot") == 0 || strcasecmp(argv[argPos], "--eraseef3kernal") == 0 || strcasecmp(argv[argPos], "--eraseef3freezer") == 0)
		{
			const char* option = argv[argPos];
			argPos++;
			int number = std::stoi(argv[argPos], nullptr, 0);
			argPos++;

			std::vector<EF3Sector> sectors;
			std::vector<int> nameIndices;
			std::string what;
			if (strcasecmp(option, "--eraseef3slot") == 0)
			{
				// Slot 0 holds the menu, the KERNALs and the freezers
				if (number < 1 || number > 7)
				{
					printf("Slot %d cannot be erased, only slots 1 to 7\n", number);
					continue;
				}
				EF3AddSlotSectors(number, sectors);
				nameIndices.push_back(number);
				what = "erases slot " + std::to_string(number);
			}
			else
			{
				EF3Area area;
				static unsigned char directory[kEF3BankSize];
				if (!EF3GetArea(strcasecmp(option, "--eraseef3kernal") == 0, number, area) || !EF3ReadDirectory(directory) || !EF3CheckFC3(directory, area))
				{
					continue;
				}
				EF3AddAreaSectors(area, sectors);
				nameIndices.push_back(area.directoryIndex);
				what = "erases " + std::string(area.type) + " " + std::to_string(number) + " in slot 0 (system area)";
			}
			EF3Erase(what.c_str(), sectors, nameIndices, "yes");

			continue;
		}

		if (strcasecmp(argv[argPos], "--eraseef3all") == 0)
		{
			argPos++;

			// Like a new EasyFlash 3: everything except the EF3 menu and EasyProg
			std::vector<EF3Sector> sectors;
			std::vector<int> nameIndices;
			for (int slot = 1; slot < kEF3NumSlots; slot++)
			{
				EF3AddSlotSectors(slot, sectors);
			}
			// The FC3 freezer only with the EF3 menu 1.3.0 and newer, an older menu keeps its directory as it was
			static unsigned char directory[kEF3BankSize];
			bool fc3 = EF3ReadDirectory(directory) && EF3MenuHasFC3(directory);
			for (int pass = 0; pass < 2; pass++)
			{
				for (int number = 1; number <= (pass == 0 ? 8 : (fc3 ? 4 : 3)); number++)
				{
					EF3Area area;
					EF3GetArea(pass == 0, number, area);
					EF3AddAreaSectors(area, sectors);
				}
			}
			for (int index = 1; index <= (fc3 ? 27 : 26); index++)
			{
				nameIndices.push_back(index);
			}
			EF3Erase("erases slots 1 to 7, all KERNALs and all freezers, and sets all names back. The EF3 menu and EasyProg stay", sectors, nameIndices, "ERASE ALL");

			continue;
		}

		if (strcasecmp(argv[argPos], "--ef3resetnames") == 0)
		{
			argPos++;

			// Only the menu directory sector is rewritten, much faster than --eraseef3all
			std::vector<int> nameIndices;
			static unsigned char directory[kEF3BankSize];
			bool fc3 = EF3ReadDirectory(directory) && EF3MenuHasFC3(directory);
			for (int index = 1; index <= (fc3 ? 27 : 26); index++)
			{
				nameIndices.push_back(index);
			}
			EF3Erase("sets all names in the menu back. The slots, KERNALs and freezers keep their content, so the menu starts them as before", std::vector<EF3Sector>(), nameIndices, "yes");

			continue;
		}

		if (strcasecmp(argv[argPos], "--probeio1") == 0)
		{
			argPos++;
			ProbeIO1();
			continue;
		}

		if (strcasecmp(argv[argPos], "--ide64devices") == 0)
		{
			argPos++;
			if (!Ide64Devices())
			{
				sExitCode = 1;
			}
			continue;
		}

		if (strcasecmp(argv[argPos], "--ide64id") == 0)
		{
			argPos++;
			if (!Ide64ID())
			{
				sExitCode = 1;
			}
			continue;
		}

		if (strcasecmp(argv[argPos], "--readide64") == 0)
		{
			argPos++;
			if (!Ide64Read(argv[argPos]))
			{
				sExitCode = 1;
			}
			argPos++;
			continue;
		}

		if (strcasecmp(argv[argPos], "--readide64setup") == 0)
		{
			argPos++;
			if (!Ide64ReadSetup(argv[argPos]))
			{
				sExitCode = 1;
			}
			argPos++;
			continue;
		}

		// --showide64setup [setup file]: the setup by name, from the file or, without a file name (the next argument is
		// missing or an option), read from the IDE64
		if (strcasecmp(argv[argPos], "--showide64setup") == 0)
		{
			argPos++;
			const char* filename = nullptr;
			if (argPos < argc && argv[argPos][0] != '-')
			{
				filename = argv[argPos++];
			}
			if (!Ide64ShowSetupByName(filename))
			{
				sExitCode = 1;
			}
			continue;
		}

		if (strcasecmp(argv[argPos], "--setide64clock") == 0)
		{
			argPos++;
			if (!Ide64SetClock(argv[argPos]))
			{
				sExitCode = 1;
			}
			argPos++;
			continue;
		}

		if (strcasecmp(argv[argPos], "--writeide64setup") == 0)
		{
			argPos++;
			if (!Ide64WriteSetup(argv[argPos]))
			{
				sExitCode = 1;
			}
			argPos++;
			continue;
		}

		// The PGM jumper must be set when PiCartTool resets the IDE64 (at the start of this option) and removed before
		// the flash can be seen, so it waits for the button. --ide64nowait leaves this out (the jumper is handled otherwise).
		// --swapide64 [backup]: swaps the two IDEDOS halves of a 128 KB flash, after saving it. Without a file name (the
		// next argument is missing or an option) the backup is ide64_backup_<date>_<time>.bin in the current directory.
		// The PGM jumper as with --writeide64, --swapide64nowait does not wait for the button.
		if (strcasecmp(argv[argPos], "--swapide64") == 0 || strcasecmp(argv[argPos], "--swapide64nowait") == 0)
		{
			bool wait = strcasecmp(argv[argPos], "--swapide64") == 0;
			argPos++;
			std::string backup;
			if (argPos < argc && argv[argPos][0] != '-')
			{
				backup = argv[argPos++];
			}
			else
			{
				char name[64];
				time_t now = time(nullptr);
				strftime(name, sizeof(name), "ide64_backup_%Y%m%d_%H%M%S.bin", localtime(&now));
				backup = name;
			}
			if (!Ide64Swap(backup.c_str(), wait))
			{
				sExitCode = 1;
			}
			continue;
		}

		if (strcasecmp(argv[argPos], "--writeide64") == 0 || strcasecmp(argv[argPos], "--writeide64nowait") == 0)
		{
			bool wait = strcasecmp(argv[argPos], "--writeide64") == 0;
			argPos++;
			if (!Ide64Write(argv[argPos], wait))
			{
				sExitCode = 1;
			}
			argPos++;
			continue;
		}

		if (strcasecmp(argv[argPos], "--detect") == 0)
		{
			argPos++;
			Detect();
			continue;
		}

		if (strcasecmp(argv[argPos], "--probe") == 0)
		{
			argPos++;
			Probe();
			continue;
		}

		if (strcasecmp(argv[argPos], "--readef3slot") == 0)
		{
			argPos++;
			int slot = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			const char* filename = argv[argPos];
			argPos++;

			if (slot < 1 || slot > 7)
			{
				printf("Slot %d cannot be read as .crt, only slots 1 to 7\n", slot);
				continue;
			}
			EF3ReadSlot(slot, filename);

			continue;
		}

		if (strcasecmp(argv[argPos], "--readef3kernal") == 0 || strcasecmp(argv[argPos], "--readef3freezer") == 0 ||
			strcasecmp(argv[argPos], "--writeef3kernal") == 0 || strcasecmp(argv[argPos], "--writeef3freezer") == 0)
		{
			const char* option = argv[argPos];
			argPos++;
			int number = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			const char* filename = argv[argPos];
			argPos++;

			EF3Area area;
			if (!EF3GetArea(strstr(option, "kernal") != nullptr, number, area))
			{
				continue;
			}
			if (strncasecmp(option, "--read", 6) == 0)
			{
				EF3ReadArea(area, filename);
			}
			else
			{
				EF3WriteArea(area, filename);
			}

			continue;
		}

		if (strcasecmp(argv[argPos], "--readef3") == 0 || strcasecmp(argv[argPos], "-ref3") == 0)
		{
			argPos++;
			const char* filename = argv[argPos];
			argPos++;

			static unsigned char image[kEF3ImageSize];
			if (EF3ReadImage(image))
			{
				WriteFile(filename, image, kEF3ImageSize);
			}

			continue;
		}

		if (strcasecmp(argv[argPos], "--dumpef3") == 0)
		{
			argPos++;
			std::string folder = argv[argPos];
			argPos++;

			static unsigned char image[kEF3ImageSize];
			mkdir(folder.c_str(), 0755);
			if (EF3ReadImage(image) && WriteFile(folder + "/ef3.bin", image, kEF3ImageSize))
			{
				printf("%s/ef3.bin: whole EasyFlash 3\n", folder.c_str());
				EF3Extract(image, folder);
			}

			continue;
		}

		if (strcasecmp(argv[argPos], "--extractef3") == 0)
		{
			argPos++;
			const char* filename = argv[argPos];
			argPos++;
			std::string folder = argv[argPos];
			argPos++;

			static unsigned char image[kEF3ImageSize];
			fp = fopen(filename, "rb");
			if (!fp)
			{
				printf("Cannot open %s\n", filename);
				continue;
			}
			size_t numBytes = fread(image, 1, kEF3ImageSize, fp);
			bool tooLong = fgetc(fp) != EOF;
			fclose(fp);
			if (numBytes != (size_t)kEF3ImageSize || tooLong)
			{
				printf("%s has %d bytes, expected %d\n", filename, (int)numBytes, kEF3ImageSize);
				continue;
			}
			EF3Extract(image, folder);

			continue;
		}

		if (strcasecmp(argv[argPos], "--loop") == 0 || strcasecmp(argv[argPos], "-l") == 0)
		{
			argPos = 1;
			continue;
		}


		if (strcasecmp(argv[argPos], "--dump") == 0 || strcasecmp(argv[argPos], "-d") == 0)
		{
			argPos++;
			int address = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			int numBytes = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			int maxDelayNeeded = 0;
			for (int slot = 0; slot < activeSlots; slot++)
			{
				printf("Dumping slot %d\n", slot + 1);
				InterfaceControl::SetActiveSlot(slot);

				for (int bytes = 0; bytes < numBytes; bytes++)
				{
					int bank = (address + bytes) / sizeof(bankData);
					SetDataIO1(0, bank);
					SetDataIO2(bank >> 8);

					DataLatchOut::SetAddress(address + bytes);
					C64Control::ClearFlashWrite();
					C64Control::ClearDataLatchOut();

					// Tries two reads without any delay, then progressively increases the delay until we get two reads that are the same
					int gotPrevious = -1;
					int gotNow = -2;
					int progressiveDelay = 0;
					while (gotPrevious != gotNow)
					{
						gotPrevious = gotNow;

						C64Control::SetLowROM();
						C64Control::UpdateLatch();
						if (readDelayMicroSeconds > 0)
						{
							delayMicroseconds(readDelayMicroSeconds);
						}
						if (progressiveDelay >= 2)
						{
							delayMicroseconds(progressiveDelay / 2);
							maxDelayNeeded = std::max(maxDelayNeeded, progressiveDelay / 2);
						}
						gotNow = (unsigned char)GetInputByte();
						C64Control::ClearLowROM();
						C64Control::UpdateLatch();
						progressiveDelay++;
					}

					printf(" %02x ", gotNow);
					fflush(stdout);
				}
				printf("\n");
			}

			printf("maxDelayNeeded = %d\n", maxDelayNeeded);

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--dumpserial") == 0 || strcasecmp(argv[argPos], "-ds") == 0)
		{
			argPos++;
			int address = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			int numBytes = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			for (int slot = 0; slot < activeSlots; slot++)
			{
				printf("Dumping serial slot %d\n", slot + 1);
				InterfaceControl::SetActiveSlot(slot);

				SerialEEPROM_Init();

				// https://www.st.com/resource/en/datasheet/m93c76-r.pdf
				/*
				Chip: m93C86
				Page 8
				Each instruction is preceded by a rising edge on Chip Select Input (S) with Serial Clock (C) being held low.
					
				A start bit, which is the first '1' read on Serial Data Input (D) during the rising edge of Serial Clock (C).
					
				Two op-code bits, read on Serial Data Input (D) during the rising edge of Serial Clock (C). (Some
				instructions also use the first two bits of the address to define the op - code)
					
				The address bits of the byte or word that is to be accessed.
				For the M93C86, the address is made up of 10 bits for the x16 organization or 11 bits for the x8 organization
				Page 9 for M93C86
				READ Read Data from Memory 1 10 A10-A0 Q7-Q0
				*/


				// Reset instruction
				SerialEEPROM_Reset();
				// Start bit
				SerialEEPROM_SendBit(1);

				// Read command
				SerialEEPROM_SendBit(1);
				SerialEEPROM_SendBit(0);

				SerialEEPROM_SendAddress(address);

				// Skip dummy bit...
				SerialEEPROM_DoClock();

				for (int i = 0; i < numBytes; i++)
				{
					int byte = SerialEEPROM_ReadByte();
					printf("%02x ", byte);
					fflush(stdout);
				}

				printf("\n");
			}

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--readserial") == 0 || strcasecmp(argv[argPos], "-rs") == 0)
		{
			argPos++;
			fp = fopen(argv[argPos], "wb");
			argPos++;

			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			printf("Reading serial\n");

			SerialEEPROM_Init();

			// Reset instruction
			SerialEEPROM_Reset();
			// Start bit
			SerialEEPROM_SendBit(1);

			// Read command
			SerialEEPROM_SendBit(1);
			SerialEEPROM_SendBit(0);

			SerialEEPROM_SendAddress(0);

			// Skip dummy bit...
			SerialEEPROM_DoClock();

			for (int i = 0; i < (1<<kSerialEEPROM_AddressBits); i++)
			{
				if ((i & 0xff) == 0)
				{
					printf(".");
					fflush(stdout);
				}

				int byte = SerialEEPROM_ReadByte();
				fputc(byte, fp);
				byte = SerialEEPROM_ReadByte();
				fputc(byte, fp);
			}

			fclose(fp);
			printf("\nDone reading serial\n");

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--writebyte") == 0 || strcasecmp(argv[argPos], "-wb") == 0)
		{
			argPos++;
			int address = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			int byte = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			printf("Writing single byte $%x at $%x...\n" , byte , address);
			int maxDelayNeeded = 0;

			int bank = address / sizeof(bankData);
			SetDataIO1(0, bank);
			SetDataIO2(bank >> 8);

			SendChipCommandProgram();

			DataLatchOut::SetAddress(address);
			DataLatchOut::SetData(byte);

			PerformCartridgeWriteCycle();

			C64Control::SetRead();
			C64Control::ClearLowROM();
			C64Control::ClearHighROM();
			C64Control::UpdateLatch();

			for (int slot = 0; slot < activeSlots; slot++)
			{
				InterfaceControl::SetActiveSlot(slot);

				// Tries two reads without any delay, then progressively increases the delay until we get two reads that are the same
				int gotPrevious = -1;
				int gotNow = -2;
				int progressiveDelay = 0;
				while ((gotPrevious != gotNow) && (gotNow != byte))
				{
					gotPrevious = gotNow;

					C64Control::SetLowROM();
					C64Control::UpdateLatch();
					if (readDelayMicroSeconds > 0)
					{
						delayMicroseconds(readDelayMicroSeconds);
					}
					if (progressiveDelay >= 2)
					{
						delayMicroseconds(progressiveDelay / 2);
						maxDelayNeeded = std::max(maxDelayNeeded, progressiveDelay / 2);
					}
					if (progressiveDelay > 20)
					{
						printf("Error the read delay is too long, perhaps the flash write is failing...\n");
						break;
					}
					gotNow = (unsigned char)GetInputByte();
					C64Control::ClearLowROM();
					C64Control::UpdateLatch();
					progressiveDelay++;
				}

				InterfaceControl::SetActiveSlot(theActiveSlot);
			}

			printf("\n");
			printf("maxDelayNeeded = %d\n", maxDelayNeeded);

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--slots") == 0 || strcasecmp(argv[argPos], "-s") == 0)
		{
			argPos++;
			activeSlots = std::stoi(argv[argPos], nullptr, 0);
			argPos++;

			continue;
		}

		if (strcasecmp(argv[argPos], "--eraseserial") == 0 || strcasecmp(argv[argPos], "-es") == 0)
		{
			bool gotError = false;
			argPos++;

			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			InterfaceControl::SetActiveSlot(theActiveSlot);
			InterfaceControl::UpdateLatch();

			SerialEEPROM_Init();

			SerialEEPROM_SendWriteEnable();

			// Reset instruction
			SerialEEPROM_Reset();
			// Start bit
			SerialEEPROM_SendBit(1);

			// Erase command
			SerialEEPROM_SendBit(0);
			SerialEEPROM_SendBit(0);

			// Send expected address...
			SerialEEPROM_SendBit(1);
			SerialEEPROM_SendBit(0);
			for (int i = 0; i < kSerialEEPROM_AddressBits - 2; i++)
			{
				SerialEEPROM_SendBit(0);
			}

			SerialEEPROM_WaitForReadyAllslots(gotError);

			printf("\n");

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}


		if (strcasecmp(argv[argPos], "--writeserial") == 0 || strcasecmp(argv[argPos], "-ws") == 0)
		{
			bool gotError = false;
			argPos++;

			InterfaceControl::SetLED1();
			InterfaceControl::UpdateLatch();

			printf("Writing serial...\n");

			// Write some data to the flash, using the program command sequence
			fp = fopen(argv[argPos], "rb");
			argPos++;
			if (fp == 0)
			{
				printf("Error reading input file\n");
				continue;
			}

			size_t numBytes = fread(bankData, sizeof(bankData[0]), sizeof(bankData), fp);
			fclose(fp);

			if (numBytes == 0)
			{
				break;
			}

			printf("Got bytes %d\n", numBytes);

			InterfaceControl::SetActiveSlot(theActiveSlot);
			InterfaceControl::UpdateLatch();

			SerialEEPROM_Init();

			SerialEEPROM_SendWriteEnable();

			for (int address = 0; address < (int)numBytes; address += 2)
			{
				if ((address & 0xff) == 0)
				{
					printf(".");
					fflush(stdout);
				}

				// Reset instruction
				SerialEEPROM_Reset();
				// Start bit
				SerialEEPROM_SendBit(1);

				// Write command
				SerialEEPROM_SendBit(0);
				SerialEEPROM_SendBit(1);

				SerialEEPROM_SendAddress(address >> 1);

				SerialEEPROM_SendByte(bankData[address]);
				SerialEEPROM_SendByte(bankData[address+1]);

				SerialEEPROM_WaitForReadyAllslots(gotError);

				if (gotError)
				{
					break;
				}
			}

			printf("\nWrite serial done\n");

			InterfaceControl::SetActiveSlot(theActiveSlot);
			InterfaceControl::UpdateLatch();

			InterfaceControl::ClearLED1();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--activeslot") == 0 || strcasecmp(argv[argPos], "-as") == 0)
		{
			argPos++;
			theActiveSlot = std::stoi(argv[argPos], nullptr, 0);
			argPos++;

			continue;
		}


		printf("Unknown argument: %s\n", argv[argPos]);
		argPos++;
	}

	// Final power off
	InterfaceControl::ClearRelay1();
	InterfaceControl::UpdateLatch();

	auto end = std::chrono::steady_clock::now();

	// Calculate elapsed time as a double in seconds
	std::chrono::duration<double> elapsed = end - start;

	printf("Elapsed time: %f seconds\n" , elapsed.count());
	return sExitCode;
}
