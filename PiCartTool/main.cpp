#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wiringPi.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <string>
#include "Hardware.h"
#include "CartridgePort.h"
#include "CartridgeHardware.h"
#include "ProgrammerHardware.h"
#include <sys/stat.h>
#include <functional>
#include <vector>

// EasyFlash 3: 8 slots of 64 banks, each bank is 8KB ROML followed by 8KB ROMH, the same order as an EasyFlash .crt
static const int kEF3BankSize = 8192;
static const int kEF3NumBanks = 64;
static const int kEF3SlotSize = kEF3NumBanks * 2 * kEF3BankSize;
static const int kEF3NumSlots = 8;
static const int kEF3ImageSize = kEF3NumSlots * kEF3SlotSize;
// The EF3 menu directory in the system area (slot 0), see the Makefile of https://gitlab.com/easyflash/easyflash3-bootimage
static const int kEF3DirectoryBank = 0x10;
static const unsigned char kEF3DirectorySignature[] = { 0xc5, 0xc6, 0x2d, 0xc4, 0x49, 0x52, 0x45, 0x43, 0x54, 0x4f, 0x52, 0x59 };

static const unsigned char* EF3Chip(const unsigned char* image, int slot, int bank, bool high)
{
	return image + slot * kEF3SlotSize + (bank * 2 + (high ? 1 : 0)) * kEF3BankSize;
}

static bool IsEmpty(const unsigned char* data, int size)
{
	for (int i = 0; i < size; i++)
	{
		if (data[i] != 0xff)
		{
			return false;
		}
	}
	return true;
}

// Directory names are PETSCII with the upper/lower case character set
static std::string PETSCIIToASCII(const unsigned char* data, int size)
{
	std::string text;
	for (int i = 0; i < size && data[i]; i++)
	{
		unsigned char c = data[i];
		if (c >= 0xc1 && c <= 0xda)
		{
			c -= 0x80;
		}
		else if (c >= 0x41 && c <= 0x5a)
		{
			c += 0x20;
		}
		if (c >= 0x20 && c <= 0x7e)
		{
			text += (char)c;
		}
	}
	return text;
}

// For example "slot1_prince_of_persia.crt"
static std::string EF3FileName(const std::string& folder, const char* prefix, int index, const std::string& name, const char* extension)
{
	std::string cleanName;
	bool separator = false;
	for (char c : name)
	{
		c = (char)tolower((unsigned char)c);
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		{
			if (separator && !cleanName.empty())
			{
				cleanName += '_';
			}
			cleanName += c;
			separator = false;
		}
		else
		{
			separator = true;
		}
	}
	return folder + "/" + prefix + std::to_string(index) + "_" + cleanName + extension;
}

static bool WriteFile(const std::string& filename, const unsigned char* data, int size)
{
	FILE* fp = fopen(filename.c_str(), "wb");
	if (!fp)
	{
		printf("Cannot write %s\n", filename.c_str());
		return false;
	}
	fwrite(data, 1, size, fp);
	fclose(fp);
	return true;
}

static void WriteBigEndian(std::string& out, unsigned int value, int bytes)
{
	for (int i = bytes - 1; i >= 0; i--)
	{
		out += (char)((value >> (i * 8)) & 0xff);
	}
}

// EasyFlash .crt: hardware type 32, EXROM inactive, GAME active, chips that are completely $ff are left out
static bool EF3WriteCRT(const std::string& filename, const std::string& name, const unsigned char* slotData, int& numChips)
{
	std::string crt = "C64 CARTRIDGE   ";
	WriteBigEndian(crt, 0x40, 4);
	WriteBigEndian(crt, 0x0100, 2);
	WriteBigEndian(crt, 32, 2);
	crt += (char)1;
	crt += (char)0;
	crt.append(6, '\0');
	std::string crtName;
	for (char c : name)
	{
		crtName += (char)toupper((unsigned char)c);
	}
	crtName.resize(32, '\0');
	crt += crtName;

	numChips = 0;
	for (int bank = 0; bank < kEF3NumBanks; bank++)
	{
		for (int high = 0; high < 2; high++)
		{
			const unsigned char* data = EF3Chip(slotData, 0, bank, high == 1);
			if (IsEmpty(data, kEF3BankSize))
			{
				continue;
			}
			crt += "CHIP";
			WriteBigEndian(crt, 0x10 + kEF3BankSize, 4);
			WriteBigEndian(crt, 2, 2);
			WriteBigEndian(crt, bank, 2);
			WriteBigEndian(crt, high ? 0xa000 : 0x8000, 2);
			WriteBigEndian(crt, kEF3BankSize, 2);
			crt.append((const char*)data, kEF3BankSize);
			numChips++;
		}
	}
	if (numChips == 0)
	{
		return true;
	}
	return WriteFile(filename, (const unsigned char*)crt.data(), (int)crt.size());
}

// Parts of the system area (slot 0) that are read and written on their own, see the Makefile of easyflash3-bootimage
// The data of an area is stored bank by bank, 8KB ROML (if used) followed by 8KB ROMH (if used)
struct EF3Area
{
	const char* type;
	int number;
	int firstBank;
	int numBanks;
	bool low;
	bool high;
	int directoryIndex;
};

static int EF3AreaSize(const EF3Area& area)
{
	return area.numBanks * ((area.low ? 1 : 0) + (area.high ? 1 : 0)) * kEF3BankSize;
}

// KERNAL 1 to 8 are the 8KB boot sectors in banks 0 to 7 ROML
// Freezer 1 to 3 are Retro Replay, Action Replay and Super Snapshot 5, each one uses whole 64KB sectors
static bool EF3GetArea(bool kernal, int number, EF3Area& area)
{
	if (kernal)
	{
		if (number < 1 || number > 8)
		{
			printf("KERNAL %d does not exist, only 1 to 8\n", number);
			return false;
		}
		area = { "KERNAL", number, number - 1, 1, true, false, 16 + number - 1 };
		return true;
	}
	const EF3Area freezers[] = {
		{ "Freezer", 1, 0x10, 8, false, true, 24 },
		{ "Freezer", 2, 0x18, 8, false, true, 25 },
		{ "Freezer", 3, 0x20, 8, true, true, 26 },
	};
	if (number < 1 || number > 3)
	{
		printf("Freezer %d does not exist, only 1 (Retro Replay), 2 (Action Replay) and 3 (Super Snapshot 5)\n", number);
		return false;
	}
	area = freezers[number - 1];
	return true;
}

// Leaves out empty 8KB blocks at the end
static std::string EF3AreaFromImage(const unsigned char* image, const EF3Area& area)
{
	std::string data;
	for (int bank = area.firstBank; bank < area.firstBank + area.numBanks; bank++)
	{
		if (area.low)
		{
			data.append((const char*)EF3Chip(image, 0, bank, false), kEF3BankSize);
		}
		if (area.high)
		{
			data.append((const char*)EF3Chip(image, 0, bank, true), kEF3BankSize);
		}
	}
	while (!data.empty() && IsEmpty((const unsigned char*)data.data() + data.size() - kEF3BankSize, kEF3BankSize))
	{
		data.resize(data.size() - kEF3BankSize);
	}
	return data;
}

static std::string EF3DirectoryName(const unsigned char* directory, int index)
{
	// Entries of 16 bytes after the signature: 16 slots, 8 KERNALs, then the freezers
	return PETSCIIToASCII(directory + 16 + index * 16, 16);
}

