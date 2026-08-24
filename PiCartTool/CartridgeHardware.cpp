// Page numbers reference "For M29F160FT55N3E2 - M29FxxFT_FB-2999423.pdf"
#include <stdio.h>
#include <wiringPi.h>
#include "Hardware.h"
#include "CartridgePort.h"
#include "CartridgeHardware.h"
#include "ProgrammerHardware.h"

// Some cartridges need a slightly longer delay for accurate writes
int RegisterDelayMicroSeconds = 0;
// Some cartridges need extra clocking of output signals to simulate slightly staggered bus signals from the C64.
// This is slower but more accurate.
bool flashWriteCommandExtraClocking = false;

FlashChipType flashChipType = M29F160;

void ResetCartridge(void)
{
	// Reset the cartridge before any operations
	printf("Before reset\n");
	ShowCartridgeState();
	InterfaceControl::SetReset();
	InterfaceControl::UpdateLatch();
	printf("During reset\n");
	ShowCartridgeState();
	InterfaceControl::ClearReset();
	InterfaceControl::UpdateLatch();
	printf("After reset\n");
	ShowCartridgeState();
	ClearCartridgeIO();
	printf("After IO clear\n");
	ShowCartridgeState();
}

void ClearCartridgeIO(void)
{
	// Clear any cartridge IO, to common values
	for (int i = 0; i < 256; i++)
	{
		SetDataIO1(i, 0);
		SetDataIO2(i, 0);
	}
}

void SendChipCommand(int address, int data)
{
	DataLatchOut::SetAddress(address);
	DataLatchOut::SetData(data);

	if (flashWriteCommandExtraClocking)
	{
		C64Control::SetDataLatchOut();
		C64Control::UpdateLatch();
		C64Control::SetWrite();
		C64Control::UpdateLatch();
		C64Control::SetFlashWrite();
		C64Control::UpdateLatch();
		C64Control::ToggleDotClock();
		safeDelayMicroseconds(RegisterDelayMicroSeconds);
		C64Control::ClearFlashWrite();
		C64Control::UpdateLatch();
		C64Control::SetRead();
		C64Control::UpdateLatch();
		C64Control::ToggleDotClock();
		// Will clear flash write a little before the data output, this might generate a momentary logic contention state
			// TODO: See if both the write and the latch out can be cleared at the same time
		C64Control::ClearDataLatchOut();
		C64Control::UpdateLatch();
	}
	else
	{
		C64Control::SetDataLatchOut();
		C64Control::SetFlashWrite();
		C64Control::UpdateLatch();
		C64Control::ToggleDotClock();
		safeDelayMicroseconds(RegisterDelayMicroSeconds);
		C64Control::ClearFlashWrite();
		C64Control::ToggleDotClock();

		//	C64Control::UpdateLatch();
			// Will clear flash write a little before the data output, this might generate a momentary logic contention state
			// TODO: See if both the write and the latch out can be cleared at the same time
		C64Control::ClearDataLatchOut();
		C64Control::UpdateLatch();
		C64Control::ToggleDotClock();
	}
}


void SendChipCommandErase(void)
{
	// Write some data to the flash, using the erase command sequence
	switch (flashChipType)
	{
	case M29F160:
		SendChipCommand(0xaaa, 0xaa);
		SendChipCommand(0x555, 0x55);
		SendChipCommand(0xaaa, 0x80);
		SendChipCommand(0xaaa, 0xaa);
		SendChipCommand(0x555, 0x55);
		SendChipCommand(0xaaa, 0x10);
		break;

	case M29F040:
		SendChipCommand(0x555, 0xaa);
		SendChipCommand(0x2aa, 0x55);
		SendChipCommand(0x555, 0x80);
		SendChipCommand(0x555, 0xaa);
		SendChipCommand(0x2aa, 0x55);
		SendChipCommand(0x555, 0x10);
		break;
	}
}

void SendChipCommandBlockErase(void)
{
	// Block erase commands
	// The actual block (sector in some datasheets) to erase usually comes from the bank register(s), from the upper address bits
	switch (flashChipType)
	{
	case M29F160:
		SendChipCommand(0xaaa, 0xaa);
		SendChipCommand(0x555, 0x55);
		SendChipCommand(0xaaa, 0x80);
		SendChipCommand(0xaaa, 0xaa);
		SendChipCommand(0x555, 0x55);
		SendChipCommand(0xba, 0x30);
		break;
	case M29F040:
		SendChipCommand(0x555, 0xaa);
		SendChipCommand(0x2aa, 0x55);
		SendChipCommand(0x555, 0x80);
		SendChipCommand(0x555, 0xaa);
		SendChipCommand(0x2aa, 0x55);
		SendChipCommand(0xba, 0x30);
		break;
	}
}

void SendChipCommandProgram(void)
{
	switch (flashChipType)
	{
	case M29F160:
		SendChipCommand(0xaaa, 0xaa);
		SendChipCommand(0x555, 0x55);
		SendChipCommand(0xaaa, 0xa0);
		break;
	case M29F040:
		SendChipCommand(0x555, 0xaa);
		SendChipCommand(0x2aa, 0x55);
		SendChipCommand(0x555, 0xa0);
		break;
	}
}

