#include <stdio.h>
#include <stdlib.h>
#include <wiringPi.h>

// projects/PiCartTool/bin/ARM/Release/PiCartTool.out

int GetInputByte(void)
{
	int ret = 0;
	for (int i = 0; i < 8; i++)
	{
		int pinState = digitalRead(i);
		if (pinState == HIGH)
		{
			ret |= 1 << i;
		}
	}
	return ret;
}

static int sCachedSignals[28];

void SetOutputByte(int value)
{
	for (int i = 8; i < 16; i++)
	{
		int intendedSignal = value & 0x01;
		if (sCachedSignals[i] != intendedSignal)
		{
			digitalWrite(i, intendedSignal ? HIGH : LOW);
			sCachedSignals[i] = intendedSignal;
		}

		value >>= 1;
	}
}

void WriteLatch(int latch)
{
	digitalWrite(16 + latch, LOW);
	digitalWrite(16 + latch, HIGH);
}

static int sLatchStates[5] = { -1,-1,-1,0,0 };

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
	const int kDataLatchOut		= 0b00000001;
	const int kFlashWriteEnable = 0b00000010;
	const int kNotIO1			= 0b00000100;
	const int kNotIO2			= 0b00001000;
	const int kPHI2				= 0b00010000;	// High when the CPU can access memory, low is when VIC accesses memory
	const int kReadNotWrite		= 0b00100000;
	const int kNotROML			= 0b01000000;	// Usually connected to memory _OE
	const int kNotROMH			= 0b10000000;

	void SetDataLatchOut(void)
	{
		sLatchStates[3] |= kDataLatchOut;
	}
	void ClearDataLatchOut(void)
	{
		sLatchStates[3] &= ~kDataLatchOut;
	}

	void SetFlashWrite(void)
	{
		sLatchStates[3] |= kFlashWriteEnable;
	}
	void ClearFlashWrite(void)
	{
		sLatchStates[3] &= ~kFlashWriteEnable;
	}
	// Note active low logic
	void SetIO1(void)
	{
		sLatchStates[3] &= ~kNotIO1;
	}
	void ClearIO1(void)
	{
		sLatchStates[3] |= kNotIO1;
	}
	void SetIO2(void)
	{
		sLatchStates[3] &= ~kNotIO2;
	}
	void ClearIO2(void)
	{
		sLatchStates[3] |= kNotIO2;
	}
	void SetLowROM(void)
	{
		sLatchStates[3] &= ~kNotROML;
	}
	void ClearLowROM(void)
	{
		sLatchStates[3] |= kNotROML;
	}
	void SetHighROM(void)
	{
		sLatchStates[3] &= ~kNotROMH;
	}
	void ClearHighROM(void)
	{
		sLatchStates[3] |= kNotROMH;
	}
	// Normal logic again
	void SetPHI2(void)
	{
		sLatchStates[3] |= kPHI2;
	}
	void ClearPHI2(void)
	{
		sLatchStates[3] &= ~kPHI2;
	}
	void SetRead(void)
	{
		sLatchStates[3] |= kReadNotWrite;
	}
	void SetWrite(void)
	{
		sLatchStates[3] &= ~kReadNotWrite;
	}
	void UpdateLatch(void)
	{
		SetOutputByte(sLatchStates[3]);
		WriteLatch(3);
	}
}

namespace InterfaceControl
{
	const int kNotReset			= 0b00000001;
	const int kLED0				= 0b00010000;
	const int kLED1				= 0b00100000;
	const int kLED2				= 0b01000000;
	const int kLED3				= 0b10000000;

