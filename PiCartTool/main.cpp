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

// A real IDE64 V4.1 needs more than the 1 us of GetDataROML() until its data is stable on the bus: with 1 us about one
// read of the whole flash in 30 had wrong bytes, with 5 us none in 160
static int Ide64ReadBus(int address, void (*select)(void), void (*deselect)(void))
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

static int Ide64ReadIO1(int address)
{
	return Ide64ReadBus(0xde00 | (address & 0xff), C64Control::SetIO1, C64Control::ClearIO1);
}

static int Ide64ReadROML(int address)
{
	return Ide64ReadBus(0x8000 | (address & 0x1fff), C64Control::SetLowROM, C64Control::ClearLowROM);
}

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

// ---------------------------------------------------------------------------------------------------------------------
// IDE64 V4.x firmware (--ide64id, --readide64, --writeide64), the same way as the Perom programmer 2.0a by Soci/Singular
// on a C64. The IDE64 decodes the whole address bus itself (MAME ide64.cpp, VICE ide64.c, Perom):
// - $DE32 reads the status: bit 5 V4.x, bits 2-4 the flash bank (A14-A16), bit 1 GAME, bit 0 EXROM
// - a write to $DE60-$DE67 selects the 16 KB flash bank by its address, the data is not used
// - a write to $DEFC-$DEFF sets the memory configuration: $DEFC 16K, $DEFD 8K, $DEFE Ultimax, $DEFF off
// - ROML ($8000-$9FFF) and ROMH ($A000-$BFFF) read the flash at bank * 16 KB + (address & $3FFF)
// - every write to $8000-$BFFF goes to the flash, without ROML or ROMH (a C64 writes into its RAM there). The flash
//   only takes writes when the PGM jumper was set at the last reset, and it is only seen when the jumper is removed again.
// All writes are C64 cycles: PHI2 rises with the address and the data on the bus, PHI2 falls before IO1 or the write
// ends. Every write goes to all programmer slots, so the other slots must be empty.
struct Ide64FlashChip
{
	const char* name;
	int manufacturer;
	int device;
	int size;
	bool pageWrite;		// 128 byte pages, no erase needed (Atmel, Winbond, SST29EE), otherwise byte program and sector erase
	int sectorSize;
};

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
// The exit code of PiCartTool: 1 when an IDE64 option failed
static int sExitCode = 0;

static void Ide64Cycle(int address, int value, void (*select)(void), void (*deselect)(void))
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

static void Ide64WriteIO1(int address, int value)
{
	Ide64Cycle(0xde00 | (address & 0xff), value, C64Control::SetIO1, C64Control::ClearIO1);
}

static int Ide64Status(void)
{
	return Ide64ReadIO1(0x32);
}

static void Ide64SelectBank(int bank)
{
	if (bank != sIde64Bank)
	{
		Ide64WriteIO1(0x60 + bank, 0);
		sIde64Bank = bank;
	}
}

// Reads a flash address, the bank is selected as needed. The 16K configuration maps the flash to $8000-$BFFF.
static int Ide64ReadFlash(int address)
{
	Ide64SelectBank(address >> 14);
	int offset = address & 0x3fff;
	return offset < 0x2000 ? Ide64ReadROML(offset)
		: Ide64ReadBus(0xa000 | (offset & 0x1fff), C64Control::SetHighROM, C64Control::ClearHighROM);
}

static void Ide64WriteFlash(int address, int value)
{
	Ide64SelectBank(address >> 14);
	Ide64Cycle(0x8000 | (address & 0x3fff), value, nullptr, nullptr);
}

static void Ide64FlashCommand(int command)
{
	Ide64WriteFlash(0x5555, 0xaa);
	Ide64WriteFlash(0x2aaa, 0x55);
	Ide64WriteFlash(0x5555, command);
}

// While the flash is busy, bit 6 toggles with every read. Done when three reads in a row give the same value (Perom).
static bool Ide64WaitReady(int address, int timeoutMilliseconds)
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
static int Ide64Detect(int& status)
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

static void Ide64Leave(void)
{
	Ide64SelectBank(0);
	Ide64WriteIO1(0xfd, 0);
	sIde64Bank = -1;
}