// Writes the EasyFlash slots 1 to 7 as .crt, the KERNALs and the freezers as .bin, named after the EF3 menu directory
static bool EF3Extract(const unsigned char* image, const std::string& folder)
{
	const unsigned char* directory = EF3Chip(image, 0, kEF3DirectoryBank, false);
	if (memcmp(directory, kEF3DirectorySignature, sizeof(kEF3DirectorySignature)) != 0)
	{
		printf("No EF3 directory in bank $%02x of the system area\n", kEF3DirectoryBank);
		return false;
	}
	mkdir(folder.c_str(), 0755);

	for (int slot = 1; slot < kEF3NumSlots; slot++)
	{
		std::string name = EF3DirectoryName(directory, slot);
		std::string filename = EF3FileName(folder, "slot", slot, name, ".crt");
		int numChips = 0;
		if (!EF3WriteCRT(filename, name, image + slot * kEF3SlotSize, numChips))
		{
			return false;
		}
		if (numChips > 0)
		{
			printf("%s: %s, %d chips\n", filename.c_str(), name.c_str(), numChips);
		}
	}

	// First the KERNALs, then the freezers
	for (int pass = 0; pass < 2; pass++)
	{
		bool kernal = pass == 0;
		for (int number = 1; number <= (kernal ? 8 : 3); number++)
		{
			EF3Area area;
			EF3GetArea(kernal, number, area);
			std::string data = EF3AreaFromImage(image, area);
			if (data.empty())
			{
				continue;
			}
			std::string name = EF3DirectoryName(directory, area.directoryIndex);
			std::string filename = EF3FileName(folder, kernal ? "kernal" : "freezer", number, name, ".bin");
			if (!WriteFile(filename, (const unsigned char*)data.data(), (int)data.size()))
			{
				return false;
			}
			printf("%s: %s, %d bytes\n", filename.c_str(), name.c_str(), (int)data.size());
		}
	}

	return true;
}

// Loads an EasyFlash .crt or a raw slot file (up to 1MB in the order of one slot of --readef3), unused bytes are $ff
static bool EF3LoadSlotFile(const char* filename, unsigned char* slotData)
{
	memset(slotData, 0xff, kEF3SlotSize);
	FILE* fp = fopen(filename, "rb");
	if (!fp)
	{
		printf("Cannot open %s\n", filename);
		return false;
	}
	std::string file;
	char buffer[65536];
	size_t got;
	while ((got = fread(buffer, 1, sizeof(buffer), fp)) > 0)
	{
		file.append(buffer, got);
	}
	fclose(fp);
	const unsigned char* data = (const unsigned char*)file.data();
	auto readBigEndian = [&](size_t offset, int bytes) { unsigned int value = 0; for (int i = 0; i < bytes; i++) value = (value << 8) | data[offset + i]; return value; };

	if (file.compare(0, 16, "C64 CARTRIDGE   ") != 0)
	{
		if (file.size() > (size_t)kEF3SlotSize)
		{
			printf("%s has %d bytes, a slot has at most %d\n", filename, (int)file.size(), kEF3SlotSize);
			return false;
		}
		memcpy(slotData, data, file.size());
		printf("%s: raw slot file, %d bytes\n", filename, (int)file.size());
		return true;
	}

	if (file.size() < 0x40 || readBigEndian(0x16, 2) != 32)
	{
		printf("%s is not an EasyFlash .crt (hardware type 32)\n", filename);
		return false;
	}
	size_t offset = readBigEndian(0x10, 4);
	int numChips = 0;
	while (offset < file.size())
	{
		if (offset + 16 > file.size() || file.compare(offset, 4, "CHIP") != 0)
		{
			printf("%s: no CHIP packet at offset $%x\n", filename, (int)offset);
			return false;
		}
		unsigned int packetLength = readBigEndian(offset + 4, 4);
		unsigned int bank = readBigEndian(offset + 10, 2);
		unsigned int load = readBigEndian(offset + 12, 2);
		unsigned int size = readBigEndian(offset + 14, 2);
		int high;
		// 16KB chips at $8000 hold ROML and ROMH
		if (load == 0x8000 && (size == (unsigned int)kEF3BankSize || size == 2 * (unsigned int)kEF3BankSize))
		{
			high = 0;
		}
		else if ((load == 0xa000 || load == 0xe000) && size == (unsigned int)kEF3BankSize)
		{
			high = 1;
		}
		else
		{
			printf("%s: unsupported CHIP packet, bank %d, load $%04x, size $%x\n", filename, bank, load, size);
			return false;
		}
		if (bank >= (unsigned int)kEF3NumBanks || offset + 16 + size > file.size() || packetLength < 16 + size)
		{
			printf("%s: invalid CHIP packet at offset $%x\n", filename, (int)offset);
			return false;
		}
		memcpy(slotData + (bank * 2 + high) * kEF3BankSize, data + offset + 16, size);
		offset += packetLength;
		numChips++;
	}
	printf("%s: EasyFlash .crt \"%s\", %d chips\n", filename, file.substr(0x20, 32).c_str(), numChips);
	return true;
}

// Reads 8KB of ROML or ROMH from the selected slot and bank. ROML or ROMH stays active for the whole 8KB, because the
// CPLD decodes reads asynchronously, so only the address changes from byte to byte.
static void EF3ReadCurrentChip(bool high, unsigned char* data, int& maxDelayNeeded)
{
	C64Control::ClearDataLatchOut();
	C64Control::SetRead();
	if (high)
	{
		C64Control::SetHighROM();
	}
	else
	{
		C64Control::SetLowROM();
	}
	C64Control::UpdateLatch();

	for (int address = 0; address < kEF3BankSize; address++)
	{
		DataLatchOut::SetAddress(address);
		// The EasyFlash 3 needs more time until D0 is valid, without this delay it reads as 1 in most banks
		delayMicroseconds(1);
		// Two reads must be the same, otherwise the delay is increased progressively
		int gotPrevious = GetInputByte();
		int gotNow = GetInputByte();
		int progressiveDelay = 0;
		while (gotPrevious != gotNow)
		{
			progressiveDelay++;
			maxDelayNeeded = std::max(maxDelayNeeded, progressiveDelay);
			delayMicroseconds(progressiveDelay);
			gotPrevious = gotNow;
			gotNow = GetInputByte();
		}
		data[address] = (unsigned char)gotNow;
	}

	C64Control::ClearLowROM();
	C64Control::ClearHighROM();
	C64Control::UpdateLatch();
}

// Reads all 8 slots of an EasyFlash 3
static bool EF3ReadImage(unsigned char* image)
{
	InterfaceControl::SetLED2();
	InterfaceControl::UpdateLatch();

	printf("Reading EasyFlash 3...\n");
	int maxDelayNeeded = 0;
	bool gotError = false;
	for (int slot = 0; slot < kEF3NumSlots && !gotError; slot++)
	{
		printf("Slot %d\n", slot);
		SetDataIO1WithPHI2Edge(1, slot);
		int gotSlot = GetDataIO1(1) & 0x07;
		if (gotSlot != slot)
		{
			// One wrong read was seen once on the hardware, so the register is written and read once more
			SetDataIO1WithPHI2Edge(1, slot);
			gotSlot = GetDataIO1(1) & 0x07;
		}
		if (gotSlot != slot)
		{
			printf("Slot register $de01 reads %d instead of %d\n", gotSlot, slot);
			gotError = true;
			break;
		}

		for (int bank = 0; bank < kEF3NumBanks; bank++)
		{
			SetDataIO1WithPHI2Edge(0, bank);
			for (int chip = 0; chip < 2; chip++)
			{
				EF3ReadCurrentChip(chip == 1, image + slot * kEF3SlotSize + (bank * 2 + chip) * kEF3BankSize, maxDelayNeeded);
			}
		}
	}
	printf("maxDelayNeeded = %d\n", maxDelayNeeded);

	InterfaceControl::ClearLED2();
	InterfaceControl::UpdateLatch();

	return !gotError;
}

