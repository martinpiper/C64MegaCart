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

const unsigned char* EF3Chip(const unsigned char* image, int slot, int bank, bool high)
{
	return image + slot * kEF3SlotSize + (bank * 2 + (high ? 1 : 0)) * kEF3BankSize;
}

bool IsEmpty(const unsigned char* data, int size)
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
std::string PETSCIIToASCII(const unsigned char* data, int size)
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
std::string EF3FileName(const std::string& folder, const char* prefix, int index, const std::string& name, const char* extension)
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

bool WriteFile(const std::string& filename, const unsigned char* data, int size)
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

void WriteBigEndian(std::string& out, unsigned int value, int bytes)
{
	for (int i = bytes - 1; i >= 0; i--)
	{
		out += (char)((value >> (i * 8)) & 0xff);
	}
}

// EasyFlash .crt: hardware type 32, EXROM inactive, GAME active, chips that are completely $ff are left out
bool EF3WriteCRT(const std::string& filename, const std::string& name, const unsigned char* slotData, int& numChips)
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

int EF3AreaSize(const EF3Area& area)
{
	return area.numBanks * ((area.low ? 1 : 0) + (area.high ? 1 : 0)) * kEF3BankSize;
}

// KERNAL 1 to 8 are the 8KB boot sectors in banks 0 to 7 ROML
// Freezer 1 to 3 are Retro Replay, Action Replay and Super Snapshot 5, each one uses whole 64KB sectors. Freezer 4 is the
// Final Cartridge III(+) of EF3 menu 1.3.0 and newer (EF3BootImage/Makefile of https://github.com/KimJorgensen/easyflash:
// banks $28-$37, 256KB; the older menu of skoe already reserved bank $28 for it, but does not use it)
bool EF3GetArea(bool kernal, int number, EF3Area& area)
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
		{ "Freezer", 4, 0x28, 16, true, true, 27 },
	};
	if (number < 1 || number > 4)
	{
		printf("Freezer %d does not exist, only 1 (Retro Replay), 2 (Action Replay), 3 (Super Snapshot 5) and 4 (Final Cartridge III)\n", number);
		return false;
	}
	area = freezers[number - 1];
	return true;
}

// Leaves out empty 8KB blocks at the end
std::string EF3AreaFromImage(const unsigned char* image, const EF3Area& area)
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

std::string EF3DirectoryName(const unsigned char* directory, int index)
{
	// Entries of 16 bytes after the signature: 16 slots, 8 KERNALs, then the freezers
	return PETSCIIToASCII(directory + 16 + index * 16, 16);
}

// The FC3 freezer (entry 27) is only in the EF3 menu 1.3.0 and newer, which names it ("FC3 Slot" when unused). An
// older directory has no entry there, and the menu does not start the FC3 (it also needs the CPLD firmware 1.2.0).
bool EF3MenuHasFC3(const unsigned char* directory)
{
	const unsigned char* entry = directory + 16 + 27 * 16;
	return entry[0] >= 0x20 && entry[0] != 0xff;
}

bool EF3CheckFC3(const unsigned char* directory, const EF3Area& area)
{
	if (strcmp(area.type, "Freezer") != 0 || area.number != 4 || EF3MenuHasFC3(directory))
	{
		return true;
	}
	printf("The EF3 menu has no FC3 slot (directory entry 27 is empty): it is older than 1.3.0 and does not start a Final Cartridge III. Nothing was changed\n");
	return false;
}

// Writes the EasyFlash slots 1 to 7 as .crt, the KERNALs and the freezers as .bin, named after the EF3 menu directory
bool EF3Extract(const unsigned char* image, const std::string& folder)
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
		for (int number = 1; number <= (kernal ? 8 : 4); number++)
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
bool EF3LoadSlotFile(const char* filename, unsigned char* slotData)
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
void EF3ReadCurrentChip(bool high, unsigned char* data)
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
		// GetInputByte() waits 200ns for the data bus and reads D0 first. After the address change D0 sometimes still
		// has the value of the address before, so D0 is read again after the other bits (about 1us after the address).
		int value = GetInputByte();
		data[address] = (unsigned char)((value & 0xfe) | (digitalRead(0) == HIGH ? 1 : 0));
	}

	C64Control::ClearLowROM();
	C64Control::ClearHighROM();
	C64Control::UpdateLatch();
}

// Reads all 8 slots of an EasyFlash 3
bool EF3ReadImage(unsigned char* image)
{
	InterfaceControl::SetLED2();
	InterfaceControl::UpdateLatch();

	printf("Reading EasyFlash 3...\n");
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
				EF3ReadCurrentChip(chip == 1, image + slot * kEF3SlotSize + (bank * 2 + chip) * kEF3BankSize);
			}
		}
	}

	InterfaceControl::ClearLED2();
	InterfaceControl::UpdateLatch();

	return !gotError;
}