	// Note active low logic
	void SetReset(void)
	{
		sLatchStates[4] &= ~kNotReset;
	}
	void ClearReset(void)
	{
		sLatchStates[4] |= ~kNotReset;
	}
	// Normal logic
	void SetLED0(void)
	{
		sLatchStates[4] |= kLED0;
	}
	void ClearLED0(void)
	{
		sLatchStates[4] &= ~kLED0;
	}
	void SetLED1(void)
	{
		sLatchStates[4] |= kLED1;
	}
	void ClearLED1(void)
	{
		sLatchStates[4] &= ~kLED1;
	}
	void SetLED2(void)
	{
		sLatchStates[4] |= kLED2;
	}
	void ClearLED2(void)
	{
		sLatchStates[4] &= ~kLED2;
	}
	void SetLED3(void)
	{
		sLatchStates[4] |= kLED3;
	}
	void ClearLED3(void)
	{
		sLatchStates[4] &= ~kLED3;
	}
	void UpdateLatch(void)
	{
		SetOutputByte(sLatchStates[4]);
		WriteLatch(4);
	}
}

int main(void)
{
	printf("wiringPiSetupSys\n");
	wiringPiSetupSys();
	for (int i = 0; i < 28; i++)
	{
		sCachedSignals[i] = 0;
	}

	printf("pinMode\n");
	// Don't forget the gpio exportx in the project post build config
	for (int i = 0; i < 8; i++)
	{
		pinMode(i, INPUT);	// D0..7
	}
	for (int i = 8; i < 25; i++)
	{
		pinMode(i, OUTPUT);
	}
	// Allows for these to be read from any plugged in cartridge
	pinMode(26, INPUT);	// _EXROM
	pinMode(25, INPUT);	// _GAME
	// Read the flash status bit, if connected
	pinMode(27, INPUT);	// RYBY

	InterfaceControl::SetReset();
	InterfaceControl::ClearLED0();
	InterfaceControl::ClearLED1();
	InterfaceControl::ClearLED2();
	InterfaceControl::ClearLED3();
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

	printf("_GAME=%d\n", digitalRead(25));
	printf("_EXROM=%d\n" , digitalRead(26));

	printf("Running...\n");
	// Write some data to the flash, using the program command sequence

	// Program command1
	DataLatchOut::SetAddress(0xaaa);
	DataLatchOut::SetData(0xaa);
	C64Control::SetDataLatchOut();
	C64Control::SetFlashWrite();
	C64Control::UpdateLatch();
	// Will clear flash write a little before the data output, this might generate a momentary logic contention state
	// TODO: See if both the write and the latch out can be cleared at the same time
	C64Control::ClearFlashWrite();
	C64Control::UpdateLatch();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();

	// Program command2
	DataLatchOut::SetAddress(0x555);
	DataLatchOut::SetData(0x55);
	C64Control::SetDataLatchOut();
	C64Control::SetFlashWrite();
	C64Control::UpdateLatch();
	// Will clear flash write a little before the data output, this might generate a momentary logic contention state
	// TODO: See if both the write and the latch out can be cleared at the same time
	C64Control::ClearFlashWrite();
	C64Control::UpdateLatch();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();

	// Program command3
	DataLatchOut::SetAddress(0xaaa);
	DataLatchOut::SetData(0xa0);
	C64Control::SetDataLatchOut();
	C64Control::SetFlashWrite();
	C64Control::UpdateLatch();
	// Will clear flash write a little before the data output, this might generate a momentary logic contention state
	// TODO: See if both the write and the latch out can be cleared at the same time
	C64Control::ClearFlashWrite();
	C64Control::UpdateLatch();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();

	// Program command4 (the actual byte)
	DataLatchOut::SetAddress(0x0);
	DataLatchOut::SetData(0x00);
	C64Control::SetDataLatchOut();
	C64Control::SetFlashWrite();
	C64Control::UpdateLatch();
	// Will clear flash write a little before the data output, this might generate a momentary logic contention state
	// TODO: See if both the write and the latch out can be cleared at the same time
	C64Control::ClearFlashWrite();
	C64Control::UpdateLatch();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();

	// Now read the result
	C64Control::ClearDataLatchOut();
	C64Control::SetLowROM();
	C64Control::UpdateLatch();

	while (true)
	{
		printf("GetInputByte $%x\n", GetInputByte());
		delay(250);
	}

	return 0;
}
