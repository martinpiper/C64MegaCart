#pragma once

// EasyFlash 1 and compatible (two 512KB flash chips on ROML and ROMH): --ef1id, --readef1, --writeef1
struct EF1FlashChip
{
	const char* name;
	int manufacturer;
	int device;
	// The commands go to $5555/$2aaa of the chip (bank 2 $1555, bank 1 $0aaa) instead of $555/$2aa of any bank
	bool longCommands;
	// The erase command clears this much, a 64KB sector needs several of them when it is smaller
	int eraseSize;
};
void EF1SetBank(int bank);
void EF1WriteChip(bool high, int address, int value);
int EF1ReadChip(bool high, int address);
// The unlock cycles $aa and $55 and, unless it is -1, the command. Long commands change the bank for the cycles and
// set the selected bank again afterwards.
void EF1Command(bool high, bool longCommands, int command);
const EF1FlashChip* EF1FindChip(int manufacturer, int device);
std::string EF1ChipText(int manufacturer, int device);
// The flash IDs of ROML and ROMH in all 5 programmer slots. The long commands work for all chips of kEF1FlashChips.
static const int kEF1MaxProgrammerSlots = 5;
void EF1ReadIDs(int ids[kEF1MaxProgrammerSlots][2][2]);
// The chips of all programmer slots that are written must be known and the same. Every write goes to all programmer
// slots, so the others must be empty (flash ID $ff in ROML and ROMH). Without --slots another slot that reads the same
// as the active one (bank 0 of ROML and ROMH) only gives a note: a programmer with one slot reads it in every slot.
bool EF1CheckChips(void);
// The bank register cannot be read back. As for the EF3: reading another bank first and then the target bank proves
// that the register changed when the content differs, in every programmer slot. Where all other banks of the chip are
// empty, the commands can only hit empty banks, so no proof is needed.
bool EF1SelectBankChecked(int bank, bool high);
// Erases the 64KB sector of firstBank, with an SST39SF040 in 16 steps of 4KB as eapi-sst39sf040.s does it
bool EF1EraseSector(int firstBank, bool high);
// Programs 8KB in the selected bank of all programmer slots and verifies each one, $ff bytes are left out because the
// sector is erased or empty
bool EF1ProgramInCurrentBank(bool high, const unsigned char* data);
// Writes one 64KB sector (8 banks of ROML or ROMH from firstBank), as EF3WriteSector(): it is read first in every
// programmer slot, skipped when it holds the data in all of them, not erased when it is empty in all of them; the bank
// switch is proven before erasing. changed tells if the flash was touched, also when an error happened.
bool EF1WriteSector(int firstBank, bool high, const unsigned char* slotData, bool& changed);
// The LED of the EasyFlash 1 is on while it is written, the cartridge is off ($de02 mode 1, _GAME and _EXROM high)
void EF1Led(bool on);
// --ef1id: the flash IDs of ROML and ROMH, and whether the bank register takes the writes: the first 256 bytes of
// ROML and ROMH are read in all 64 banks, a working register gives different content in the used banks
bool EF1ID(void);
// --readef1 <file>: the whole EasyFlash 1 of the programmer slot of --activeslot as .crt, named after the file
bool EF1Read(const char* filename);
// --writeef1 <file>: an EasyFlash .crt (hardware type 32) or a raw image of up to 1MB (8KB ROML and 8KB ROMH per bank)
// into all programmer slots of --slots, sector by sector, unchanged sectors are left out
bool EF1Write(const char* filename);