// The flash ID with $90, then with $80 $60 (Atmel). It must not be the normal data at $0000/$0001.
static const Ide64FlashChip* Ide64FlashID(int& manufacturer, int& device)
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

static void Ide64WaitForButton(const char* text)
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
static bool Ide64CheckOtherSlots(void)
{
	const int kMaxProgrammerSlots = 5;
	unsigned char active[16];
	EF3SelectProgrammerSlot(theActiveSlot);
	for (int i = 0; i < 16; i++)
	{
		active[i] = (unsigned char) GetDataROML(0x8000 | i);
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
static bool Ide64Prepare(int& banks, bool quiet = false)
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
static bool Ide64FlashHidden(int& status)
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
static void Ide64ClockCommand(int command)
{
	Ide64WriteIO1(0xfb, 0x02);
	for (int bit = 0; bit < 8; bit++)
	{
		Ide64WriteIO1(0x5f, (command >> bit) & 1);
	}
}

static void Ide64ClockRead(int command, unsigned char* data, int size)
{
	Ide64ClockCommand(command);
	for (int i = 0; i < size; i++)
	{
		int value = 0;
		for (int bit = 0; bit < 8; bit++)
		{
			value |= (Ide64ReadIO1(0x5f) & 1) << bit;
		}
		data[i] = (unsigned char) value;
	}
	Ide64WriteIO1(0xfb, 0);
}

static void Ide64ClockWrite(int command, const unsigned char* data, int size)
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

static std::string Ide64ClockText(const unsigned char* clock)
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

static void Ide64ShowClock(void)
{
	unsigned char clock[8];
	Ide64ClockRead(0xbf, clock, sizeof(clock));
	printf("Clock: %s\n", Ide64ClockText(clock).c_str());
}

static int Ide64Bcd(int value)
{
	return (value / 10) << 4 | (value % 10);
}

static int Ide64FromBcd(int value)
{
	return (value >> 4) * 10 + (value & 15);
}

// Sets the clock to "now" (the local time of the Pi) or "YYYY-MM-DD HH:MM:SS": write protect off ($8E $00), then the
// 7 clock registers and the write protect register in a burst ($BE, 24 hour mode, the clock runs), which turns the
// write protect on again. The clock is read back. The setup in the RAM is not changed.
static bool Ide64SetClock(const char* when)
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
		clock[0] = (unsigned char) Ide64Bcd(target.tm_sec);
		clock[1] = (unsigned char) Ide64Bcd(target.tm_min);
		clock[2] = (unsigned char) Ide64Bcd(target.tm_hour);
		clock[3] = (unsigned char) Ide64Bcd(target.tm_mday);
		clock[4] = (unsigned char) Ide64Bcd(target.tm_mon + 1);
		clock[5] = (unsigned char) (target.tm_wday + 1);
		clock[6] = (unsigned char) Ide64Bcd(target.tm_year - 100);
		clock[7] = 0x80;
		const unsigned char kProtectOff = 0x00;
		auto start = std::chrono::steady_clock::now();
		Ide64ClockWrite(0x8e, &kProtectOff, 1);
		Ide64ClockWrite(0xbe, clock, sizeof(clock));
		Ide64ClockRead(0xbf, readBack, sizeof(readBack));
		// The clock runs on: compare the read time with the written one plus the time since writing
		long elapsed = (long) std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
		struct tm got = {};
		got.tm_sec = Ide64FromBcd(readBack[0] & 0x7f);
		got.tm_min = Ide64FromBcd(readBack[1]);
		got.tm_hour = Ide64FromBcd(readBack[2] & 0x3f);
		got.tm_mday = Ide64FromBcd(readBack[3]);
		got.tm_mon = Ide64FromBcd(readBack[4]) - 1;
		got.tm_year = Ide64FromBcd(readBack[6]) + 100;
		got.tm_isdst = target.tm_isdst;
		long difference = (long) difftime(mktime(&got), written) - elapsed;
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
static const char* Ide64SetupVersion(const unsigned char* data)
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
static std::string Ide64FlashVersion(void)
{
	std::string text, previous;
	for (int reads = 0; reads < 10 && (reads < 2 || text != previous); reads++)
	{
		previous.swap(text);
		text.clear();
		for (int address = 0; address < 0x800; address++)
		{
			text += (char) Ide64ReadFlash(address);
		}
	}
	size_t at = text.find("IDEDOS V");
	return at == std::string::npos || at + 12 > text.size() ? "" : text.substr(at + 8, 4);
}

// Reads the RAM until two reads in a row are the same (see Ide64Prepare()), returns the number of reads or 0
static int Ide64ReadSetupBytes(unsigned char* data)
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

static void Ide64ShowSetup(const unsigned char* data)
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
static bool Ide64LoadSetupFile(const char* filename, unsigned char* data, const char* failText)
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

static const char* Ide64ColourName(int colour)
{
	static const char* const kNames[16] = { "Black", "White", "Red", "Cyan", "Purple", "Green", "Blue", "Yellow",
		"Orange", "Brown", "L.red", "D.gray", "Med.gray", "L.green", "L.blue", "L.gray" };
	return kNames[colour & 15];
}

// A drive number of bytes 26-30: 0 is none, otherwise the value + 7
static std::string Ide64DriveName(int value)
{
	return value ? std::to_string(value + 7) : "--";
}

// The setup by name, as the setup screens of IDEDOS show it. The layout is the same in 0.90 and 0.91 except for
// ShortBus I/O 2 (byte 4 bit 2, only 0.91) and the drive numbers in bytes 26-30 (see patches/picarttool-ide64.md).
static void Ide64ShowSetupNames(const unsigned char* data, const char* version)
{
	bool v091 = strcmp(version, "0.91") == 0;
	static const char* const kStartBoot[4] = { "Disabled", "Enabled", "Power on", "Always" };
	struct Option { const char* name; int byte; int bit; };
	static const Option kOptions[] = {
		{ "Floppy speeder", 3, 5 }, { "Set basic clock", 3, 4 }, { "Keyboard repeat", 3, 3 }, { "Lowercase chars", 3, 2 },
		{ "Use DOS wedge", 3, 1 }, { "C128 keyboard", 3, 0 }, { "CMD emulation", 4, 7 }, { "Function keys", 4, 6 },
		{ "Control stops", 4, 5 }, { "Blocks free hack", 4, 4 }, { "Fast detection", 4, 3 }, { "ShortBus I/O 2", 4, 2 } };
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
static bool Ide64ShowSetupByName(const char* filename)
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

static bool Ide64ReadSetup(const char* filename)
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
	if (!WriteFile(filename, data, (int) sizeof(data)))
	{
		return false;
	}
	printf("Read the 31 bytes of the IDE64 setup (DS1302 RAM) to %s (the same twice in a row, %d reads)\n", filename, reads);
	return true;
}

// Writes a setup file of --readide64setup back into the RAM of the DS1302, as IDEDOS saves its setup: write protect off
// ($8E $00), the 31 bytes in a burst ($FE), write protect on ($8E $80). The file must have a valid check byte of the
// IDEDOS version in the flash. The clock is not changed.
static bool Ide64WriteSetup(const char* filename)
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

static bool Ide64ID(void)
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
static int Ide64AtaRegister(int reg)
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
static int Ide64AtaWait(int timeoutMilliseconds)
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

static void Ide64AtaSelect(int device)
{
	Ide64WriteIO1(0x28 + device / 2, 0);
	Ide64WriteIO1(0x26, device % 2 ? 0xb0 : 0xa0);
}

// IDENTIFY DEVICE ($EC) or IDENTIFY PACKET DEVICE ($A1): false when the device aborts it or has no data (status DRQ)
static bool Ide64AtaIdentify(int device, int command, unsigned short* words)
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
		words[i] = (unsigned short) (Ide64ReadIO1(0x31) << 8 | low);
	}
	return true;
}

