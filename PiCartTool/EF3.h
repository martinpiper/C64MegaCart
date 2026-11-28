#pragma once

// EasyFlash 3: 8 slots of 64 banks, each bank is 8KB ROML followed by 8KB ROMH, the same order as an EasyFlash .crt
static const int kEF3BankSize = 8192;
static const int kEF3NumBanks = 64;
static const int kEF3SlotSize = kEF3NumBanks * 2 * kEF3BankSize;
static const int kEF3NumSlots = 8;
static const int kEF3ImageSize = kEF3NumSlots * kEF3SlotSize;
// The EF3 menu directory in the system area (slot 0), see the Makefile of https://gitlab.com/easyflash/easyflash3-bootimage
static const int kEF3DirectoryBank = 0x10;
static const unsigned char kEF3DirectorySignature[] = { 0xc5, 0xc6, 0x2d, 0xc4, 0x49, 0x52, 0x45, 0x43, 0x54, 0x4f, 0x52, 0x59 };


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


const unsigned char* EF3Chip(const unsigned char* image, int slot, int bank, bool high);
bool IsEmpty(const unsigned char* data, int size);
std::string PETSCIIToASCII(const unsigned char* data, int size);
std::string EF3FileName(const std::string& folder, const char* prefix, int index, const std::string& name, const char* extension);
bool WriteFile(const std::string& filename, const unsigned char* data, int size);
void WriteBigEndian(std::string& out, unsigned int value, int bytes);
bool EF3WriteCRT(const std::string& filename, const std::string& name, const unsigned char* slotData, int& numChips);
int EF3AreaSize(const EF3Area& area);
bool EF3GetArea(bool kernal, int number, EF3Area& area);
std::string EF3AreaFromImage(const unsigned char* image, const EF3Area& area);
std::string EF3DirectoryName(const unsigned char* directory, int index);
bool EF3MenuHasFC3(const unsigned char* directory);
bool EF3CheckFC3(const unsigned char* directory, const EF3Area& area);
bool EF3Extract(const unsigned char* image, const std::string& folder);
bool EF3LoadSlotFile(const char* filename, unsigned char* slotData);
void EF3ReadCurrentChip(bool high, unsigned char* data);
bool EF3ReadImage(unsigned char* image);
void EF3ReadChip(int bank, bool high, unsigned char* data);
bool EF3ReadDirectory(unsigned char* directory);
void EF3PrintDirectory(void);
std::vector<int> EF3ProgrammerSlots(void);
void EF3SelectProgrammerSlot(int programmerSlot);
void SelectProgrammerSlotForLines(int programmerSlot);
bool EF3CheckOtherProgrammerSlots(void);
bool EF3CheckChips(void);
bool EF3SelectSlotAndBank(int slot, int bank);
bool EF3WaitForFlashAll(bool high, int address, int value, int timeoutMicroseconds, int& failedProgrammerSlot);
extern bool ef3NoConfirm;
bool AskConfirmation(const char* expected);
bool AskYes(void);
bool EF3SelectBankChecked(int slot, int bank, bool high);
bool EF3EraseSectorInCurrentBank(bool high);
extern bool ef3UnlockBypassWorks;
extern bool ef3UnlockBypassReported;
bool EF3ProgramByte(bool high, int address, int value, bool bypass, int& failedProgrammerSlot);
void EF3EnterUnlockBypass(void);
void EF3ExitUnlockBypass(void);
bool EF3ProgramInCurrentBank(bool high, const unsigned char* data);
bool EF3WriteSector(int slot, int firstBank, int numBanks, bool high, const std::function<const unsigned char* (int)>& target, bool& changed, bool& skipped);
bool EF3NameValid(const char* name);
bool EF3ReadDirectorySector(unsigned char* directory);
void EF3SetDirectoryName(unsigned char* directory, int index, const std::string& name);
void EF3SetName(int index, const char* label, const char* name);
void EF3ReadArea(const EF3Area& area, const char* filename);
void EF3WriteArea(const EF3Area& area, const char* filename);

// One sector: 8 banks of ROML or ROMH from firstBank, or in slot 0 one of the 8KB boot sectors (numBanks 1)
struct EF3Sector
{
	int slot;
	int firstBank;
	int numBanks;
	bool high;
};

std::string EF3DefaultName(int index);
void EF3AddAreaSectors(const EF3Area& area, std::vector<EF3Sector>& sectors);
void EF3AddSlotSectors(int slot, std::vector<EF3Sector>& sectors);
void EF3Erase(const char* what, const std::vector<EF3Sector>& sectors, const std::vector<int>& nameIndices, const char* confirmation);
void EF3WriteImage(const char* filename, bool all);
void EF3ReadSlot(int slot, const char* filename);
