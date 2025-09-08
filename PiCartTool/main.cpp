// Page numbers reference "For M29F160FT55N3E2 - M29FxxFT_FB-2999423.pdf"
#include <stdio.h>
#include <stdlib.h>
#include <wiringPi.h>
#include <atomic>
#include <algorithm>

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

static volatile std::atomic<int> sCachedSignals[28];

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
		SetHighROM();	// MegaCart V2.0
	}
	void ClearFlashWrite(void)
	{
		sLatchStates[kLatch] &= ~kFlashWriteEnable;	// Compatibility with MegaCart V1.0
		ClearHighROM();	// MegaCart V2.0
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

void InitDevice(void)
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
		pullUpDnControl(i, PUD_OFF);
	}
	for (int i = 8; i < 24; i++)
	{
		pinMode(i, OUTPUT);
	}
	pinMode(24, INPUT);	// PButton
	pullUpDnControl(24, PUD_UP);
	// Allows for these to be read from any plugged in cartridge
	pinMode(25, INPUT);	// _EXROM
	pullUpDnControl(25, PUD_OFF);
	pinMode(26, INPUT);	// _GAME
	pullUpDnControl(26, PUD_OFF);
	// Read the flash status bit, if connected
	pinMode(27, INPUT);	// RYBY
	pullUpDnControl(27, PUD_OFF);
}

void InitCartTool(void)
{
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
	delayMicroseconds(1);
	C64Control::SetRead();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();
	C64Control::ClearIO1();
	C64Control::UpdateLatch();
}

int main(void)
{
	InitDevice();
	InitCartTool();

#if 0
	while (true)
	{
		delay(500);

		printf("Read %d %d %d %d\n", digitalRead(24), digitalRead(25), digitalRead(26), digitalRead(27));
	}
#endif

#if 0
	while (true)
	{
		InterfaceControl::ClearLED0();
		InterfaceControl::ClearLED1();
		InterfaceControl::ClearLED2();
		InterfaceControl::ClearLED3();
		InterfaceControl::UpdateLatch();
		delay(100);
		InterfaceControl::SetLED0();
		InterfaceControl::SetLED1();
		InterfaceControl::SetLED2();
		InterfaceControl::SetLED3();
		InterfaceControl::UpdateLatch();
		delay(100);
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

		printf("Read %d %d %d %d\n", digitalRead(24), digitalRead(25), digitalRead(26), digitalRead(27));
	}
#endif

#if 0
	InterfaceControl::SetLED0();
	InterfaceControl::SetLED1();
	InterfaceControl::SetLED2();
	InterfaceControl::SetLED3();
	InterfaceControl::UpdateLatch();
#endif

	printf("Before reset\n");
	printf("_GAME=%d\n", digitalRead(26));
	printf("_EXROM=%d\n", digitalRead(25));
	InterfaceControl::SetReset();
	InterfaceControl::UpdateLatch();
	printf("During reset\n");
	printf("_GAME=%d\n", digitalRead(26));
	printf("_EXROM=%d\n", digitalRead(25));
	InterfaceControl::ClearReset();
	InterfaceControl::UpdateLatch();
	printf("After reset\n");
	printf("_GAME=%d\n", digitalRead(26));
	printf("_EXROM=%d\n", digitalRead(25));

	unsigned char bankData[8192];
	FILE* fp;

#if 0
	InterfaceControl::SetLED0();
	InterfaceControl::ClearLED1();
	InterfaceControl::UpdateLatch();

	printf("Writing...\n");
	// Write some data to the flash, using the erase and program command sequence
	// Erase commands
	SendChipCommand(0xaaa, 0xaa);
	SendChipCommand(0x555, 0x55);
	SendChipCommand(0xaaa, 0x80);
	SendChipCommand(0xaaa, 0xaa);
	SendChipCommand(0x555, 0x55);
	SendChipCommand(0xaaa, 0x10);
	WaitForStatusRegisterEqual(0xff);

	fp = fopen("../../../scrollerbanks.bin" , "rb");

	int bank = 0;
	while (!feof(fp))
	{
		size_t numBytes = fread(bankData, sizeof(bankData[0]), sizeof(bankData), fp);
		if (numBytes == 0)
		{
			break;
		}
		printf("Got bytes %d for bank %d\n", numBytes, bank);

		// Set the bank register
		SetDataIO1(0, bank);

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

	InterfaceControl::SetLED1();
	InterfaceControl::UpdateLatch();
#endif

#if 0
	InterfaceControl::SetLED0();
	InterfaceControl::ClearLED1();
	InterfaceControl::UpdateLatch();

	printf("Writing one block...\n");
	// Write some data to the flash, using the erase and program command sequence
	SetDataIO1(0, 0xfd); // Set bank $fd which equates to the 8KB block at $1fa000

	// Block erase commands
	SendChipCommand(0xaaa, 0xaa);
	SendChipCommand(0x555, 0x55);
	SendChipCommand(0xaaa, 0x80);
	SendChipCommand(0xaaa, 0xaa);
	SendChipCommand(0x555, 0x55);
	SendChipCommand(0xba, 0x30);
//	DataLatchOut::SetAddress(0);
	WaitForStatusRegisterEqual(0xff);

	// Program command
	SendChipCommand(0xaaa, 0xaa);
	SendChipCommand(0x555, 0x55);
	SendChipCommand(0xaaa, 0xa0);

	// Program command4 (the actual byte)
	DataLatchOut::SetAddress(0);
	int theWriteValue = 0x02;
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
			printf("There seems to be a problem verifying the byte at address $%04x\n", 0/*address*/);
			exit(-1);
		}
	} while (statusRegister != theWriteValue);

	printf("\nBlock done\n");

	InterfaceControl::SetLED1();
	InterfaceControl::UpdateLatch();
