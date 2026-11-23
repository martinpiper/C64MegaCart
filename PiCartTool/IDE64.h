#pragma once

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

int Ide64ReadBus(int address, void (*select)(void), void (*deselect)(void));
int Ide64ReadIO1(int address);
int Ide64ReadROML(int address);

extern int sExitCode;

void Ide64Cycle(int address, int value, void (*select)(void), void (*deselect)(void));
void Ide64WriteIO1(int address, int value);
int Ide64Status(void);
void Ide64SelectBank(int bank);
int Ide64ReadFlash(int address);
void Ide64WriteFlash(int address, int value);
void Ide64FlashCommand(int command);
bool Ide64WaitReady(int address, int timeoutMilliseconds);
int Ide64Detect(int& status);
void Ide64Leave(void);
const Ide64FlashChip* Ide64FlashID(int& manufacturer, int& device);
void Ide64WaitForButton(const char* text);
bool Ide64CheckOtherSlots(void);
bool Ide64Prepare(int& banks, bool quiet = false);
bool Ide64FlashHidden(int& status);
void Ide64ClockCommand(int command);
void Ide64ClockRead(int command, unsigned char* data, int size);
void Ide64ClockWrite(int command, const unsigned char* data, int size);
std::string Ide64ClockText(const unsigned char* clock);
void Ide64ShowClock(void);
int Ide64Bcd(int value);
int Ide64FromBcd(int value);
bool Ide64SetClock(const char* when);
const char* Ide64SetupVersion(const unsigned char* data);
std::string Ide64FlashVersion(void);
int Ide64ReadSetupBytes(unsigned char* data);
void Ide64ShowSetup(const unsigned char* data);
bool Ide64LoadSetupFile(const char* filename, unsigned char* data, const char* failText);
const char* Ide64ColourName(int colour);
std::string Ide64DriveName(int value);
void Ide64ShowSetupNames(const unsigned char* data, const char* version);
bool Ide64ShowSetupByName(const char* filename);
bool Ide64ReadSetup(const char* filename);
bool Ide64WriteSetup(const char* filename);
bool Ide64ID(void);
int Ide64AtaRegister(int reg);
int Ide64AtaWait(int timeoutMilliseconds);
void Ide64AtaSelect(int device);
bool Ide64AtaIdentify(int device, int command, unsigned short* words);
std::string Ide64AtaText(const unsigned short* words, int first, int count);
std::string Ide64AtaName(const unsigned short* words, bool packet);
std::vector<std::string> Ide64ScanDevices(bool& ok);
bool Ide64Devices(void);
bool Ide64ReadAll(int banks, std::vector<unsigned char>& data, int& reads);
bool Ide64Read(const char* filename);
bool Ide64WriteImage(std::vector<unsigned char> file, const char* filename, bool waitForJumper,	const std::function<bool(int, std::vector<unsigned char>&)>& prepare = nullptr, bool upperFirst = false);
bool Ide64Write(const char* filename, bool waitForJumper);
bool Ide64Swap(const char* backupFile, bool waitForJumper);
