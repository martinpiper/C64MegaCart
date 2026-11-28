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
#include "EasyFlash1.h"

// EasyFlash 1 (EasyFlash-ProgRef.pdf, the schematic of the EasyFlash 1CR, eapi-am29f040.s and eapi-sst39sf040.s of
// https://gitlab.com/easyflash/eapi): two 512KB flash chips, one on ROML and one on ROMH, 64 banks of 8KB each in the
// same order as an EasyFlash .crt and an EF3 slot. $de00 selects the bank (bits 0-5) and cannot be read back, $de02 is
// the control register (bit 7 LED, bit 2 mode, bit 1 _EXROM, bit 0 _GAME). TTL logic: the registers and the flash take
// a write while PHI2 is high and R/W is low, at the end of the select line, as the PHI2 edge writes of the EF3 do it.
// Each chip gets its own commands through its own select line. A sector is 64KB, 8 banks of one chip.
static const int kEF1NumBanks = 64;
static const int kEF1SectorBanks = 8;

static const EF1FlashChip kEF1FlashChips[] = {
	{ "Am29F040", 0x01, 0xa4, false, 0x10000 },
	{ "M29F040", 0x20, 0xe2, false, 0x10000 },
	{ "MX29F040", 0xc2, 0xa4, false, 0x10000 },
	{ "SST39SF040", 0xbf, 0xb7, true, 0x1000 },
};

// The flash of ROML and ROMH, set by EF1CheckChips()
static const EF1FlashChip* sEF1Chip[2] = { nullptr, nullptr };
static int sEF1Bank = 0;

void EF1SetBank(int bank)
{
	SetDataIO1WithPHI2Edge(0, bank);
	sEF1Bank = bank;
}

void EF1WriteChip(bool high, int address, int value)
{
	if (high)
	{
		SetDataROMHWithPHI2Edge(address, value);
	}
	else
	{
		SetDataROMLWithPHI2Edge(address, value);
	}
}

int EF1ReadChip(bool high, int address)
{
	return high ? GetDataROMH(address) : GetDataROML(address);
}

// The unlock cycles $aa and $55 and, unless it is -1, the command. Long commands change the bank for the cycles and
// set the selected bank again afterwards.
void EF1Command(bool high, bool longCommands, int command)
{
	if (!longCommands)
	{
		EF1WriteChip(high, 0x555, 0xaa);
		EF1WriteChip(high, 0x2aa, 0x55);
		if (command >= 0)
		{
			EF1WriteChip(high, 0x555, command);
		}
		return;
	}
	SetDataIO1WithPHI2Edge(0, 2);
	EF1WriteChip(high, 0x1555, 0xaa);
	SetDataIO1WithPHI2Edge(0, 1);
	EF1WriteChip(high, 0x0aaa, 0x55);
	if (command >= 0)
	{
		SetDataIO1WithPHI2Edge(0, 2);
		EF1WriteChip(high, 0x1555, command);
	}
	SetDataIO1WithPHI2Edge(0, sEF1Bank);
}

const EF1FlashChip* EF1FindChip(int manufacturer, int device)
{
	for (const EF1FlashChip& chip : kEF1FlashChips)
	{
		if (chip.manufacturer == manufacturer && chip.device == device)
		{
			return &chip;
		}
	}
	return nullptr;
}

std::string EF1ChipText(int manufacturer, int device)
{
	const EF1FlashChip* chip = EF1FindChip(manufacturer, device);
	char text[96];
	snprintf(text, sizeof(text), "$%02x $%02x (%s)", manufacturer, device, chip ? chip->name : manufacturer == 0xff && device == 0xff ? "no flash" : "unknown flash");
	return text;
}
void EF1ReadIDs(int ids[kEF1MaxProgrammerSlots][2][2])
{
	EF1SetBank(0);
	for (int high = 0; high < 2; high++)
	{
		EF1Command(high == 1, true, 0x90);
	}
	for (int programmerSlot = 0; programmerSlot < kEF1MaxProgrammerSlots; programmerSlot++)
	{
		EF3SelectProgrammerSlot(programmerSlot);
		for (int high = 0; high < 2; high++)
		{
			ids[programmerSlot][high][0] = EF1ReadChip(high == 1, 0);
			ids[programmerSlot][high][1] = EF1ReadChip(high == 1, 1);
		}
	}
	for (int high = 0; high < 2; high++)
	{
		EF1Command(high == 1, true, 0xf0);
	}
	EF3SelectProgrammerSlot(theActiveSlot);
}