#endif

#if 1
	InterfaceControl::SetLED0();
	InterfaceControl::ClearLED1();
	InterfaceControl::UpdateLatch();

	printf("Reading...\n");
	int maxDelayNeeded = 0;
	fp = fopen("../../../readdata.bin", "wb");
	for (int bank = 0; bank < 256; bank++)
	{
		printf("Bank %d\n", bank);
		SetDataIO1(0, bank);
		for (int address = 0; address < (int)sizeof(bankData); address++)
		{
/*
			if ((address & 0x3ff) == 0)
			{
				printf(".");
				fflush(stdout);
			}
*/

			DataLatchOut::SetAddress(address);
			C64Control::ClearFlashWrite();
			C64Control::ClearDataLatchOut();

			// Tries two reads without any delay, then progressively increases the delay until we get two reads that are the same
			int gotPrevious = -1;
			int gotNow = -2;
			int progressiveDelay = 0;
			while (gotPrevious != gotNow)
			{
				gotPrevious = gotNow;

				C64Control::SetLowROM();
				C64Control::UpdateLatch();
				if (progressiveDelay >= 2)
				{
					delayMicroseconds(progressiveDelay/2);
					maxDelayNeeded = std::max(maxDelayNeeded, progressiveDelay/2);
				}
				gotNow = (unsigned char)GetInputByte();
				C64Control::ClearLowROM();
				C64Control::UpdateLatch();
				progressiveDelay++;
			}
			bankData[address] = (unsigned char)gotNow;
		}

		fwrite(bankData, sizeof(bankData[0]), sizeof(bankData), fp);
//		printf("\nBank done\n");
	}
	fclose(fp);
	printf("maxDelayNeeded = %d\n", maxDelayNeeded);

	InterfaceControl::SetLED1();
	InterfaceControl::UpdateLatch();
#endif

	InterfaceControl::SetLED3();
	InterfaceControl::UpdateLatch();

	return 0;
}
