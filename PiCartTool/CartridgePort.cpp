#include <stdio.h>
#include <wiringPi.h>
#include "Hardware.h"

static int sLatchStates[5] = { -1,-1,-1,0,0 };

int activeSlots = 1;

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
