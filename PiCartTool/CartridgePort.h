#pragma once

#include <string>
#include <vector>

extern int activeSlots;
extern int theActiveSlot;
// The programmer slots that are used, bit 0 for slot 1: slots 1 to n of --slots n, or the slots of --slotlist
extern int usedSlotMask;

// The used programmer slots (zero based), in order
std::vector<int> UsedSlots(void);
// "1 to 3" or "1, 3, 5", for messages
std::string UsedSlotsText(void);
// --slotlist: "1,3,5" (also "1-3-5" or "1+3+5"), slots 1 to 5. Returns false for a wrong list.
bool SetSlotList(const char* list);
// --slots n: slots 1 to n
void SetSlotCount(int count);

// Some cartridges expect to see HIROM and some expect to see LOROM when writing to the flash
extern bool flashWriteCommandIsHiROM;
// Some cartridges rely on seeing dot clock signals to latch other signals
extern int flashWriteToggleDotClockIterations;

namespace DataLatchOut
{
	void SetData(int value);
}

namespace DataLatchOut
{
	void SetAddress(int value);
}

namespace C64Control
{
	void SetDataLatchOut(void);
	void ClearDataLatchOut(void);

	void SetIO1(void);
	void ClearIO1(void);
	void SetIO2(void);
	void ClearIO2(void);
	void SetLowROM(void);
	void ClearLowROM(void);
	void SetHighROM(void);
	void ClearHighROM(void);
	void SetPHI2(void);
	void ClearPHI2(void);
	void SetRead(void);
	void SetWrite(void);
	void UpdateLatch(void);
	void SetFlashWrite(void);
	void ClearFlashWrite(void);
	void ToggleDotClock(void);
}

namespace InterfaceControl
{
	void UpdateLatch(void);
	void SetReset(void);
	void ClearReset(void);
	void SetLED0(void);
	void ClearLED0(void);
	void SetLED1(void);
	void ClearLED1(void);
	void SetLED2(void);
	void ClearLED2(void);
	void SetRelay1(void);
	void ClearRelay1(void);
	void SetActiveSlot(const int slot);
}

void ShowCartridgeState();

void InitCartTool(void);

void SetDataIO1(int address, int data);

void SetDataIO2(int data);
void SetDataIO2(int address, int data);

// The EasyFlash 3 CPLD only accepts register and flash writes shortly after a rising PHI2 edge
void SetDataIO1WithPHI2Edge(int address, int data);
void SetDataROMLWithPHI2Edge(int address, int data);
void SetDataROMHWithPHI2Edge(int address, int data);
int GetDataIO1(int address);
int GetDataROML(int address);
int GetDataROMH(int address);

// Flash commands of the MX29LV640EB in the EasyFlash 3
void EF3FlashCommand(int command);
bool EF3SetSlotAndBank(int slot, int bank);
bool EF3WaitForFlash(bool high, int address, int value, int timeoutMicroseconds);
