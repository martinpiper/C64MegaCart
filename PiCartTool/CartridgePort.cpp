#include <stdio.h>
#include <time.h>
#include <wiringPi.h>
#include "Hardware.h"

static int sLatchStates[5] = { -1,-1,-1,0,0 };

int activeSlots = 1;
int theActiveSlot = 0;

// Some cartridges expect to see HIROM and some expect to see LOROM when writing to the flash
bool flashWriteCommandIsHiROM = true;
// Some cartridges rely on seeing dot clock signals to latch other signals
int flashWriteToggleDotClockIterations = 0;

namespace DataLatchOut
{
	void SetData(int value)
	{
		// Cache check
		if (sLatchStates[0] == value)
		{
			return;
		}
		sLatchStates[0] = value;
		SetOutputByte(value);
		WriteLatch(0);
	}
}

namespace DataLatchOut
{
	void SetAddress(int value)
	{
		// Cache checks
		int theValue = value & 0xff;
		if (sLatchStates[1] != theValue)
		{
			sLatchStates[1] = theValue;
			SetOutputByte(theValue);
			WriteLatch(1);
		}
		theValue = (value >> 8) & 0xff;
		if (sLatchStates[2] != theValue)
		{
			sLatchStates[2] = theValue;
			SetOutputByte(theValue);
			WriteLatch(2);
		}
	}
}

namespace C64Control
{
	const int kLatch = 3;
	const int kDataLatchOut = 0b00000001;
	const int kFlashWriteEnable = 0b00000010;
	const int kNotIO1 = 0b00000100;
	const int kNotIO2 = 0b00001000;
	const int kPHI2 = 0b00010000;	// High when the CPU can access memory, low is when VIC accesses memory
	const int kReadNotWrite = 0b00100000;
	const int kNotROML = 0b01000000;	// Usually connected to memory _OE
	const int kNotROMH = 0b10000000;

	void SetDataLatchOut(void)
	{
		sLatchStates[kLatch] |= kDataLatchOut;
	}
	void ClearDataLatchOut(void)
	{
		sLatchStates[kLatch] &= ~kDataLatchOut;
	}

	// Note active low logic
	void SetIO1(void)
	{
		sLatchStates[kLatch] &= ~kNotIO1;
	}
	void ClearIO1(void)
	{
		sLatchStates[kLatch] |= kNotIO1;
	}
	void SetIO2(void)
	{
		sLatchStates[kLatch] &= ~kNotIO2;
	}
	void ClearIO2(void)
	{
		sLatchStates[kLatch] |= kNotIO2;
	}
	void SetLowROM(void)
	{
		sLatchStates[kLatch] &= ~kNotROML;
	}
	void ClearLowROM(void)
	{
		sLatchStates[kLatch] |= kNotROML;
	}
	void SetHighROM(void)
	{
		sLatchStates[kLatch] &= ~kNotROMH;
	}
	void ClearHighROM(void)
	{
		sLatchStates[kLatch] |= kNotROMH;
	}
	// Normal logic again
	void SetPHI2(void)
	{
		sLatchStates[kLatch] |= kPHI2;
	}
	void ClearPHI2(void)
	{
		sLatchStates[kLatch] &= ~kPHI2;
	}
	void SetRead(void)
	{
		sLatchStates[kLatch] |= kReadNotWrite;
	}
	void SetWrite(void)
	{
		sLatchStates[kLatch] &= ~kReadNotWrite;
	}
	void UpdateLatch(void)
	{
		SetOutputByte(sLatchStates[kLatch]);
		WriteLatch(kLatch);
	}

	void SetFlashWrite(void)
	{
		sLatchStates[kLatch] |= kFlashWriteEnable;	// Compatibility with MegaCart V1.0
		if (flashWriteCommandIsHiROM)
		{
			SetHighROM();	// MegaCart V2.0
		}
		else
		{
			SetLowROM();	// Megabyter
		}
	}
	void ClearFlashWrite(void)
	{
		sLatchStates[kLatch] &= ~kFlashWriteEnable;	// Compatibility with MegaCart V1.0
		if (flashWriteCommandIsHiROM)
		{
			ClearHighROM();	// MegaCart V2.0
		}
		else
		{
			ClearLowROM();	// Megabyter
		}
	}