// Reads 8KB of ROML or ROMH from a bank of the selected EF3 slot, from the selected programmer slot
void EF3ReadChip(int bank, bool high, unsigned char* data)
{
	SetDataIO1WithPHI2Edge(0, bank);
	EF3ReadCurrentChip(high, data);
}

// Reads the menu directory of the selected programmer slot
bool EF3ReadDirectory(unsigned char* directory)
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

// --ef3dir: the names of the menu directory of the EF3 in the slot of --activeslot, and which slots are empty (bank 0
// ROML only $ff). Reads the directory bank and one bank of each slot, much faster than --readef3.
void EF3PrintDirectory(void)
{
	static unsigned char directory[kEF3BankSize];
	if (!EF3ReadDirectory(directory))
	{
		return;
	}
	static unsigned char bank0[kEF3BankSize];
	for (int slot = 1; slot < kEF3NumSlots; slot++)
	{
		bool empty = false;
		if (EF3SetSlotAndBank(slot, 0))
		{
			EF3ReadChip(0, false, bank0);
			empty = true;
			for (int i = 0; i < kEF3BankSize && empty; i++)
			{
				empty = bank0[i] == 0xff;
			}
		}
		printf("EF3 slot %d: %s%s\n", slot, EF3DirectoryName(directory, slot).c_str(), empty ? " (empty)" : "");
	}
	bool fc3 = EF3MenuHasFC3(directory);
	for (int pass = 0; pass < 2; pass++)
	{
		bool kernal = pass == 0;
		for (int number = 1; number <= (kernal ? 8 : (fc3 ? 4 : 3)); number++)
		{
			EF3Area area;
			if (EF3GetArea(kernal, number, area))
			{
				printf("EF3 %s %d: %s\n", kernal ? "KERNAL" : "freezer", number, EF3DirectoryName(directory, area.directoryIndex).c_str());
			}
		}
	}
	EF3SetSlotAndBank(0, 0);
}

// The programmer slots with an EasyFlash 3 that is written: slot 1 to n of --slots n or the slots of --slotlist, or only
// the slot of --activeslot.
// All slots get every write at the same time, so all of them get the same data. Reads come from one slot at a time.
std::vector<int> EF3ProgrammerSlots(void)
{
	std::vector<int> slots;
	if (usedSlotMask != 1)
	{
		slots = UsedSlots();
	}
	else
	{
		slots.push_back(theActiveSlot);
	}
	return slots;
}

void EF3SelectProgrammerSlot(int programmerSlot)
{
	InterfaceControl::SetActiveSlot(programmerSlot);
	InterfaceControl::UpdateLatch();
}

// Selects the programmer slot to read its _GAME and _EXROM (GPIO 26 and 25), which need some us to follow the switch:
// on piper1 an empty slot read _GAME low right after the slot of a Kung Fu Flash (_GAME low) in 31 of 40 tries, with
// 10 us never
void SelectProgrammerSlotForLines(int programmerSlot)
{
	EF3SelectProgrammerSlot(programmerSlot);
	delayMicroseconds(10);
}

// The programmer sends every write to all its slots, but only the slots of EF3ProgrammerSlots() are checked.
// So nothing is written while a cartridge is in another slot: an empty slot reads the flash ID $ff $ff.
// Without --slots, another EF3 with the same content only gives a note, because a programmer with one slot reads
// the same cartridge in every slot. The content is compared with the menu directory and bank 0 ROML of every EF3 slot.
bool EF3CheckOtherProgrammerSlots(void)
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
	bool compare = usedSlotMask == 1 && theActiveSlot < kMaxProgrammerSlots && isEF3(theActiveSlot);
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
			EF3ReadCurrentChip(false, fingerprint[programmerSlot] + slot * kEF3BankSize);
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

// Reads the flash ID in every programmer slot before writing. Without an MX29LV640EB in all of them nothing is written.
bool EF3CheckChips(void)
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
	else if (usedSlotMask != 1)
	{
		printf("EasyFlash 3 found in programmer slots %s, all of them get the same data\n", UsedSlotsText().c_str());
	}
	return allFound;
}

// Selects the EF3 slot and bank in all programmer slots and reads back the slot register of each one
bool EF3SelectSlotAndBank(int slot, int bank)
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
bool EF3WaitForFlashAll(bool high, int address, int value, int timeoutMicroseconds, int& failedProgrammerSlot)
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
bool ef3NoConfirm = false;

bool AskConfirmation(const char* expected)
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