void PerformCartridgeWriteCycle(void)
{
	if (flashWriteCommandExtraClocking)
	{
		C64Control::SetDataLatchOut();
		C64Control::UpdateLatch();
		C64Control::SetWrite();
		C64Control::UpdateLatch();
		C64Control::SetFlashWrite();
		C64Control::UpdateLatch();
		C64Control::ToggleDotClock();
		safeDelayMicroseconds(RegisterDelayMicroSeconds);
		C64Control::ClearFlashWrite();
		C64Control::UpdateLatch();
		C64Control::SetRead();
		C64Control::UpdateLatch();
		C64Control::ToggleDotClock();
		C64Control::ClearDataLatchOut();
		C64Control::UpdateLatch();

		C64Control::ClearFlashWrite();
		C64Control::ClearDataLatchOut();
		C64Control::UpdateLatch();
	}
	else
	{
		// Page 37: During Program operations the Data Polling Bit outputs the complement of the bit being programmed to DQ7.
		C64Control::SetDataLatchOut();
		C64Control::SetFlashWrite();
		C64Control::UpdateLatch();
		delayMicroseconds(1);
		C64Control::ClearFlashWrite();
		//			C64Control::UpdateLatch();
		C64Control::ClearDataLatchOut();
		C64Control::UpdateLatch();
	}
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


// Can be configurable later on...
int kSerialEEPROM_DataOut = 0b10000000;
int kSerialEEPROM_ChipSelect = 0b01000000;
int kSerialEEPROM_Clock = 0b00100000;
int kSerialEEPROM_DataIn = 0b00010000;
int kSerialEEPROM_IOAddress = 0;
int kSerialEEPROM_AddressBits = 10;
int serialState = 0;

void SerialEEPROM_Init(void)
{
	SetDataIO1(0, 0);
	SetDataIO2(0);

	C64Control::ClearHighROM();
	C64Control::ClearLowROM();

	C64Control::SetIO1();
	C64Control::UpdateLatch();
}

void SerialEEPROM_DoClock(void)
{
	SetDataIO1(kSerialEEPROM_IOAddress, serialState);
	serialState |= kSerialEEPROM_Clock;
	SetDataIO1(kSerialEEPROM_IOAddress, serialState);
	serialState &= ~kSerialEEPROM_Clock;
	// But no need to send serialState yet... Wait for the next data...
}

void SerialEEPROM_Reset(void)
{
	SetDataIO1(kSerialEEPROM_IOAddress, 0);
	serialState = kSerialEEPROM_ChipSelect;
	SetDataIO1(kSerialEEPROM_IOAddress, serialState);
}

void SerialEEPROM_SendBit(const int bit)
{
	serialState = kSerialEEPROM_ChipSelect;
	if (bit)
	{
		serialState |= kSerialEEPROM_DataIn;
	}
	SetDataIO1(kSerialEEPROM_IOAddress, serialState);
	SerialEEPROM_DoClock();
}

void SerialEEPROM_SendAddress(const int address)
{
	for (int i = kSerialEEPROM_AddressBits - 1; i >= 0; i--)
	{
		SerialEEPROM_SendBit(address & (1 << i));
	}
}

int SerialEEPROM_ReadByte(void)
{
	int theByte = 0;
	for (int i = 0; i < 8; i++)
	{
		C64Control::ClearDataLatchOut();
		C64Control::SetRead();
		C64Control::SetIO1();
		C64Control::UpdateLatch();
		int byte = GetInputByte();
		theByte <<= 1;
		if (byte & kSerialEEPROM_DataOut)
		{
			//			printf("1"); // Debug
			theByte |= 1;
		}
		else
		{
			//			printf("0"); // Debug
		}
		SerialEEPROM_DoClock();
	}
	return theByte;
}

bool SerialEEPROM_WaitForReady(void)
{
	SerialEEPROM_Reset();
	int iterations = 0;
	C64Control::ClearDataLatchOut();
	C64Control::SetRead();
	C64Control::SetIO1();
	C64Control::UpdateLatch();
	while (iterations < 5000)
	{
		int byte = GetInputByte();
		//		printf("%02x ", byte);
		//		fflush(stdout);
		if ((byte & kSerialEEPROM_DataOut))
		{
			//			printf("EEPROM ready %d\n", iterations);
			return true;
		}
		delay(1);
		C64Control::ToggleDotClock();
		iterations++;
	}
	printf("SerialEEPROM_WaitForReady had problems\n");
	return false;
}

void SerialEEPROM_WaitForReadyAllslots(bool& gotError)
{
	for (int slot = 0; slot < activeSlots; slot++)
	{
		if (gotError)
		{
			break;
		}
		InterfaceControl::SetActiveSlot(slot);

		if (!SerialEEPROM_WaitForReady())
		{
			ReportCartridgeError();
			gotError = true;
			break;
		}
	}
}

void SerialEEPROM_SendWriteEnable(void)
{
	// Reset instruction
	SerialEEPROM_Reset();
	// Start bit
	SerialEEPROM_SendBit(1);

	// Write Enable command
	SerialEEPROM_SendBit(0);
	SerialEEPROM_SendBit(0);

	// Send expected address...
	SerialEEPROM_SendBit(1);
	SerialEEPROM_SendBit(1);
	for (int i = 0; i < kSerialEEPROM_AddressBits - 2; i++)
	{
		SerialEEPROM_SendBit(0);
	}
}

void SerialEEPROM_SendByte(int byte)
{
	for (int i = 7; i >= 0; i--)
	{
		SerialEEPROM_SendBit(byte & (1 << i));
	}
}



// Bits of test code

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
	InterfaceControl::ClearRelay1();
	InterfaceControl::UpdateLatch();
	delay(1000);
	//		InterfaceControl::SetLED0();
	//		InterfaceControl::SetLED1();
	InterfaceControl::SetLED2();
	InterfaceControl::SetRelay1();
	InterfaceControl::UpdateLatch();
	delay(1000);
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
InterfaceControl::SetRelay1();
InterfaceControl::UpdateLatch();
#endif