// Reads 8KB of ROML or ROMH from a bank of the selected EF3 slot, from the selected programmer slot
static void EF3ReadChip(int bank, bool high, unsigned char* data)
{
	SetDataIO1WithPHI2Edge(0, bank);
	int maxDelayNeeded = 0;
	EF3ReadCurrentChip(high, data, maxDelayNeeded);
}

// Reads the menu directory of the selected programmer slot
static bool EF3ReadDirectory(unsigned char* directory)
{
	if (!EF3SetSlotAndBank(0, kEF3DirectoryBank))
	{
		printf("Slot register $de01 does not read 0\n");
		return false;
	}
	EF3ReadChip(kEF3DirectoryBank, false, directory);
	if (memcmp(directory, kEF3DirectorySignature, sizeof(kEF3DirectorySignature)) != 0)
	{
		printf("No EF3 directory in bank $%02x\n", kEF3DirectoryBank);
		return false;
	}
	return true;
}

// The programmer slots with an EasyFlash 3 that is written: slot 1 to n of --slots n, or only the slot of --activeslot.
// All slots get every write at the same time, so all of them get the same data. Reads come from one slot at a time.
static std::vector<int> EF3ProgrammerSlots(void)
{
	std::vector<int> slots;
	if (activeSlots > 1)
	{
		for (int slot = 0; slot < activeSlots; slot++)
		{
			slots.push_back(slot);
		}
	}
	else
	{
		slots.push_back(theActiveSlot);
	}
	return slots;
}

static void EF3SelectProgrammerSlot(int programmerSlot)
{
	InterfaceControl::SetActiveSlot(programmerSlot);
	InterfaceControl::UpdateLatch();
}

// The programmer sends every write to all its slots, but only the slots of EF3ProgrammerSlots() are checked.
// So nothing is written while a cartridge is in another slot: an empty slot reads the flash ID $ff $ff.
// Without --slots, another EF3 with the same content only gives a note, because a programmer with one slot reads
// the same cartridge in every slot. The content is compared with the menu directory and bank 0 ROML of every EF3 slot.
static bool EF3CheckOtherProgrammerSlots(void)
{
	const int kMaxProgrammerSlots = 5;
	std::vector<int> programmerSlots = EF3ProgrammerSlots();
	auto inUse = [&](int programmerSlot) { return std::find(programmerSlots.begin(), programmerSlots.end(), programmerSlot) != programmerSlots.end(); };

	int manufacturer[kMaxProgrammerSlots];
	int device[kMaxProgrammerSlots];
	EF3FlashCommand(0x90);
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		EF3SelectProgrammerSlot(programmerSlot);
		manufacturer[programmerSlot] = GetDataROML(0);
		device[programmerSlot] = GetDataROML(2);
	}
	SetDataROMLWithPHI2Edge(0, 0xf0);
	EF3SelectProgrammerSlot(theActiveSlot);
	auto isEF3 = [&](int programmerSlot) { return manufacturer[programmerSlot] == 0xc2 && device[programmerSlot] == 0xcb; };

	// Only without --slots the content of another EF3 is compared, with the EF3 in the slot of --activeslot
	bool compare = activeSlots <= 1 && theActiveSlot < kMaxProgrammerSlots && isEF3(theActiveSlot);
	static unsigned char fingerprint[kMaxProgrammerSlots][kEF3NumSlots * kEF3BankSize];
	for (int slot = 0; slot < kEF3NumSlots && compare; slot++)
	{
		SetDataIO1WithPHI2Edge(1, slot);
		SetDataIO1WithPHI2Edge(0, slot == 0 ? kEF3DirectoryBank : 0);
		for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
		{
			if (!isEF3(programmerSlot))
			{
				continue;
			}
			EF3SelectProgrammerSlot(programmerSlot);
			int maxDelayNeeded = 0;
			EF3ReadCurrentChip(false, fingerprint[programmerSlot] + slot * kEF3BankSize, maxDelayNeeded);
		}
	}
	EF3SelectProgrammerSlot(theActiveSlot);

	bool refused = false;
	int sameContent = 0;
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		if (inUse(programmerSlot) || (manufacturer[programmerSlot] == 0xff && device[programmerSlot] == 0xff))
		{
			continue;
		}
		if (!isEF3(programmerSlot))
		{
			printf("Programmer slot %d holds another cartridge (flash ID $%02x $%02x). It would get the same writes.\n", programmerSlot + 1, manufacturer[programmerSlot], device[programmerSlot]);
			refused = true;
		}
		else if (!compare)
		{
			printf("Programmer slot %d holds an EasyFlash 3 that is not part of --slots. It would get the same writes without any check.\n", programmerSlot + 1);
			refused = true;
		}
		else if (memcmp(fingerprint[programmerSlot], fingerprint[theActiveSlot], sizeof(fingerprint[0])) != 0)
		{
			printf("Programmer slot %d holds another EasyFlash 3 with different content. It would get the same writes without any check.\n", programmerSlot + 1);
			refused = true;
		}
		else
		{
			sameContent++;
		}
	}
	if (refused)
	{
		printf("Take it out, or write all EasyFlash 3 with --slots. Nothing was written\n");
		return false;
	}
	if (sameContent > 0)
	{
		printf("Note: %d other programmer slot(s) read the same EasyFlash 3. With a one slot programmer this is normal; otherwise they get the same writes without any check, use --slots to check them too\n", sameContent);
	}
	return true;
}

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

	printf("Probing programmer slots 1 to %d, only the flash ID is read, nothing is erased or written\n", kMaxProgrammerSlots);
	std::string before[kMaxProgrammerSlots];
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		EF3SelectProgrammerSlot(programmerSlot);
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
		EF3SelectProgrammerSlot(theActiveSlot);
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
// Every write goes to all programmer slots, so each test is sent again for every slot and undone afterwards.
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

static void DetectReadROML(int programmerSlot, int count, unsigned char* data)
{
	EF3SelectProgrammerSlot(programmerSlot);
	for (int address = 0; address < count; address++)
	{
		data[address] = (unsigned char) GetDataROML(address);
	}
}

// Writes $de00 with all write methods of --probe. SetDataIO1() takes the data off the bus before IO1 ends, so a
// Magic Desk, which takes the value at the end of IO1, gets an open bus ($ff: turned off). The write with a PHI2 edge
// keeps the data until IO1 ends and comes last.
static void DetectSetIO1(int value)
{
	int savedDotClock = flashWriteToggleDotClockIterations;
	flashWriteToggleDotClockIterations = 0;
	SetDataIO1(0, value);
	flashWriteToggleDotClockIterations = 4;
	SetDataIO1(0, value);
	flashWriteToggleDotClockIterations = savedDotClock;
	SetDataIO1WithPHI2Edge(0, value);
}