bool AskYes(void)
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
bool EF3SelectBankChecked(int slot, int bank, bool high)
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
			EF3ReadCurrentChip(high, other[programmerSlot]);
			allReferencesEmpty[programmerSlot] = allReferencesEmpty[programmerSlot] && IsEmpty(other[programmerSlot], kEF3BankSize);
		}
		SetDataIO1WithPHI2Edge(0, bank);
		std::vector<int> stillUnproven;
		for (int programmerSlot : unproven)
		{
			EF3SelectProgrammerSlot(programmerSlot);
			EF3ReadCurrentChip(high, target);
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
bool EF3EraseSectorInCurrentBank(bool high)
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
bool ef3UnlockBypassWorks = true;
bool ef3UnlockBypassReported = false;

// Programs one byte in the selected bank, in unlock bypass mode or with the normal program command
bool EF3ProgramByte(bool high, int address, int value, bool bypass, int& failedProgrammerSlot)
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

void EF3EnterUnlockBypass(void)
{
	SetDataROMLWithPHI2Edge(0xaaa, 0xaa);
	SetDataROMLWithPHI2Edge(0x555, 0x55);
	SetDataROMLWithPHI2Edge(0xaaa, 0x20);
}

void EF3ExitUnlockBypass(void)
{
	SetDataROMLWithPHI2Edge(0, 0x90);
	SetDataROMLWithPHI2Edge(0, 0x00);
	// Back to read mode, also when the flash ignored the bypass commands
	SetDataROMLWithPHI2Edge(0, 0xf0);
}

// Programs 8KB in the selected bank of all programmer slots and verifies each one, $ff bytes are skipped because they are erased already
bool EF3ProgramInCurrentBank(bool high, const unsigned char* data)
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
		EF3ReadCurrentChip(high, got);
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
bool EF3WriteSector(int slot, int firstBank, int numBanks, bool high, const std::function<const unsigned char* (int)>& target, bool& changed, bool& skipped)
{
	static unsigned char current[kEF3BankSize];
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
			EF3ReadCurrentChip(high, current);
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
bool EF3NameValid(const char* name)
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
bool EF3ReadDirectorySector(unsigned char* directory)
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
			EF3ReadCurrentChip(false, chip);
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
void EF3SetDirectoryName(unsigned char* directory, int index, const std::string& name)
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
void EF3SetName(int index, const char* label, const char* name)
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
void EF3ReadArea(const EF3Area& area, const char* filename)
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
void EF3WriteArea(const EF3Area& area, const char* filename)
{
	const int areaSize = EF3AreaSize(area);
	static unsigned char areaData[16 * 2 * kEF3BankSize];
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
		else if (!EF3CheckFC3(directory, area))
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
		// A sector has at most 8 banks, the FC3 freezer two of them in ROML and in ROMH
		for (int bank = area.firstBank; bank < area.firstBank + area.numBanks && !gotError; bank += 8)
		{
			bool changed = false;
			bool skipped = false;
			gotError = !EF3WriteSector(0, bank, std::min(8, area.firstBank + area.numBanks - bank), high == 1, target, changed, skipped);
			erased = erased || changed;
		}
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

// The names that EasyProg uses for empty entries: "Slot n" (checkEraseSlot() in write.c), "KERNAL n" (slotsFillEFDir()
// in slots.c). For the freezers the tested cartridges show "SS5 Slot" and "FC3 Slot" for unused entries.
std::string EF3DefaultName(int index)
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

// A KERNAL is one boot sector, a freezer one or two 64KB sectors (8 banks) of ROML and of ROMH
void EF3AddAreaSectors(const EF3Area& area, std::vector<EF3Sector>& sectors)
{
	for (int high = 0; high < 2; high++)
	{
		if ((high == 0 && !area.low) || (high == 1 && !area.high))
		{
			continue;
		}
		for (int bank = area.firstBank; bank < area.firstBank + area.numBanks; bank += 8)
		{
			sectors.push_back({ 0, bank, std::min(8, area.firstBank + area.numBanks - bank), high == 1 });
		}
	}
}

void EF3AddSlotSectors(int slot, std::vector<EF3Sector>& sectors)
{
	for (int sector = 0; sector < 16; sector++)
	{
		sectors.push_back({ slot, (sector / 2) * 8, 8, (sector & 1) != 0 });
	}
}

// Erases sectors and sets the names of the erased entries back to the names EasyProg uses for empty entries.
// Sectors that are empty already are skipped, and the directory is only rewritten if a name changes.
void EF3Erase(const char* what, const std::vector<EF3Sector>& sectors, const std::vector<int>& nameIndices, const char* confirmation)
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
void EF3WriteImage(const char* filename, bool all)
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
		for (int number = 1; number <= 4; number++)
		{
			EF3Area area;
			EF3GetArea(false, number, area);
			EF3AddAreaSectors(area, sectors);
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
void EF3ReadSlot(int slot, const char* filename)
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