	void ToggleDotClock(void)
	{
		if (flashWriteToggleDotClockIterations <= 0)
		{
			return;
		}
		int iters = flashWriteToggleDotClockIterations;
		do
		{
			iters--;
			sLatchStates[kLatch] |= kFlashWriteEnable;
			UpdateLatch();
			sLatchStates[kLatch] &= ~kFlashWriteEnable;
			UpdateLatch();
		} while (iters > 0);
	}
}

namespace InterfaceControl
{
	const int kLatch = 4;
	const int kNotReset = 0b00000001;
	const int kLED0 = 0b00010000;
	const int kLED1 = 0b00100000;
	const int kLED2 = 0b01000000;
	const int kRelay1 = 0b10000000;

	void UpdateLatch(void)
	{
		SetOutputByte(sLatchStates[kLatch]);
		WriteLatch(kLatch);
	}

	// Note active low logic
	void SetReset(void)
	{
		sLatchStates[kLatch] &= ~kNotReset;
	}
	void ClearReset(void)
	{
		sLatchStates[kLatch] |= kNotReset;
	}
	// Normal logic
	void SetLED0(void)
	{
		sLatchStates[kLatch] |= kLED0;
	}
	void ClearLED0(void)
	{
		sLatchStates[kLatch] &= ~kLED0;
	}
	void SetLED1(void)
	{
		sLatchStates[kLatch] |= kLED1;
	}
	void ClearLED1(void)
	{
		sLatchStates[kLatch] &= ~kLED1;
	}
	void SetLED2(void)
	{
		sLatchStates[kLatch] |= kLED2;
	}
	void ClearLED2(void)
	{
		sLatchStates[kLatch] &= ~kLED2;
	}
	void SetRelay1(void)
	{
		sLatchStates[kLatch] |= kRelay1;
	}
	void ClearRelay1(void)
	{
		sLatchStates[kLatch] &= ~kRelay1;
	}
	void SetActiveSlot(const int slot)
	{
		sLatchStates[kLatch] &= ~(0x07 << 1);
		sLatchStates[kLatch] |= slot << 1;

		// For performance reasons we only do this when more than one active slot is configured
		if (activeSlots > 1)
		{
			UpdateLatch();
		}
	}
}

void ShowCartridgeState()
{
	printf("_GAME=%d _EXROM=%d\n", digitalRead(26), digitalRead(25));
}


void InitCartTool(void)
{
	InterfaceControl::SetReset();
	InterfaceControl::ClearLED0();
	InterfaceControl::ClearLED1();
	InterfaceControl::ClearLED2();
	InterfaceControl::ClearRelay1();
	InterfaceControl::UpdateLatch();

	C64Control::ClearDataLatchOut();
	C64Control::ClearFlashWrite();
	C64Control::ClearIO1();
	C64Control::ClearIO2();
	C64Control::ClearLowROM();
	C64Control::ClearHighROM();
	C64Control::SetPHI2();
	C64Control::SetRead();
	C64Control::UpdateLatch();

	InterfaceControl::ClearReset();
	InterfaceControl::UpdateLatch();
}

void SetDataIO1(int address, int data)
{
	DataLatchOut::SetAddress(address);
	DataLatchOut::SetData(data);
	C64Control::SetIO1();
	C64Control::UpdateLatch();

	C64Control::SetDataLatchOut();
	C64Control::SetWrite();
	C64Control::UpdateLatch();
	C64Control::ToggleDotClock();
	delayMicroseconds(1);
	C64Control::SetRead();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();
	C64Control::ClearIO1();
	C64Control::UpdateLatch();
	C64Control::ToggleDotClock();
}

void SetDataIO2(int data)
{
	DataLatchOut::SetData(data);
	C64Control::SetIO2();
	C64Control::UpdateLatch();

	C64Control::SetDataLatchOut();
	C64Control::SetWrite();
	C64Control::UpdateLatch();
	delayMicroseconds(1);
	C64Control::SetRead();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();
	C64Control::ClearIO2();
	C64Control::UpdateLatch();
}

void SetDataIO2(int address, int data)
{
	DataLatchOut::SetAddress(address);
	DataLatchOut::SetData(data);
	C64Control::SetIO2();
	C64Control::UpdateLatch();

	C64Control::SetDataLatchOut();
	C64Control::SetWrite();
	C64Control::UpdateLatch();
	C64Control::ToggleDotClock();
	delayMicroseconds(1);
	C64Control::SetRead();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();
	C64Control::ClearIO2();
	C64Control::UpdateLatch();
	C64Control::ToggleDotClock();
}