static void Detect(void)
{
	const int kMaxProgrammerSlots = 5;
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

	printf("Detecting the cartridges in programmer slots 1 to %d, nothing is erased or written\n", kMaxProgrammerSlots);
	// Bank 0 like after a reset: the IO clear at the start leaves a Magic Desk turned off and a Megabyter in another bank
	DetectSetIO1(0);
	unsigned char before[kMaxProgrammerSlots][2][kDetectBytes];
	int game[kMaxProgrammerSlots], exrom[kMaxProgrammerSlots], io1[kMaxProgrammerSlots];
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		EF3SelectProgrammerSlot(programmerSlot);
		game[programmerSlot] = digitalRead(26);
		exrom[programmerSlot] = digitalRead(25);
		for (int address = 0; address < kDetectBytes; address++)
		{
			before[programmerSlot][0][address] = (unsigned char) GetDataROML(address);
			before[programmerSlot][1][address] = (unsigned char) GetDataROMH(address);
		}
		EF3SelectProgrammerSlot(programmerSlot);
		io1[programmerSlot] = GetDataIO1(0);
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

	flashWriteCommandIsHiROM = savedHigh;
	flashWriteToggleDotClockIterations = savedDotClock;
	RegisterDelayMicroSeconds = savedDelay;
	flashWriteCommandExtraClocking = savedExtra;

	// A Magic Desk has no flash ID: bank 1 in $de00 must show other data than bank 0. Bit 7 stays clear, it turns a
	// Magic Desk off.
	bool bankChanges[kMaxProgrammerSlots] = {};
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		if (found[programmerSlot] || game[programmerSlot] != 1 || exrom[programmerSlot] != 0)
		{
			continue;
		}
		DetectSetIO1(1);
		unsigned char data[kDetectBytes];
		DetectReadROML(programmerSlot, kDetectBytes, data);
		DetectSetIO1(0);
		bankChanges[programmerSlot] = memcmp(data, before[programmerSlot][0], kDetectBytes) != 0;
	}

	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		bool allEmpty = true;
		for (int i = 0; i < kDetectBytes; i++)
		{
			allEmpty = allEmpty && before[programmerSlot][0][i] == 0xff && before[programmerSlot][1][i] == 0xff;
		}
		printf("Programmer slot %d: ", programmerSlot + 1);
		if (found[programmerSlot])
		{
			printf("%s (flash ID $%02x $%02x, %s)\n", found[programmerSlot]->type, found[programmerSlot]->manufacturer, found[programmerSlot]->device, found[programmerSlot]->chip);
		}
		else if (bankChanges[programmerSlot])
		{
			printf("Magic Desk (no flash ID, the data changes with the bank in $de00)\n");
		}
		else if (!allEmpty && game[programmerSlot] == 1 && exrom[programmerSlot] == 0)
		{
			// A Magic Desk whose bank register does not take the writes of the programmer looks the same
			printf("8KB ROM cartridge without flash ID, for example a Magic Desk (the bank in $de00 does not change the data)\n");
		}
		else if (allEmpty && io1[programmerSlot] == 0xff && game[programmerSlot] == 1 && exrom[programmerSlot] == 1)
		{
			printf("empty\n");
		}
		else if (allEmpty && io1[programmerSlot] == 0xff)
		{
			printf("empty or no data (_GAME=%d _EXROM=%d)\n", game[programmerSlot], exrom[programmerSlot]);
		}
		else
		{
			printf("unknown (_GAME=%d _EXROM=%d, $de00 reads $%02x, ROML %02x %02x %02x %02x), --probe shows more\n", game[programmerSlot], exrom[programmerSlot], io1[programmerSlot], before[programmerSlot][0][0], before[programmerSlot][0][1], before[programmerSlot][0][2], before[programmerSlot][0][3]);
		}
	}

	// The cartridges must be back in read mode and in bank 0
	for (int programmerSlot = 0; programmerSlot < kMaxProgrammerSlots; programmerSlot++)
	{
		EF3SelectProgrammerSlot(programmerSlot);
		unsigned char after[2][kDetectBytes];
		for (int address = 0; address < kDetectBytes; address++)
		{
			after[0][address] = (unsigned char) GetDataROML(address);
			after[1][address] = (unsigned char) GetDataROMH(address);
		}
		if (memcmp(after, before[programmerSlot], sizeof(after)) != 0)
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
	EF3SelectProgrammerSlot(theActiveSlot);
}

// Reads the flash ID in every programmer slot before writing. Without an MX29LV640EB in all of them nothing is written.
static bool EF3CheckChips(void)
{
	if (!EF3CheckOtherProgrammerSlots())
	{
		return false;
	}
	EF3FlashCommand(0x90);
	bool allFound = true;
	for (int programmerSlot : EF3ProgrammerSlots())
	{
		EF3SelectProgrammerSlot(programmerSlot);
		int manufacturer = GetDataROML(0);
		int device = GetDataROML(2);
		if (manufacturer != 0xc2 || device != 0xcb)
		{
			printf("Programmer slot %d: no EasyFlash 3 found (flash manufacturer $%02x device $%02x, expected MX29LV640EB $c2 $cb)\n", programmerSlot + 1, manufacturer, device);
			allFound = false;
		}
	}
	SetDataROMLWithPHI2Edge(0, 0xf0);
	EF3SelectProgrammerSlot(theActiveSlot);
	if (!allFound)
	{
		printf("Nothing was written\n");
	}
	else if (activeSlots > 1)
	{
		printf("EasyFlash 3 found in programmer slots 1 to %d, all of them get the same data\n", activeSlots);
	}
	return allFound;
}

// Selects the EF3 slot and bank in all programmer slots and reads back the slot register of each one
static bool EF3SelectSlotAndBank(int slot, int bank)
{
	SetDataIO1WithPHI2Edge(1, slot);
	SetDataIO1WithPHI2Edge(0, bank);
	for (int programmerSlot : EF3ProgrammerSlots())
	{
		EF3SelectProgrammerSlot(programmerSlot);
		int gotSlot = GetDataIO1(1) & 0x07;
		if (gotSlot != slot)
		{
			// One wrong read was seen once on the hardware, so the register is written and read once more
			SetDataIO1WithPHI2Edge(1, slot);
			gotSlot = GetDataIO1(1) & 0x07;
		}
		if (gotSlot != slot)
		{
			printf("Programmer slot %d: slot register $de01 reads %d instead of %d\n", programmerSlot + 1, gotSlot, slot);
			return false;
		}
	}
	return true;
}

// Waits until the flash in every programmer slot reads the value
static bool EF3WaitForFlashAll(bool high, int address, int value, int timeoutMicroseconds, int& failedProgrammerSlot)
{
	for (int programmerSlot : EF3ProgrammerSlots())
	{
		EF3SelectProgrammerSlot(programmerSlot);
		if (!EF3WaitForFlash(high, address, value, timeoutMicroseconds))
		{
			failedProgrammerSlot = programmerSlot;
			return false;
		}
	}
	return true;
}

// Set with --ef3noconfirm, for scripts that write to the system area without a question
static bool ef3NoConfirm = false;

static bool AskConfirmation(const char* expected)
{
	if (ef3NoConfirm)
	{
		printf("No confirmation (--ef3noconfirm)\n");
		return true;
	}
	printf("Type %s to continue: ", expected);
	fflush(stdout);
	char answer[32] = { 0 };
	if (!fgets(answer, sizeof(answer), stdin) || strcmp(answer, (std::string(expected) + "\n").c_str()) != 0)
	{
		printf("Cancelled, nothing was changed\n");
		return false;
	}
	return true;
}

static bool AskYes(void)
{
	return AskConfirmation("yes");
}

