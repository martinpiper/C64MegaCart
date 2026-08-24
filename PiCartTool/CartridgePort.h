#pragma once

extern int activeSlots;

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