// A text of the IDENTIFY data: two characters per word, the first in the high byte
static std::string Ide64AtaText(const unsigned short* words, int first, int count)
{
	std::string text;
	for (int i = first; i < first + count; i++)
	{
		text += (char) (words[i] >> 8);
		text += (char) (words[i] & 0xff);
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
static std::string Ide64AtaName(const unsigned short* words, bool packet)
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
static std::vector<std::string> Ide64ScanDevices(bool& ok)
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
static bool Ide64Devices(void)
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
static bool Ide64ReadAll(int banks, std::vector<unsigned char>& data, int& reads)
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
			data[address] = (unsigned char) Ide64ReadFlash((int) address);
		}
		reads++;
	}
	return data == previous;
}

static bool Ide64Read(const char* filename)
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
	if (!WriteFile(filename, data.data(), (int) data.size()))
	{
		return false;
	}
	printf("Read %d KB of the IDE64 flash to %s (the same twice in a row, %d reads)\n", (int) data.size() / 1024, filename, reads);
	return true;
}

// Writes an image from flash address 0. Pages or sectors that are already the same are left out. Every page or sector
// is checked right after writing, and the whole image at the end. With prepare, the image is made after the IDE64 can
// be read (--swapide64), it returns false when nothing is to be written. With upperFirst the second half of the image
// is written before the first.
static bool Ide64WriteImage(std::vector<unsigned char> file, const char* filename, bool waitForJumper,
	const std::function<bool(int, std::vector<unsigned char>&)>& prepare = nullptr, bool upperFirst = false)
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
	if ((int) file.size() > flashSize)
	{
		printf("%s has %d KB, the flash of this IDE64 only %d KB. Nothing was written\n", filename, (int) file.size() / 1024, flashSize / 1024);
		Ide64Leave();
		return false;
	}
	printf("Writing %d KB to the %s\n", (int) file.size() / 1024, chip->name);
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
	int numBlocks = ((int) file.size() + blockSize - 1) / blockSize;
	for (int n = 0; n < numBlocks; n++)
	{
		int index = upperFirst ? (n + numBlocks / 2) % numBlocks : n;
		int block = index * blockSize;
		int size = std::min(blockSize, (int) file.size() - block);
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
	for (int address = 0; address < (int) file.size(); address++)
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

static bool Ide64Write(const char* filename, bool waitForJumper)
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
		printf("%s has %d bytes, an IDEDOS image is a multiple of 16 KB\n", filename, (int) file.size());
		return false;
	}
	return Ide64WriteImage(file, filename, waitForJumper);
}