// The bank register cannot be read back. Reading another bank of the slot first and then the target bank proves that
// the register changed when the content differs. Only then the erase and program commands go to this bank.
// This is done for every programmer slot, each one with its own content.
// In slots 1 to 7 a programmer slot where all 64 banks of this chip read $ff needs no proof: whatever bank is
// selected, the commands can only hit empty banks of the same slot, and the slot register is read back.
// Every reference must read $ff for this. Comparing with the target is not enough: if the switch to the target
// bank is lost, the target reads the same as each reference although the slot holds data.
static bool EF3SelectBankChecked(int slot, int bank, bool high)
{
	static unsigned char target[kEF3BankSize];
	static unsigned char other[8][kEF3BankSize];
	if (!EF3SelectSlotAndBank(slot, bank))
	{
		return false;
	}
	std::vector<int> unproven = EF3ProgrammerSlots();
	bool allReferencesEmpty[8];
	for (int programmerSlot = 0; programmerSlot < 8; programmerSlot++)
	{
		allReferencesEmpty[programmerSlot] = true;
	}
	for (int reference = 0; reference < kEF3NumBanks && !unproven.empty(); reference++)
	{
		if (reference == bank)
		{
			continue;
		}
		SetDataIO1WithPHI2Edge(0, reference);
		for (int programmerSlot : unproven)
		{
			EF3SelectProgrammerSlot(programmerSlot);
			int maxDelayNeeded = 0;
			EF3ReadCurrentChip(high, other[programmerSlot], maxDelayNeeded);
			allReferencesEmpty[programmerSlot] = allReferencesEmpty[programmerSlot] && IsEmpty(other[programmerSlot], kEF3BankSize);
		}
		SetDataIO1WithPHI2Edge(0, bank);
		std::vector<int> stillUnproven;
		for (int programmerSlot : unproven)
		{
			EF3SelectProgrammerSlot(programmerSlot);
			int maxDelayNeeded = 0;
			EF3ReadCurrentChip(high, target, maxDelayNeeded);
			if (memcmp(target, other[programmerSlot], kEF3BankSize) == 0)
			{
				stillUnproven.push_back(programmerSlot);
			}
		}
		unproven = stillUnproven;
	}
	bool allProven = true;
	for (int programmerSlot : unproven)
	{
		if (slot != 0 && allReferencesEmpty[programmerSlot])
		{
			printf("Programmer slot %d: all banks of slot %d %s are empty, the bank does not matter\n", programmerSlot + 1, slot, high ? "ROMH" : "ROML");
			continue;
		}
		printf("Programmer slot %d: cannot prove that bank $%02x %s of slot %d is selected: the bank register did not change or all banks have the same content\n", programmerSlot + 1, bank, high ? "ROMH" : "ROML", slot);
		allProven = false;
	}
	return allProven;
}

// Erases the sector of the selected bank. The commands stay in this bank, the flash ignores the upper address bits of the unlock cycles.
static bool EF3EraseSectorInCurrentBank(bool high)
{
	SetDataROMLWithPHI2Edge(0xaaa, 0xaa);
	SetDataROMLWithPHI2Edge(0x555, 0x55);
	SetDataROMLWithPHI2Edge(0xaaa, 0x80);
	SetDataROMLWithPHI2Edge(0xaaa, 0xaa);
	SetDataROMLWithPHI2Edge(0x555, 0x55);
	if (high)
	{
		SetDataROMHWithPHI2Edge(0, 0x30);
	}
	else
	{
		SetDataROMLWithPHI2Edge(0, 0x30);
	}
	int failedProgrammerSlot = 0;
	if (!EF3WaitForFlashAll(high, 0, 0xff, 30000000, failedProgrammerSlot))
	{
		printf("Programmer slot %d: timeout while erasing\n", failedProgrammerSlot + 1);
		return false;
	}
	return true;
}

// The unlock bypass mode of the flash needs only 2 instead of 4 writes per byte. If the flash does not support it,
// the first byte times out; then the normal program command is used for the rest of the run.
static bool ef3UnlockBypassWorks = true;
static bool ef3UnlockBypassReported = false;

// Programs one byte in the selected bank, in unlock bypass mode or with the normal program command
static bool EF3ProgramByte(bool high, int address, int value, bool bypass, int& failedProgrammerSlot)
{
	if (bypass)
	{
		// In bypass mode the program command may go to any address. The target address keeps the address latches unchanged.
		SetDataROMLWithPHI2Edge(address, 0xa0);
	}
	else
	{
		SetDataROMLWithPHI2Edge(0xaaa, 0xaa);
		SetDataROMLWithPHI2Edge(0x555, 0x55);
		SetDataROMLWithPHI2Edge(0xaaa, 0xa0);
	}
	if (high)
	{
		SetDataROMHWithPHI2Edge(address, value);
	}
	else
	{
		SetDataROMLWithPHI2Edge(address, value);
	}
	return EF3WaitForFlashAll(high, address, value, 10000, failedProgrammerSlot);
}

static void EF3EnterUnlockBypass(void)
{
	SetDataROMLWithPHI2Edge(0xaaa, 0xaa);
	SetDataROMLWithPHI2Edge(0x555, 0x55);
	SetDataROMLWithPHI2Edge(0xaaa, 0x20);
}

static void EF3ExitUnlockBypass(void)
{
	SetDataROMLWithPHI2Edge(0, 0x90);
	SetDataROMLWithPHI2Edge(0, 0x00);
	// Back to read mode, also when the flash ignored the bypass commands
	SetDataROMLWithPHI2Edge(0, 0xf0);
}

// Programs 8KB in the selected bank of all programmer slots and verifies each one, $ff bytes are skipped because they are erased already
static bool EF3ProgramInCurrentBank(bool high, const unsigned char* data)
{
	bool bypass = ef3UnlockBypassWorks;
	if (bypass)
	{
		EF3EnterUnlockBypass();
	}
	for (int address = 0; address < kEF3BankSize; address++)
	{
		if (data[address] == 0xff)
		{
			continue;
		}
		int failedProgrammerSlot = 0;
		if (EF3ProgramByte(high, address, data[address], bypass, failedProgrammerSlot))
		{
			if (bypass && !ef3UnlockBypassReported)
			{
				printf("Flash unlock bypass mode works\n");
				ef3UnlockBypassReported = true;
			}
			continue;
		}
		if (bypass && !ef3UnlockBypassReported)
		{
			// The very first byte in bypass mode failed: the flash does not support it, try again without
			printf("Flash unlock bypass mode does not work, using the normal program command\n");
			EF3ExitUnlockBypass();
			ef3UnlockBypassWorks = false;
			ef3UnlockBypassReported = true;
			bypass = false;
			if (EF3ProgramByte(high, address, data[address], false, failedProgrammerSlot))
			{
				continue;
			}
		}
		if (bypass)
		{
			EF3ExitUnlockBypass();
		}
		printf("Programmer slot %d: timeout while writing $%02x to %s $%04x\n", failedProgrammerSlot + 1, data[address], high ? "ROMH" : "ROML", address);
		return false;
	}
	if (bypass)
	{
		EF3ExitUnlockBypass();
	}
	static unsigned char got[kEF3BankSize];
	for (int programmerSlot : EF3ProgrammerSlots())
	{
		EF3SelectProgrammerSlot(programmerSlot);
		int maxDelayNeeded = 0;
		EF3ReadCurrentChip(high, got, maxDelayNeeded);
		for (int address = 0; address < kEF3BankSize; address++)
		{
			if (got[address] != data[address])
			{
				printf("Programmer slot %d: verify error at %s $%04x: read $%02x, expected $%02x\n", programmerSlot + 1, high ? "ROMH" : "ROML", address, got[address], data[address]);
				return false;
			}
		}
	}
	return true;
}

