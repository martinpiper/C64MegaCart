// Page numbers reference "For M29F160FT55N3E2 - M29FxxFT_FB-2999423.pdf"
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
	digitalWrite(16 + latch, LOW);
	digitalWrite(16 + latch, LOW);
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
	const int kLatch = 3;
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
		sLatchStates[kLatch] |= kDataLatchOut;
	}
	void ClearDataLatchOut(void)
	{
		sLatchStates[kLatch] &= ~kDataLatchOut;
	}

	void SetFlashWrite(void)
	{
		sLatchStates[kLatch] |= kFlashWriteEnable;
	}
	void ClearFlashWrite(void)
	{
		sLatchStates[kLatch] &= ~kFlashWriteEnable;
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
}

namespace InterfaceControl
{
	const int kLatch = 4;
	const int kNotReset			= 0b00000001;
	const int kLED0				= 0b00010000;
	const int kLED1				= 0b00100000;
	const int kLED2				= 0b01000000;
	const int kLED3				= 0b10000000;

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
	void SetLED3(void)
	{
		sLatchStates[kLatch] |= kLED3;
	}
	void ClearLED3(void)
	{
		sLatchStates[kLatch] &= ~kLED3;
	}
	void UpdateLatch(void)
	{
		SetOutputByte(sLatchStates[kLatch]);
		WriteLatch(kLatch);
	}
}

void SendChipCommand(int address, int data)
{
	DataLatchOut::SetAddress(address);
	DataLatchOut::SetData(data);
	C64Control::SetDataLatchOut();
	C64Control::SetFlashWrite();
	C64Control::UpdateLatch();
//	delayMicroseconds(1);
	C64Control::ClearFlashWrite();
//	C64Control::UpdateLatch();
	// Will clear flash write a little before the data output, this might generate a momentary logic contention state
	// TODO: See if both the write and the latch out can be cleared at the same time
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();
}

void WaitForStatusRegisterEqual(int waitFor)
{
	int statusRegister = 0;
	int iterations = 0;
	do
	{
		C64Control::SetLowROM();
		C64Control::UpdateLatch();
		delay(1);	// Certainly more than the 20ns for a bus read
		statusRegister = GetInputByte();
		int ryby = digitalRead(27);
		printf("statusRegister $%x RYBY %d iterations %d\n", statusRegister, ryby, iterations++);
		C64Control::ClearLowROM();
		C64Control::UpdateLatch();
		delay(250);
	} while (statusRegister != waitFor);
}

int main(void)
{
	printf("wiringPiSetupSys\n");
	wiringPiSetupGpio();
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
	pinMode(25, INPUT);	// _EXROM
	pinMode(26, INPUT);	// _GAME
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

#if 0
	while (true)
	{
		InterfaceControl::ClearLED0();
		InterfaceControl::ClearLED1();
		InterfaceControl::ClearLED2();
		InterfaceControl::ClearLED3();
		InterfaceControl::UpdateLatch();
		InterfaceControl::SetLED0();
		InterfaceControl::SetLED1();
		InterfaceControl::SetLED2();
		InterfaceControl::SetLED3();
		InterfaceControl::UpdateLatch();
		C64Control::SetIO1();
		C64Control::UpdateLatch();
		C64Control::ClearIO1();
		C64Control::UpdateLatch();
		C64Control::SetIO2();
		C64Control::UpdateLatch();
		C64Control::ClearIO2();
		C64Control::UpdateLatch();
		C64Control::SetLowROM();
		C64Control::UpdateLatch();
		C64Control::ClearLowROM();
		C64Control::UpdateLatch();
		C64Control::SetHighROM();
		C64Control::UpdateLatch();
		C64Control::ClearHighROM();
		C64Control::UpdateLatch();
	}
#endif

	printf("_GAME=%d\n", digitalRead(26));
	printf("_EXROM=%d\n", digitalRead(25));

	printf("Running...\n");
	// Write some data to the flash, using the program command sequence

	// Erase commands
	SendChipCommand(0xaaa , 0xaa);
	SendChipCommand(0x555, 0x55);
	SendChipCommand(0xaaa, 0x80);
	SendChipCommand(0xaaa, 0xaa);
	SendChipCommand(0x555, 0x55);
	SendChipCommand(0xaaa, 0x10);
	WaitForStatusRegisterEqual(0xff);

	FILE* fp = fopen("../../../scrollerbanks.bin" , "rb");
	unsigned char bankData[8192];

	int bank = 0;
	while (!feof(fp))
	{
		int numBytes = fread(bankData, sizeof(bankData[0]), sizeof(bankData), fp);
		if (numBytes <= 0)
		{
			break;
		}
		printf("Got bytes %d for bank %d\n", numBytes, bank);

		// Set the bank register
		DataLatchOut::SetAddress(0);
		DataLatchOut::SetData(bank);
		C64Control::SetIO1();
		C64Control::UpdateLatch();

		C64Control::SetDataLatchOut();
		C64Control::SetWrite();
		C64Control::UpdateLatch();
		delayMicroseconds(1);
		C64Control::SetRead();
//		C64Control::UpdateLatch();
		// TODO: See if both the write and the latch out can be cleared at the same time
		C64Control::ClearDataLatchOut();
		C64Control::UpdateLatch();
		C64Control::ClearIO1();
		C64Control::UpdateLatch();

		for (int address = 0; address < (int)sizeof(bankData); address++)
		{
			if ((address & 0xff) == 0)
			{
				printf(".");
				fflush(stdout);
			}

			// If it's going to be the same as an erased flash byte then skip it :)
			if (bankData[address] == 0xff)
			{
				continue;
			}

			// Program commands
			SendChipCommand(0xaaa, 0xaa);
			SendChipCommand(0x555, 0x55);
			SendChipCommand(0xaaa, 0xa0);

			// Program command4 (the actual byte)
			DataLatchOut::SetAddress(address);
			int theWriteValue = bankData[address];
			DataLatchOut::SetData(theWriteValue);
			// Page 37: During Program operations the Data Polling Bit outputs the complement of the bit being programmed to DQ7.
			C64Control::SetDataLatchOut();
			C64Control::SetFlashWrite();
			C64Control::UpdateLatch();
			delayMicroseconds(1);
			C64Control::ClearFlashWrite();
//			C64Control::UpdateLatch();
			C64Control::ClearDataLatchOut();
			C64Control::UpdateLatch();

			int statusRegister = 0;
			int iterations = 0;
			do
			{
				C64Control::SetLowROM();
				C64Control::UpdateLatch();
//				delay(0);	// Certainly more than the 20ns for a bus read
				if (iterations > 50)
				{
					delayMicroseconds(1);
				}
				statusRegister = GetInputByte();
				C64Control::ClearLowROM();
				C64Control::UpdateLatch();
				if (iterations++ > 100)
				{
					printf("There seems to be a problem verifying the byte at address $%04x\n", address);
					exit(-1);
				}
			} while (statusRegister != theWriteValue);
		}

		printf("\nBank done\n");
		bank++;
	}

	InterfaceControl::SetLED0();
	InterfaceControl::UpdateLatch();

	return 0;
}