// The chips of all programmer slots that are written must be known and the same. Every write goes to all programmer
// slots, so the others must be empty (flash ID $ff in ROML and ROMH). Without --slots another slot that reads the same
// as the active one (bank 0 of ROML and ROMH) only gives a note: a programmer with one slot reads it in every slot.
bool EF1CheckChips(void)
{
	int ids[kEF1MaxProgrammerSlots][2][2];
	EF1ReadIDs(ids);
	std::vector<int> slots = EF3ProgrammerSlots();
	bool ok = true;
	for (int high = 0; high < 2; high++)
	{
		sEF1Chip[high] = EF1FindChip(ids[slots[0]][high][0], ids[slots[0]][high][1]);
		for (int programmerSlot : slots)
		{
			const EF1FlashChip* chip = EF1FindChip(ids[programmerSlot][high][0], ids[programmerSlot][high][1]);
			if (!chip)
			{
				printf("Programmer slot %d: no EasyFlash 1 found, %s flash ID %s\n", programmerSlot + 1, high ? "ROMH" : "ROML",
					EF1ChipText(ids[programmerSlot][high][0], ids[programmerSlot][high][1]).c_str());
				ok = false;
			}
			else if (sEF1Chip[high] && (chip->longCommands != sEF1Chip[high]->longCommands || chip->eraseSize != sEF1Chip[high]->eraseSize))
			{
				// The writes go to all programmer slots at the same time, so all chips must take the same commands
				printf("Programmer slot %d: %s is %s, programmer slot %d has %s. Write them one at a time\n", programmerSlot + 1, high ? "ROMH" : "ROML",
					chip->name, slots[0] + 1, sEF1Chip[high]->name);
				ok = false;
			}
		}
	}
	static unsigned char active[2][256], other[2][256];
	EF1SetBank(0);
	for (int high = 0; high < 2; high++)
	{
		for (int i = 0; i < 256; i++)
		{
			active[high][i] = (unsigned char) EF1ReadChip(high == 1, i);
		}
	}
	int sameContent = 0;
	for (int programmerSlot = 0; programmerSlot < kEF1MaxProgrammerSlots && ok; programmerSlot++)
	{
		if (std::find(slots.begin(), slots.end(), programmerSlot) != slots.end())
		{
			continue;
		}
		bool empty = true;
		for (int high = 0; high < 2; high++)
		{
			empty = empty && ids[programmerSlot][high][0] == 0xff && ids[programmerSlot][high][1] == 0xff;
		}
		if (empty)
		{
			continue;
		}
		EF3SelectProgrammerSlot(programmerSlot);
		bool same = activeSlots <= 1;
		for (int high = 0; high < 2 && same; high++)
		{
			for (int i = 0; i < 256; i++)
			{
				other[high][i] = (unsigned char) EF1ReadChip(high == 1, i);
			}
			same = ids[programmerSlot][high][0] == ids[slots[0]][high][0] && ids[programmerSlot][high][1] == ids[slots[0]][high][1] &&
				memcmp(active[high], other[high], 256) == 0;
		}
		if (same)
		{
			sameContent++;
			continue;
		}
		printf("Programmer slot %d is not empty (flash ID ROML %s, ROMH %s). It would get the same writes without any check.\n", programmerSlot + 1,
			EF1ChipText(ids[programmerSlot][0][0], ids[programmerSlot][0][1]).c_str(), EF1ChipText(ids[programmerSlot][1][0], ids[programmerSlot][1][1]).c_str());
		printf("Take it out, or write all EasyFlash 1 with --slots or --slotlist\n");
		ok = false;
	}
	EF3SelectProgrammerSlot(theActiveSlot);
	if (sameContent > 0)
	{
		printf("Note: %d other programmer slot(s) read the same EasyFlash 1. With a one slot programmer this is normal; otherwise they get the same writes without any check, use --slots or --slotlist to check them too\n", sameContent);
	}
	if (!ok)
	{
		printf("Nothing was written\n");
		return false;
	}
	printf("EasyFlash 1: ROML %s, ROMH %s", sEF1Chip[0]->name, sEF1Chip[1]->name);
	if (slots.size() > 1)
	{
		printf(", in programmer slots %s, all of them get the same data", UsedSlotsText().c_str());
	}
	printf("\n");
	return true;
}