// Writes one sector: 8 banks of ROML or ROMH from firstBank, or in slot 0 one of the 8KB boot sectors (banks 0 to 7 ROML).
// The sector is read first in every programmer slot: if it holds the data in all of them it is skipped, if it is empty
// in all of them it is not erased. The bank switch is proven before erasing, in slot 0 also before writing.
// The slot register is read back after each switch. changed tells if the flash was touched, also when an error happened.
static bool EF3WriteSector(int slot, int firstBank, int numBanks, bool high, const std::function<const unsigned char*(int)>& target, bool& changed, bool& skipped)
{
	static unsigned char current[kEF3BankSize];
	int maxDelayNeeded = 0;
	changed = false;
	skipped = false;

	bool same = true;
	bool empty = true;
	for (int bank = firstBank; bank < firstBank + numBanks; bank++)
	{
		if (!EF3SelectSlotAndBank(slot, bank))
		{
			return false;
		}
		for (int programmerSlot : EF3ProgrammerSlots())
		{
			EF3SelectProgrammerSlot(programmerSlot);
			EF3ReadCurrentChip(high, current, maxDelayNeeded);
			same = same && memcmp(current, target(bank), kEF3BankSize) == 0;
			empty = empty && IsEmpty(current, kEF3BankSize);
		}
	}

	if (numBanks == 1)
	{
		printf("Slot %d, bank $%02x %s: ", slot, firstBank, high ? "ROMH" : "ROML");
	}
	else
	{
		printf("Slot %d, banks $%02x to $%02x %s: ", slot, firstBank, firstBank + numBanks - 1, high ? "ROMH" : "ROML");
	}
	if (same)
	{
		printf("unchanged\n");
		skipped = true;
		return true;
	}
	// Erasing writes $ff, then there is nothing to program
	bool targetEmpty = true;
	for (int bank = firstBank; bank < firstBank + numBanks; bank++)
	{
		targetEmpty = targetEmpty && IsEmpty(target(bank), kEF3BankSize);
	}
	printf("%s\n", empty ? "writing" : targetEmpty ? "erasing" : "erasing and writing");
	fflush(stdout);

	// An empty sector in slots 1 to 7 needs no erase, so no proof: all its banks may read the same
	if (slot == 0 || !empty)
	{
		if (!EF3SelectBankChecked(slot, firstBank, high))
		{
			return false;
		}
	}
	else if (!EF3SelectSlotAndBank(slot, firstBank))
	{
		return false;
	}

	if (!empty)
	{
		changed = true;
		if (!EF3EraseSectorInCurrentBank(high))
		{
			return false;
		}
	}

	for (int bank = firstBank; bank < firstBank + numBanks; bank++)
	{
		// The first bank is selected already, in slot 0 with the bank proof
		if (bank != firstBank && !EF3SelectSlotAndBank(slot, bank))
		{
			return false;
		}
		changed = true;
		if (!EF3ProgramInCurrentBank(high, target(bank)))
		{
			printf("Error in bank $%02x\n", bank);
			return false;
		}
	}
	return true;
}