// Writes with a rising PHI2 edge while the select line is active, the EasyFlash 3 CPLD needs this for register and flash writes
// Busy waits at least the given time, delayMicroseconds() has a resolution of 1us and often waits longer
static void DelayNanoseconds(long nanoseconds)
{
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	do
	{
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while ((now.tv_sec - start.tv_sec) * 1000000000L + (now.tv_nsec - start.tv_nsec) < nanoseconds);
}

static void WriteWithPHI2Edge(int address, int data, void (*select)(void), void (*deselect)(void))
{
	DataLatchOut::SetAddress(address);
	DataLatchOut::SetData(data);

	// PHI2 goes low while no select line is active, otherwise the EasyFlash 3 drives the data bus.
	// R/W and the data output change in the same latch update, nothing is selected yet.
	C64Control::ClearPHI2();
	C64Control::SetDataLatchOut();
	C64Control::SetWrite();
	C64Control::UpdateLatch();

	// PHI2 and the select lines are in the same latch, so the rising edge and the select happen at the same time
	C64Control::SetPHI2();
	select();
	C64Control::UpdateLatch();
	// The CPLD writes about 280 to 480ns after the rising edge (wp to wp_end in exp_bus_ctrl.vhdl)
	DelayNanoseconds(600);
	deselect();
	C64Control::UpdateLatch();
	C64Control::SetRead();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();
}

void SetDataIO1WithPHI2Edge(int address, int data)
{
	WriteWithPHI2Edge(address, data, C64Control::SetIO1, C64Control::ClearIO1);
}

void SetDataROMLWithPHI2Edge(int address, int data)
{
	WriteWithPHI2Edge(address, data, C64Control::SetLowROM, C64Control::ClearLowROM);
}

void SetDataROMHWithPHI2Edge(int address, int data)
{
	WriteWithPHI2Edge(address, data, C64Control::SetHighROM, C64Control::ClearHighROM);
}

int GetDataIO1(int address)
{
	DataLatchOut::SetAddress(address);
	C64Control::SetIO1();
	C64Control::UpdateLatch();
	delayMicroseconds(1);
	int value = GetInputByte();
	C64Control::ClearIO1();
	C64Control::UpdateLatch();

	// The EasyFlash 3 keeps the register value on the data bus until the next PHI2 edge
	C64Control::ClearPHI2();
	C64Control::UpdateLatch();
	C64Control::SetPHI2();
	C64Control::UpdateLatch();

	return value;
}

static int ReadWithDelay(int address, void (*select)(void), void (*deselect)(void))
{
	DataLatchOut::SetAddress(address);
	select();
	C64Control::UpdateLatch();
	delayMicroseconds(1);
	int value = GetInputByte();
	deselect();
	C64Control::UpdateLatch();

	return value;
}

int GetDataROML(int address)
{
	return ReadWithDelay(address, C64Control::SetLowROM, C64Control::ClearLowROM);
}

int GetDataROMH(int address)
{
	return ReadWithDelay(address, C64Control::SetHighROM, C64Control::ClearHighROM);
}

void EF3FlashCommand(int command)
{
	// As in eapi-mx29640b.s the commands go to slot 0, bank 0, ROML $8aaa and $8555
	SetDataIO1WithPHI2Edge(1, 0);
	SetDataIO1WithPHI2Edge(0, 0);
	SetDataROMLWithPHI2Edge(0xaaa, 0xaa);
	SetDataROMLWithPHI2Edge(0x555, 0x55);
	SetDataROMLWithPHI2Edge(0xaaa, command);
}

bool EF3SetSlotAndBank(int slot, int bank)
{
	SetDataIO1WithPHI2Edge(1, slot);
	SetDataIO1WithPHI2Edge(0, bank);

	// If the slot did not change, the next flash command would go to the system area in slot 0.
	// One wrong read was seen once on the hardware, so the register is written and read once more before giving up.
	if ((GetDataIO1(1) & 0x07) == slot)
	{
		return true;
	}
	SetDataIO1WithPHI2Edge(1, slot);
	return (GetDataIO1(1) & 0x07) == slot;
}

bool EF3WaitForFlash(bool high, int address, int value, int timeoutMicroseconds)
{
	// While the flash is busy DQ7 is the complement of the final value, so the value only matches when it is done
	for (int waited = 0; waited < timeoutMicroseconds; waited++)
	{
		int got = high ? GetDataROMH(address) : GetDataROML(address);
		if (got == value)
		{
			return true;
		}
	}
	return false;
}