// The bank register cannot be read back. As for the EF3: reading another bank first and then the target bank proves
// that the register changed when the content differs, in every programmer slot. Where all other banks of the chip are
// empty, the commands can only hit empty banks, so no proof is needed.
bool EF1SelectBankChecked(int bank, bool high)
{
	static unsigned char target[kEF3BankSize];
	static unsigned char other[kEF1MaxProgrammerSlots][kEF3BankSize];
	std::vector<int> unproven = EF3ProgrammerSlots();
	bool allReferencesEmpty[kEF1MaxProgrammerSlots];
	for (int programmerSlot = 0; programmerSlot < kEF1MaxProgrammerSlots; programmerSlot++)
	{
		allReferencesEmpty[programmerSlot] = true;
	}
	for (int reference = 0; reference < kEF1NumBanks && !unproven.empty(); reference++)
	{
		if (reference == bank)
		{
			continue;
		}
		EF1SetBank(reference);
		for (int programmerSlot : unproven)
		{
			EF3SelectProgrammerSlot(programmerSlot);
			EF3ReadCurrentChip(high, other[programmerSlot]);
			allReferencesEmpty[programmerSlot] = allReferencesEmpty[programmerSlot] && IsEmpty(other[programmerSlot], kEF3BankSize);
		}
		EF1SetBank(bank);
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
	EF1SetBank(bank);
	bool allProven = true;
	for (int programmerSlot : unproven)
	{
		if (allReferencesEmpty[programmerSlot])
		{
			continue;
		}
		printf("Programmer slot %d: cannot prove that bank $%02x %s is selected: the bank register did not change or all banks have the same content\n", programmerSlot + 1, bank, high ? "ROMH" : "ROML");
		allProven = false;
	}
	return allProven;
}

// Erases the 64KB sector of firstBank, with an SST39SF040 in 16 steps of 4KB as eapi-sst39sf040.s does it
bool EF1EraseSector(int firstBank, bool high)
{
	const EF1FlashChip& chip = *sEF1Chip[high ? 1 : 0];
	for (int offset = 0; offset < kEF1SectorBanks * kEF3BankSize; offset += chip.eraseSize)
	{
		int bank = firstBank + offset / kEF3BankSize;
		if (bank != sEF1Bank)
		{
			EF1SetBank(bank);
		}
		EF1Command(high, chip.longCommands, 0x80);
		EF1Command(high, chip.longCommands, -1);
		EF1WriteChip(high, offset % kEF3BankSize, 0x30);
		int failedProgrammerSlot = 0;
		if (!EF3WaitForFlashAll(high, offset % kEF3BankSize, 0xff, 30000000, failedProgrammerSlot))
		{
			printf("Programmer slot %d: timeout while erasing\n", failedProgrammerSlot + 1);
			EF1Command(high, chip.longCommands, 0xf0);
			return false;
		}
	}
	EF1SetBank(firstBank);
	return true;
}

// Programs 8KB in the selected bank of all programmer slots and verifies each one, $ff bytes are left out because the
// sector is erased or empty
bool EF1ProgramInCurrentBank(bool high, const unsigned char* data)
{
	const EF1FlashChip& chip = *sEF1Chip[high ? 1 : 0];
	for (int address = 0; address < kEF3BankSize; address++)
	{
		if (data[address] == 0xff)
		{
			continue;
		}
		// A byte that does not arrive is programmed again, up to 3 times: bits that are 0 already stay 0
		int failedProgrammerSlot = 0;
		bool programmed = false;
		for (int attempt = 0; attempt < 3 && !programmed; attempt++)
		{
			if (attempt > 0)
			{
				EF1Command(high, chip.longCommands, 0xf0);
			}
			EF1Command(high, chip.longCommands, 0xa0);
			EF1WriteChip(high, address, data[address]);
			programmed = EF3WaitForFlashAll(high, address, data[address], 10000, failedProgrammerSlot);
		}
		if (!programmed)
		{
			printf("Programmer slot %d: timeout while writing $%02x to %s $%04x\n", failedProgrammerSlot + 1, data[address], high ? "ROMH" : "ROML", address);
			EF1Command(high, chip.longCommands, 0xf0);
			return false;
		}
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

// Writes one 64KB sector (8 banks of ROML or ROMH from firstBank), as EF3WriteSector(): it is read first in every
// programmer slot, skipped when it holds the data in all of them, not erased when it is empty in all of them; the bank
// switch is proven before erasing. changed tells if the flash was touched, also when an error happened.
bool EF1WriteSector(int firstBank, bool high, const unsigned char* slotData, bool& changed)
{
	auto target = [&](int bank) { return slotData + (bank * 2 + (high ? 1 : 0)) * kEF3BankSize; };
	static unsigned char current[kEF3BankSize];
	changed = false;
	bool same = true;
	bool empty = true;
	for (int bank = firstBank; bank < firstBank + kEF1SectorBanks; bank++)
	{
		EF1SetBank(bank);
		for (int programmerSlot : EF3ProgrammerSlots())
		{
			EF3SelectProgrammerSlot(programmerSlot);
			EF3ReadCurrentChip(high, current);
			same = same && memcmp(current, target(bank), kEF3BankSize) == 0;
			empty = empty && IsEmpty(current, kEF3BankSize);
		}
	}
	printf("Banks $%02x to $%02x %s: ", firstBank, firstBank + kEF1SectorBanks - 1, high ? "ROMH" : "ROML");
	if (same)
	{
		printf("unchanged\n");
		return true;
	}
	bool targetEmpty = true;
	for (int bank = firstBank; bank < firstBank + kEF1SectorBanks; bank++)
	{
		targetEmpty = targetEmpty && IsEmpty(target(bank), kEF3BankSize);
	}
	printf("%s\n", empty ? "writing" : targetEmpty ? "erasing" : "erasing and writing");
	fflush(stdout);

	if (!empty)
	{
		if (!EF1SelectBankChecked(firstBank, high))
		{
			return false;
		}
		changed = true;
		if (!EF1EraseSector(firstBank, high))
		{
			return false;
		}
	}
	for (int bank = firstBank; bank < firstBank + kEF1SectorBanks; bank++)
	{
		EF1SetBank(bank);
		changed = true;
		if (!EF1ProgramInCurrentBank(high, target(bank)))
		{
			printf("Error in bank $%02x\n", bank);
			return false;
		}
	}
	return true;
}

// The LED of the EasyFlash 1 is on while it is written, the cartridge is off ($de02 mode 1, _GAME and _EXROM high)
void EF1Led(bool on)
{
	SetDataIO1WithPHI2Edge(2, on ? 0x84 : 0x00);
}

// --ef1id: the flash IDs of ROML and ROMH, and whether the bank register takes the writes: the first 256 bytes of
// ROML and ROMH are read in all 64 banks, a working register gives different content in the used banks
bool EF1ID(void)
{
	int ids[kEF1MaxProgrammerSlots][2][2];
	EF1ReadIDs(ids);
	bool ok = true;
	for (int programmerSlot : EF3ProgrammerSlots())
	{
		bool known = true;
		for (int high = 0; high < 2; high++)
		{
			known = known && EF1FindChip(ids[programmerSlot][high][0], ids[programmerSlot][high][1]) != nullptr;
		}
		printf("Programmer slot %d: ROML flash ID %s, ROMH flash ID %s%s\n", programmerSlot + 1,
			EF1ChipText(ids[programmerSlot][0][0], ids[programmerSlot][0][1]).c_str(), EF1ChipText(ids[programmerSlot][1][0], ids[programmerSlot][1][1]).c_str(),
			known ? "" : ", no EasyFlash 1");
		ok = ok && known;
	}
	static unsigned char start[kEF1MaxProgrammerSlots][kEF1NumBanks][512];
	for (int bank = 0; bank < kEF1NumBanks; bank++)
	{
		EF1SetBank(bank);
		for (int programmerSlot : EF3ProgrammerSlots())
		{
			EF3SelectProgrammerSlot(programmerSlot);
			for (int i = 0; i < 512; i++)
			{
				start[programmerSlot][bank][i] = (unsigned char) EF1ReadChip(i >= 256, i & 0xff);
			}
		}
	}
	EF1SetBank(0);
	for (int programmerSlot : EF3ProgrammerSlots())
	{
		int used = 0, different = 0;
		for (int bank = 0; bank < kEF1NumBanks; bank++)
		{
			if (IsEmpty(start[programmerSlot][bank], 512))
			{
				continue;
			}
			used++;
			bool unique = true;
			for (int other = 0; other < kEF1NumBanks && unique; other++)
			{
				unique = other == bank || memcmp(start[programmerSlot][bank], start[programmerSlot][other], 512) != 0;
			}
			different += unique ? 1 : 0;
		}
		printf("Programmer slot %d: %d of 64 banks hold data, %d of them differ from all other banks%s\n", programmerSlot + 1, used, different,
			used == 0 ? " (empty, the bank register can not be checked)" : different > 0 ? ": the bank register works" : ": the bank register DOES NOT WORK or all banks are the same");
		ok = ok && (used == 0 || different > 0);
	}
	EF3SelectProgrammerSlot(theActiveSlot);
	return ok;
}

// --readef1 <file>: the whole EasyFlash 1 of the programmer slot of --activeslot as .crt, named after the file
bool EF1Read(const char* filename)
{
	InterfaceControl::SetLED2();
	InterfaceControl::UpdateLatch();
	static unsigned char slotData[kEF3SlotSize];
	for (int bank = 0; bank < kEF1NumBanks; bank++)
	{
		EF1SetBank(bank);
		for (int high = 0; high < 2; high++)
		{
			EF3ReadCurrentChip(high == 1, slotData + (bank * 2 + high) * kEF3BankSize);
		}
	}
	EF1SetBank(0);
	InterfaceControl::ClearLED2();
	InterfaceControl::UpdateLatch();

	std::string name = filename;
	name = name.substr(name.find_last_of('/') + 1);
	name = name.substr(0, name.find_last_of('.'));
	int numChips = 0;
	if (!EF3WriteCRT(filename, name, slotData, numChips))
	{
		return false;
	}
	if (numChips == 0)
	{
		printf("The EasyFlash 1 is empty (or no EasyFlash 1 in programmer slot %d), nothing written\n", theActiveSlot + 1);
		return false;
	}
	int banks = 0;
	for (int bank = 0; bank < kEF1NumBanks; bank++)
	{
		banks += IsEmpty(slotData + bank * 2 * kEF3BankSize, 2 * kEF3BankSize) ? 0 : 1;
	}
	printf("%s: EasyFlash 1 of programmer slot %d, %d chips in %d banks\n", filename, theActiveSlot + 1, numChips, banks);
	return true;
}

// --writeef1 <file>: an EasyFlash .crt (hardware type 32) or a raw image of up to 1MB (8KB ROML and 8KB ROMH per bank)
// into all programmer slots of --slots, sector by sector, unchanged sectors are left out
bool EF1Write(const char* filename)
{
	static unsigned char slotData[kEF3SlotSize];
	if (!EF3LoadSlotFile(filename, slotData))
	{
		return false;
	}
	InterfaceControl::SetLED1();
	InterfaceControl::UpdateLatch();
	bool ok = EF1CheckChips();
	bool anyChanged = false;
	if (ok)
	{
		EF1Led(true);
		for (int sector = 0; sector < 2 * kEF1NumBanks / kEF1SectorBanks && ok; sector++)
		{
			bool changed = false;
			ok = EF1WriteSector((sector / 2) * kEF1SectorBanks, (sector & 1) != 0, slotData, changed);
			anyChanged = anyChanged || changed;
		}
		EF1SetBank(0);
		EF1Led(false);
		if (ok)
		{
			printf("EasyFlash 1 written and verified\n");
		}
		else
		{
			printf("Writing the EasyFlash 1 failed%s\n", anyChanged ? "" : ", nothing was changed");
		}
	}
	InterfaceControl::ClearLED1();
	InterfaceControl::UpdateLatch();
	return ok;
}