// The IDE64 V4.1 always starts from the lower 64 KB of its 128 KB flash, the upper 64 KB can hold another IDEDOS (it
// has no switch for the IDEDOS bank). This swaps the two halves, so the other IDEDOS starts. The whole flash is read
// and saved to the backup file first. The upper half is written first: if the writing stops, the IDEDOS that started
// before is still in one half, and the other one in the backup file.
static bool Ide64Swap(const char* backupFile, bool waitForJumper)
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
		if (!WriteFile(backupFile, file.data(), (int) file.size()))
		{
			printf("Cannot write the backup %s. Nothing was written\n", backupFile);
			return false;
		}
		printf("Saved the whole flash (%d KB) to %s\n", (int) file.size() / 1024, backupFile);
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
					SendChipCommandProgram();

					// Program command4 (the actual byte)
					DataLatchOut::SetAddress(address);
					int theWriteValue = bankData[address];
					DataLatchOut::SetData(theWriteValue);

					PerformCartridgeWriteCycle();

					for (int slot = 0; slot < activeSlots; slot++)
					{
						InterfaceControl::SetActiveSlot(slot);

						int statusRegister = 0;
						int iterations = 0;
						do
						{
							if (gotError)
							{
								break;
							}

							C64Control::SetLowROM();
							C64Control::UpdateLatch();
							//				delay(0);	// Certainly more than the 20ns for a bus read
							if (iterations > 50)
							{
								delayMicroseconds(1);
							}
							// Without it the data polling of the DIY Gmod2 can read DQ7 (on D0) as done too early
							if (readDelayMicroSeconds > 0)
							{
								delayMicroseconds(readDelayMicroSeconds);
							}
							statusRegister = GetInputByte();
							C64Control::ClearLowROM();
							C64Control::UpdateLatch();
							if (iterations++ > 100)
							{
								printf("There seems to be a problem verifying the byte at address $%04x slot %d\n", address, slot+1);
								ReportCartridgeError();
								gotError = true;
								break;
							}
						} while (statusRegister != theWriteValue);
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