// A directory entry has 16 bytes, longer names would not fit and other characters have no PETSCII equivalent
static bool EF3NameValid(const char* name)
{
	bool valid = strlen(name) >= 1 && strlen(name) <= 16;
	for (int i = 0; name[i]; i++)
	{
		if ((unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7e)
		{
			valid = false;
		}
	}
	if (!valid)
	{
		printf("Name \"%s\" is not allowed: 1 to 16 characters, only ASCII letters, digits, space and punctuation (no umlauts)\n", name);
	}
	return valid;
}

// Reads the menu directory sector (banks $10 to $17 ROML of slot 0) of every programmer slot. Refuses unless only bank $10
// is used, it starts with the signature and it is the same in all programmer slots, because all of them get the same
// directory written. Backwards, so bank $10 stays selected for the flash commands; the signature proves that.
static bool EF3ReadDirectorySector(unsigned char* directory)
{
	static unsigned char chip[kEF3BankSize];
	bool gotError = false;
	for (int bank = kEF3DirectoryBank + 7; bank >= kEF3DirectoryBank && !gotError; bank--)
	{
		if (!EF3SelectSlotAndBank(0, bank))
		{
			gotError = true;
			break;
		}
		bool first = true;
		for (int programmerSlot : EF3ProgrammerSlots())
		{
			EF3SelectProgrammerSlot(programmerSlot);
			int maxDelayNeeded = 0;
			EF3ReadCurrentChip(false, chip, maxDelayNeeded);
			if (bank != kEF3DirectoryBank)
			{
				if (!IsEmpty(chip, kEF3BankSize))
				{
					printf("Programmer slot %d: bank $%02x is not empty, the directory sector holds more than the directory\n", programmerSlot + 1, bank);
					gotError = true;
					break;
				}
			}
			// "EF-Directory" in PETSCII
			else if (memcmp(chip, kEF3DirectorySignature, sizeof(kEF3DirectorySignature)) != 0)
			{
				printf("Programmer slot %d: no EF3 directory in bank $%02x\n", programmerSlot + 1, kEF3DirectoryBank);
				gotError = true;
				break;
			}
			else if (!first && memcmp(chip, directory, kEF3BankSize) != 0)
			{
				printf("Programmer slot %d: the menu directory differs from programmer slot %d, use one EasyFlash 3 at a time with --activeslot\n", programmerSlot + 1, EF3ProgrammerSlots()[0] + 1);
				gotError = true;
				break;
			}
			else
			{
				memcpy(directory, chip, kEF3BankSize);
			}
			first = false;
		}
	}
	EF3SelectProgrammerSlot(theActiveSlot);
	return !gotError;
}

// Directory names are PETSCII with the upper/lower case character set, padded with $00
static void EF3SetDirectoryName(unsigned char* directory, int index, const std::string& name)
{
	unsigned char* entry = directory + 16 + index * 16;
	memset(entry, 0, 16);
	for (int i = 0; i < 16 && i < (int)name.size(); i++)
	{
		unsigned char c = (unsigned char)name[i];
		if (c >= 'A' && c <= 'Z')
		{
			c += 0x80;
		}
		else if (c >= 'a' && c <= 'z')
		{
			c -= 0x20;
		}
		entry[i] = c;
	}
}

// Sets a name in the EF3 menu directory: slots 1 to 15 at index 1 to 15, KERNALs at 16 to 23, freezers at 24 to 26
static void EF3SetName(int index, const char* label, const char* name)
{
	if (!EF3NameValid(name))
	{
		return;
	}

	InterfaceControl::SetLED1();
	InterfaceControl::UpdateLatch();

	static unsigned char directory[kEF3BankSize];
	bool gotError = !EF3CheckChips() || !EF3ReadDirectorySector(directory);

	bool erased = false;
	bool cancelled = false;
	if (!gotError)
	{
		printf("WARNING: This erases and rewrites the EF3 menu directory in slot 0 (system area)\n");
		printf("%s: \"%s\" -> \"%s\"\n", label, PETSCIIToASCII(directory + 16 + index * 16, 16).c_str(), name);
		if (!AskYes())
		{
			cancelled = true;
			gotError = true;
		}
	}

	if (!gotError)
	{
		EF3SetDirectoryName(directory, index, name);
		printf("Setting the name of %s to %s\n", label, name);
		erased = true;
		gotError = !EF3EraseSectorInCurrentBank(false) || !EF3ProgramInCurrentBank(false, directory);
	}

	if (gotError && !cancelled)
	{
		// Back to read mode
		SetDataROMLWithPHI2Edge(0, 0xf0);
		printf("Setting the name failed%s\n", erased ? ", the directory may be incomplete" : ", nothing was changed");
	}
	else if (!gotError)
	{
		printf("Name written and verified\n");
	}

	InterfaceControl::ClearLED1();
	InterfaceControl::UpdateLatch();
}

// Reads a KERNAL or freezer, empty 8KB blocks at the end are left out
static void EF3ReadArea(const EF3Area& area, const char* filename)
{
	InterfaceControl::SetLED2();
	InterfaceControl::UpdateLatch();

	static unsigned char directory[kEF3BankSize];
	static unsigned char chip[kEF3BankSize];
	if (EF3ReadDirectory(directory))
	{
		std::string data;
		for (int bank = area.firstBank; bank < area.firstBank + area.numBanks; bank++)
		{
			for (int high = 0; high < 2; high++)
			{
				if ((high == 0 && area.low) || (high == 1 && area.high))
				{
					EF3ReadChip(bank, high == 1, chip);
					data.append((const char*)chip, kEF3BankSize);
				}
			}
		}
		while (!data.empty() && IsEmpty((const unsigned char*)data.data() + data.size() - kEF3BankSize, kEF3BankSize))
		{
			data.resize(data.size() - kEF3BankSize);
		}
		std::string name = EF3DirectoryName(directory, area.directoryIndex);
		if (data.empty())
		{
			printf("%s %d \"%s\" is empty, nothing written\n", area.type, area.number, name.c_str());
		}
		else if (WriteFile(filename, (const unsigned char*)data.data(), (int)data.size()))
		{
			printf("%s: %s %d \"%s\", %d bytes\n", filename, area.type, area.number, name.c_str(), (int)data.size());
		}
	}

	InterfaceControl::ClearLED2();
	InterfaceControl::UpdateLatch();
}

// Writes a KERNAL or freezer in the system area. Only the sectors of the area are erased: the 8KB boot sector of a KERNAL,
// or the 64KB sectors of a freezer. The menu name is not changed.
static void EF3WriteArea(const EF3Area& area, const char* filename)
{
	const int areaSize = EF3AreaSize(area);
	static unsigned char areaData[8 * 2 * kEF3BankSize];
	memset(areaData, 0xff, sizeof(areaData));
	FILE* fp = fopen(filename, "rb");
	if (!fp)
	{
		printf("Cannot open %s\n", filename);
		return;
	}
	size_t numBytes = fread(areaData, 1, areaSize, fp);
	bool tooLong = fgetc(fp) != EOF;
	fclose(fp);
	if (tooLong || numBytes == 0)
	{
		printf("%s does not fit: %s %d has %d bytes\n", filename, area.type, area.number, areaSize);
		return;
	}

	InterfaceControl::SetLED1();
	InterfaceControl::UpdateLatch();

	// The layout of the system area is only known with a menu directory, in every programmer slot
	static unsigned char directory[kEF3BankSize];
	bool gotError = !EF3CheckChips();
	std::vector<int> programmerSlots = EF3ProgrammerSlots();
	for (int i = (int)programmerSlots.size() - 1; i >= 0 && !gotError; i--)
	{
		EF3SelectProgrammerSlot(programmerSlots[i]);
		if (!EF3ReadDirectory(directory))
		{
			printf("Programmer slot %d: nothing was written\n", programmerSlots[i] + 1);
			gotError = true;
		}
	}
	EF3SelectProgrammerSlot(theActiveSlot);
	bool started = false;
	bool erased = false;
	if (!gotError)
	{
		printf("WARNING: This erases and rewrites %s %d \"%s\" in slot 0 (system area) with %s (%d bytes)\n", area.type, area.number, EF3DirectoryName(directory, area.directoryIndex).c_str(), filename, (int)numBytes);
		printf("The name in the menu is not changed\n");
		gotError = !AskYes();
		started = !gotError;
	}

	// Each chip of the area is one sector (or one boot sector for a KERNAL): ROML and ROMH separately
	const int chipsPerBank = (area.low ? 1 : 0) + (area.high ? 1 : 0);
	for (int high = 0; high < 2 && !gotError; high++)
	{
		if ((high == 0 && !area.low) || (high == 1 && !area.high))
		{
			continue;
		}
		auto target = [&](int bank) { return (const unsigned char*)areaData + ((bank - area.firstBank) * chipsPerBank + (high == 1 && area.low ? 1 : 0)) * kEF3BankSize; };
		bool changed = false;
		bool skipped = false;
		gotError = !EF3WriteSector(0, area.firstBank, area.numBanks, high == 1, target, changed, skipped);
		erased = erased || changed;
	}

	if (started && gotError)
	{
		// Back to read mode
		SetDataROMLWithPHI2Edge(0, 0xf0);
		printf("Writing %s %d failed%s\n", area.type, area.number, erased ? "" : ", nothing was changed");
	}
	else if (started)
	{
		printf("%s %d written and verified\n", area.type, area.number);
	}

	InterfaceControl::ClearLED1();
	InterfaceControl::UpdateLatch();
}

// One sector: 8 banks of ROML or ROMH from firstBank, or in slot 0 one of the 8KB boot sectors (numBanks 1)
struct EF3Sector
{
	int slot;
	int firstBank;
	int numBanks;
	bool high;
};

// The names that EasyProg uses for empty entries: "Slot n" (checkEraseSlot() in write.c), "KERNAL n" (slotsFillEFDir()
// in slots.c). For the freezers the tested cartridges show "SS5 Slot" and "FC3 Slot" for unused entries.
static std::string EF3DefaultName(int index)
{
	if (index >= 1 && index <= 15)
	{
		return "Slot " + std::to_string(index);
	}
	if (index >= 16 && index <= 23)
	{
		return "KERNAL " + std::to_string(index - 15);
	}
	const char* freezers[] = { "RR Slot", "AR Slot", "SS5 Slot", "FC3 Slot" };
	return freezers[index - 24];
}

static void EF3AddAreaSectors(const EF3Area& area, std::vector<EF3Sector>& sectors)
{
	if (area.low)
	{
		sectors.push_back({ 0, area.firstBank, area.numBanks, false });
	}
	if (area.high)
	{
		sectors.push_back({ 0, area.firstBank, area.numBanks, true });
	}
}

static void EF3AddSlotSectors(int slot, std::vector<EF3Sector>& sectors)
{
	for (int sector = 0; sector < 16; sector++)
	{
		sectors.push_back({ slot, (sector / 2) * 8, 8, (sector & 1) != 0 });
	}
}

// Erases sectors and sets the names of the erased entries back to the names EasyProg uses for empty entries.
// Sectors that are empty already are skipped, and the directory is only rewritten if a name changes.
static void EF3Erase(const char* what, const std::vector<EF3Sector>& sectors, const std::vector<int>& nameIndices, const char* confirmation)
{
	InterfaceControl::SetLED1();
	InterfaceControl::UpdateLatch();

	static unsigned char directory[kEF3BankSize];
	static unsigned char empty[kEF3BankSize];
	memset(empty, 0xff, sizeof(empty));
	// The directory is checked before anything is erased, because the names are set back at the end
	bool gotError = !EF3CheckChips() || !EF3ReadDirectorySector(directory);
	bool started = false;
	bool anyChanged = false;
	if (gotError)
	{
		printf("Nothing was erased\n");
	}
	else
	{
		printf("WARNING: This %s\n", what);
		for (int index : nameIndices)
		{
			std::string name = PETSCIIToASCII(directory + 16 + index * 16, 16);
			if (name != EF3DefaultName(index))
			{
				printf("Name \"%s\" -> \"%s\"\n", name.c_str(), EF3DefaultName(index).c_str());
			}
		}
		gotError = !AskConfirmation(confirmation);
		started = !gotError;
	}

	int numErased = 0;
	bool namesChanged = false;
	for (const EF3Sector& sector : sectors)
	{
		if (gotError)
		{
			break;
		}
		bool changed = false;
		bool skipped = false;
		gotError = !EF3WriteSector(sector.slot, sector.firstBank, sector.numBanks, sector.high, [&](int) { return (const unsigned char*)empty; }, changed, skipped);
		anyChanged = anyChanged || changed;
		numErased += skipped ? 0 : 1;
	}

	if (!gotError && !nameIndices.empty())
	{
		// Read again: this selects and proves bank $10 for the directory sector
		gotError = !EF3ReadDirectorySector(directory);
		static unsigned char newDirectory[kEF3BankSize];
		memcpy(newDirectory, directory, kEF3BankSize);
		for (int index : nameIndices)
		{
			EF3SetDirectoryName(newDirectory, index, EF3DefaultName(index));
		}
		namesChanged = memcmp(newDirectory, directory, kEF3BankSize) != 0;
		if (!gotError && namesChanged)
		{
			printf("Setting the names back\n");
			anyChanged = true;
			gotError = !EF3EraseSectorInCurrentBank(false) || !EF3ProgramInCurrentBank(false, newDirectory);
		}
	}

	if (started && gotError)
	{
		// Back to read mode
		SetDataROMLWithPHI2Edge(0, 0xf0);
		printf("%s failed%s\n", sectors.empty() ? "Setting the names back" : "Erasing", anyChanged ? "" : ", nothing was changed");
	}
	else if (started && sectors.empty())
	{
		printf("%s\n", namesChanged ? "Names set back and verified" : "All names are the defaults already, nothing was changed");
	}
	else if (started)
	{
		printf("Erased and verified: %d sectors erased, %d were empty already\n", numErased, (int)sectors.size() - numErased);
	}

	InterfaceControl::ClearLED1();
	InterfaceControl::UpdateLatch();
}

// Writes a whole image from --readef3. Slots 1 to 7 come first, then slot 0. Without all, only the known areas of slot 0
// are written: the KERNALs, the menu directory and the freezers. With all, also the EF3 menu and EasyProg.
static void EF3WriteImage(const char* filename, bool all)
{
	static unsigned char image[kEF3ImageSize];
	FILE* fp = fopen(filename, "rb");
	if (!fp)
	{
		printf("Cannot open %s\n", filename);
		return;
	}
	size_t numBytes = fread(image, 1, kEF3ImageSize, fp);
	bool tooLong = fgetc(fp) != EOF;
	fclose(fp);
	if (numBytes != (size_t)kEF3ImageSize || tooLong)
	{
		printf("%s has %d bytes, expected %d\n", filename, (int)numBytes, kEF3ImageSize);
		return;
	}
	if (memcmp(EF3Chip(image, 0, kEF3DirectoryBank, false), kEF3DirectorySignature, sizeof(kEF3DirectorySignature)) != 0)
	{
		printf("%s is no EasyFlash 3 image, there is no menu directory in bank $%02x\n", filename, kEF3DirectoryBank);
		return;
	}

	std::vector<EF3Sector> sectors;
	for (int slot = 1; slot < kEF3NumSlots; slot++)
	{
		for (int sector = 0; sector < 16; sector++)
		{
			sectors.push_back({ slot, (sector / 2) * 8, 8, (sector & 1) != 0 });
		}
	}
	if (all)
	{
		// Slot 0: the 8KB boot sectors in banks 0 to 7 ROML, then 64KB sectors
		for (int bank = 0; bank < 8; bank++)
		{
			sectors.push_back({ 0, bank, 1, false });
		}
		for (int sector = 1; sector < 16; sector++)
		{
			sectors.push_back({ 0, (sector / 2) * 8, 8, (sector & 1) != 0 });
		}
	}
	else
	{
		for (int number = 1; number <= 8; number++)
		{
			sectors.push_back({ 0, number - 1, 1, false });
		}
		sectors.push_back({ 0, kEF3DirectoryBank, 8, false });
		for (int number = 1; number <= 3; number++)
		{
			EF3Area area;
			EF3GetArea(false, number, area);
			if (area.low)
			{
				sectors.push_back({ 0, area.firstBank, area.numBanks, false });
			}
			if (area.high)
			{
				sectors.push_back({ 0, area.firstBank, area.numBanks, true });
			}
		}
	}

	if (!EF3CheckChips())
	{
		return;
	}

	if (all)
	{
		printf("WARNING: This writes ALL 8MB of the EasyFlash 3 from %s, including the EF3 menu and EasyProg in slot 0\n", filename);
		printf("If it is interrupted while the menu is written, the EF3 may not start its menu any more\n");
		printf("Sectors that already hold the data are skipped\n");
		if (!AskConfirmation("WRITE ALL"))
		{
			return;
		}
	}
	else
	{
		printf("WARNING: This writes slots 1 to 7, the KERNALs, the freezers and the menu directory (slot 0, system area) from %s\n", filename);
		printf("The EF3 menu and EasyProg are not written. Sectors that already hold the data are skipped\n");
		if (!AskYes())
		{
			return;
		}
	}

	InterfaceControl::SetLED1();
	InterfaceControl::UpdateLatch();

	int numWritten = 0;
	int numUnchanged = 0;
	bool anyChanged = false;
	bool gotError = false;
	for (const EF3Sector& sector : sectors)
	{
		auto target = [&](int bank) { return EF3Chip(image, sector.slot, bank, sector.high); };
		bool changed = false;
		bool skipped = false;
		gotError = !EF3WriteSector(sector.slot, sector.firstBank, sector.numBanks, sector.high, target, changed, skipped);
		anyChanged = anyChanged || changed;
		if (gotError)
		{
			break;
		}
		if (skipped)
		{
			numUnchanged++;
		}
		else
		{
			numWritten++;
		}
	}

	if (gotError)
	{
		// Back to read mode
		SetDataROMLWithPHI2Edge(0, 0xf0);
		printf("Writing the image failed%s\n", anyChanged ? "" : ", nothing was changed");
	}
	else
	{
		printf("Image written and verified: %d sectors written, %d unchanged\n", numWritten, numUnchanged);
	}

	InterfaceControl::ClearLED1();
	InterfaceControl::UpdateLatch();
}

// Reads one EasyFlash slot as .crt, named after the EF3 menu directory
static void EF3ReadSlot(int slot, const char* filename)
{
	InterfaceControl::SetLED2();
	InterfaceControl::UpdateLatch();

	static unsigned char directory[kEF3BankSize];
	static unsigned char slotData[kEF3SlotSize];
	if (EF3ReadDirectory(directory))
	{
		if (!EF3SetSlotAndBank(slot, 0))
		{
			printf("Slot register $de01 does not read %d\n", slot);
		}
		else
		{
			for (int bank = 0; bank < kEF3NumBanks; bank++)
			{
				for (int high = 0; high < 2; high++)
				{
					EF3ReadChip(bank, high == 1, slotData + (bank * 2 + high) * kEF3BankSize);
				}
			}
			std::string name = EF3DirectoryName(directory, slot);
			int numChips = 0;
			if (EF3WriteCRT(filename, name, slotData, numChips))
			{
				if (numChips == 0)
				{
					printf("Slot %d \"%s\" is empty, nothing written\n", slot, name.c_str());
				}
				else
				{
					printf("%s: slot %d \"%s\", %d chips\n", filename, slot, name.c_str(), numChips);
				}
			}
		}
	}

	InterfaceControl::ClearLED2();
	InterfaceControl::UpdateLatch();
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
			if (EF3GetArea(strcasecmp(option, "--ef3kernalname") == 0, number, area))
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
				if (!EF3GetArea(strcasecmp(option, "--eraseef3kernal") == 0, number, area))
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
			for (int pass = 0; pass < 2; pass++)
			{
				for (int number = 1; number <= (pass == 0 ? 8 : 3); number++)
				{
					EF3Area area;
					EF3GetArea(pass == 0, number, area);
					EF3AddAreaSectors(area, sectors);
				}
			}
			for (int index = 1; index <= 26; index++)
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
			for (int index = 1; index <= 26; index++)
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
	return 0;
}
