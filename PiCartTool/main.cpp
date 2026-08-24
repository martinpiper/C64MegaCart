#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wiringPi.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <string>
#include "Hardware.h"
#include "CartridgePort.h"
#include "CartridgeHardware.h"
#include "ProgrammerHardware.h"

int main(int argc, char** argv)
{
	unsigned char bankData[8192];
	FILE* fp;

	auto start = std::chrono::steady_clock::now();

	HardwareInit();
	InitCartTool();

	int argPos = 1;
	while (argPos < argc)
	{
		// Parse cartridge options
		if (strcasecmp(argv[argPos], "--cfwch") == 0)
		{
			argPos++;
			flashWriteCommandIsHiROM = true;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfwcl") == 0)
		{
			argPos++;
			flashWriteCommandIsHiROM = false;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfwdci") == 0)
		{
			argPos++;
			flashWriteToggleDotClockIterations = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			continue;
		}
		if (strcasecmp(argv[argPos], "--crdms") == 0)
		{
			argPos++;
			RegisterDelayMicroSeconds = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfwec") == 0)
		{
			argPos++;
			flashWriteCommandExtraClocking = true;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfwnec") == 0)
		{
			argPos++;
			flashWriteCommandExtraClocking = false;
			continue;
		}
		if (strcasecmp(argv[argPos], "--cfct") == 0)
		{
			argPos++;
			flashChipType = (FlashChipType) std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			continue;
		}

		HardwareInit();
		InitCartTool();

		InterfaceControl::ClearRelay1();
		InterfaceControl::UpdateLatch();

		if (strcasecmp(argv[argPos], "--waitbutton") == 0 || strcasecmp(argv[argPos], "-b") == 0)
		{
			argPos++;
			printf("Waiting for button...\n");
			while (digitalRead(24) == HIGH)
			{
				delay(100);
				AlternateLED2();
			}
			while (digitalRead(24) == LOW)
			{
				delay(100);
				AlternateLED2();
			}
			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();
			continue;
		}

		// Default to the first slot...
		InterfaceControl::SetActiveSlot(0);
		InterfaceControl::UpdateLatch();

		// Enable power
		InterfaceControl::SetRelay1();
		InterfaceControl::UpdateLatch();
		delay(100); // A small delay to allow the relay to physically switch on the cartridge power

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

		if (strcasecmp(argv[argPos], "--erasechips") == 0 || strcasecmp(argv[argPos], "-ec") == 0)
		{
			argPos++;
			int numChips = std::stoi(argv[argPos], nullptr, 0);
			argPos++;

			InterfaceControl::SetLED0();
			InterfaceControl::UpdateLatch();

			for (int chip = 0; chip < numChips; chip++)
			{
				printf("Erasing chip %d\n" , chip);
				SetDataIO2(chip);
				SendChipCommandErase();
			}

			for (int slot = 0; slot < activeSlots; slot++)
			{
				InterfaceControl::SetActiveSlot(slot);

				for (int chip = 0; chip < numChips; chip++)
				{
					printf("Waiting for chip %d\n", chip);
					SetDataIO2(chip);
					WaitForStatusRegisterEqual(0xff);
				}

				SetDataIO2(0);
			}

			InterfaceControl::ClearLED0();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--erase") == 0 || strcasecmp(argv[argPos], "-e") == 0)
		{
			argPos++;

			InterfaceControl::SetLED0();
			InterfaceControl::UpdateLatch();

			printf("Erasing...\n");
			SendChipCommandErase();

			for (int slot = 0; slot < activeSlots; slot++)
			{
				InterfaceControl::SetActiveSlot(slot);
				WaitForStatusRegisterEqual(0xff);
			}

			InterfaceControl::ClearLED0();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--write") == 0 || strcasecmp(argv[argPos], "-w") == 0)
		{
			argPos++;

			InterfaceControl::SetLED1();
			InterfaceControl::UpdateLatch();

			printf("Writing...\n");
			// Write some data to the flash, using the program command sequence
			fp = fopen(argv[argPos], "rb");
			argPos++;
			if (fp == 0)
			{
				printf("Error reading input file\n");
				continue;
			}

			bool gotError = false;
			int bank = 0;
			while (!feof(fp))
			{
				size_t numBytes = fread(bankData, sizeof(bankData[0]), sizeof(bankData), fp);
				if (gotError)
				{
					break;
				}
				if (numBytes == 0)
				{
					break;
				}
				printf("Got bytes %d for bank %d\n", numBytes, bank);

				// Set the bank register
				SetDataIO1(0, bank);
				SetDataIO2(bank >> 8);

				for (int address = 0; address < (int)sizeof(bankData); address++)
				{
					if (gotError)
					{
						break;
					}

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
					SendChipCommandProgram();

					// Program command4 (the actual byte)
					DataLatchOut::SetAddress(address);
					int theWriteValue = bankData[address];
					DataLatchOut::SetData(theWriteValue);

					PerformCartridgeWriteCycle();

					for (int slot = 0; slot < activeSlots; slot++)
					{
						InterfaceControl::SetActiveSlot(slot);

						int statusRegister = 0;
						int iterations = 0;
						do
						{
							if (gotError)
							{
								break;
							}

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
								printf("There seems to be a problem verifying the byte at address $%04x slot %d\n", address, slot+1);
								ReportCartridgeError();
								gotError = true;
								break;
							}
						} while (statusRegister != theWriteValue);
					}
					InterfaceControl::SetActiveSlot(0);
				}

				if (gotError)
				{
					break;
				}

				printf("\nBank done\n");
				bank++;
			}

			InterfaceControl::ClearLED1();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--eraseblock") == 0 || strcasecmp(argv[argPos], "-eb") == 0)
		{
			argPos++;

			InterfaceControl::SetLED0();
			InterfaceControl::UpdateLatch();

			int bank = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			printf("Erasing one block at bank %d\n" , bank);
			// Write some data to the flash, using the erase block command sequence
			DataLatchOut::SetAddress(0);
			SetDataIO1(0, bank);
			SetDataIO2(bank >> 8);

			SendChipCommandBlockErase();

			for (int slot = 0; slot < activeSlots; slot++)
			{
				InterfaceControl::SetActiveSlot(slot);
				WaitForStatusRegisterEqual(0xff);
			}

			InterfaceControl::SetActiveSlot(0);

			InterfaceControl::ClearLED0();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--read") == 0 || strcasecmp(argv[argPos], "-r") == 0)
		{
			argPos++;
			int numBanks = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			printf("Reading...\n");
			int maxDelayNeeded = 0;
			fp = fopen(argv[argPos], "wb");
			argPos++;

			for (int bank = 0; bank < numBanks; bank++)
			{
				printf("Bank %d\n", bank);
				SetDataIO1(0, bank);
				SetDataIO2(bank >> 8);

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
							delayMicroseconds(progressiveDelay / 2);
							maxDelayNeeded = std::max(maxDelayNeeded, progressiveDelay / 2);
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

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--loop") == 0 || strcasecmp(argv[argPos], "-l") == 0)
		{
			argPos = 1;
			continue;
		}


		if (strcasecmp(argv[argPos], "--dump") == 0 || strcasecmp(argv[argPos], "-d") == 0)
		{
			argPos++;
			int address = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			int numBytes = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			int maxDelayNeeded = 0;
			for (int slot = 0; slot < activeSlots; slot++)
			{
				printf("Dumping slot %d\n", slot + 1);
				InterfaceControl::SetActiveSlot(slot);

				for (int bytes = 0; bytes < numBytes; bytes++)
				{
					int bank = (address + bytes) / sizeof(bankData);
					SetDataIO1(0, bank);
					SetDataIO2(bank >> 8);

					DataLatchOut::SetAddress(address + bytes);
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
							delayMicroseconds(progressiveDelay / 2);
							maxDelayNeeded = std::max(maxDelayNeeded, progressiveDelay / 2);
						}
						gotNow = (unsigned char)GetInputByte();
						C64Control::ClearLowROM();
						C64Control::UpdateLatch();
						progressiveDelay++;
					}

					printf(" %02x ", gotNow);
					fflush(stdout);
				}
				printf("\n");
			}

			printf("maxDelayNeeded = %d\n", maxDelayNeeded);

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--dumpserial") == 0 || strcasecmp(argv[argPos], "-ds") == 0)
		{
			argPos++;
			int address = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			int numBytes = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			for (int slot = 0; slot < activeSlots; slot++)
			{
				printf("Dumping serial slot %d\n", slot + 1);
				InterfaceControl::SetActiveSlot(slot);

				SerialEEPROM_Init();

				// https://www.st.com/resource/en/datasheet/m93c76-r.pdf
				/*
				Chip: m93C86
				Page 8
				Each instruction is preceded by a rising edge on Chip Select Input (S) with Serial Clock (C) being held low.
					
				A start bit, which is the first '1' read on Serial Data Input (D) during the rising edge of Serial Clock (C).
					
				Two op-code bits, read on Serial Data Input (D) during the rising edge of Serial Clock (C). (Some
				instructions also use the first two bits of the address to define the op - code)
					
				The address bits of the byte or word that is to be accessed.
				For the M93C86, the address is made up of 10 bits for the x16 organization or 11 bits for the x8 organization
				Page 9 for M93C86
				READ Read Data from Memory 1 10 A10-A0 Q7-Q0
				*/


				// Reset instruction
				SerialEEPROM_Reset();
				// Start bit
				SerialEEPROM_SendBit(1);

				// Read command
				SerialEEPROM_SendBit(1);
				SerialEEPROM_SendBit(0);

				// Send address...
				for (int i = kSerialEEPROM_AddressBits-1; i >= 0; i--)
				{
					SerialEEPROM_SendBit(address & (1<<i));
				}

				// Skip dummy bit...
				SerialEEPROM_DoClock();

				for (int i = 0; i < numBytes; i++)
				{
					int byte = SerialEEPROM_ReadByte();
					printf("%02x ", byte);
					fflush(stdout);
				}

				printf("\n");
			}

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--readserial") == 0 || strcasecmp(argv[argPos], "-rs") == 0)
		{
			argPos++;
			fp = fopen(argv[argPos], "wb");
			argPos++;

			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			printf("Reading serial\n");

			SerialEEPROM_Init();

			// Reset instruction
			SerialEEPROM_Reset();
			// Start bit
			SerialEEPROM_SendBit(1);

			// Read command
			SerialEEPROM_SendBit(1);
			SerialEEPROM_SendBit(0);

			// Send address...
			for (int i = kSerialEEPROM_AddressBits - 1; i >= 0; i--)
			{
				SerialEEPROM_SendBit(0);
			}

			// Skip dummy bit...
			SerialEEPROM_DoClock();

			for (int i = 0; i < (1<<kSerialEEPROM_AddressBits); i++)
			{
				if ((i & 0xff) == 0)
				{
					printf(".");
					fflush(stdout);
				}

				int byte = SerialEEPROM_ReadByte();
				fputc(byte, fp);
				byte = SerialEEPROM_ReadByte();
				fputc(byte, fp);
			}

			fclose(fp);
			printf("\nDone reading serial\n");

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--writebyte") == 0 || strcasecmp(argv[argPos], "-wb") == 0)
		{
			argPos++;
			int address = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			int byte = std::stoi(argv[argPos], nullptr, 0);
			argPos++;
			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			printf("Writing single byte $%x at $%x...\n" , byte , address);
			int maxDelayNeeded = 0;

			int bank = address / sizeof(bankData);
			SetDataIO1(0, bank);
			SetDataIO2(bank >> 8);

			SendChipCommandProgram();

			DataLatchOut::SetAddress(address);
			DataLatchOut::SetData(byte);

			PerformCartridgeWriteCycle();

			C64Control::SetRead();
			C64Control::ClearLowROM();
			C64Control::ClearHighROM();
			C64Control::UpdateLatch();

			for (int slot = 0; slot < activeSlots; slot++)
			{
				InterfaceControl::SetActiveSlot(slot);

				// Tries two reads without any delay, then progressively increases the delay until we get two reads that are the same
				int gotPrevious = -1;
				int gotNow = -2;
				int progressiveDelay = 0;
				while ((gotPrevious != gotNow) && (gotNow != byte))
				{
					gotPrevious = gotNow;

					C64Control::SetLowROM();
					C64Control::UpdateLatch();
					if (progressiveDelay >= 2)
					{
						delayMicroseconds(progressiveDelay / 2);
						maxDelayNeeded = std::max(maxDelayNeeded, progressiveDelay / 2);
					}
					if (progressiveDelay > 20)
					{
						printf("Error the read delay is too long, perhaps the flash write is failing...\n");
						break;
					}
					gotNow = (unsigned char)GetInputByte();
					C64Control::ClearLowROM();
					C64Control::UpdateLatch();
					progressiveDelay++;
				}

				InterfaceControl::SetActiveSlot(0);
			}

			printf("\n");
			printf("maxDelayNeeded = %d\n", maxDelayNeeded);

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}

		if (strcasecmp(argv[argPos], "--slots") == 0 || strcasecmp(argv[argPos], "-s") == 0)
		{
			argPos++;
			activeSlots = std::stoi(argv[argPos], nullptr, 0);
			argPos++;

			continue;
		}

		if (strcasecmp(argv[argPos], "--eraseserial") == 0 || strcasecmp(argv[argPos], "-es") == 0)
		{
			bool gotError = false;
			argPos++;

			InterfaceControl::SetLED2();
			InterfaceControl::UpdateLatch();

			InterfaceControl::SetActiveSlot(0);
			InterfaceControl::UpdateLatch();

			SerialEEPROM_Init();

			SerialEEPROM_SendWriteEnable();

			// Reset instruction
			SerialEEPROM_Reset();
			// Start bit
			SerialEEPROM_SendBit(1);

			// Erase command
			SerialEEPROM_SendBit(0);
			SerialEEPROM_SendBit(0);

			// Send expected address...
			SerialEEPROM_SendBit(1);
			SerialEEPROM_SendBit(0);
			for (int i = 0; i < kSerialEEPROM_AddressBits - 2; i++)
			{
				SerialEEPROM_SendBit(0);
			}

			SerialEEPROM_WaitForReadyAllslots(gotError);

			printf("\n");

			InterfaceControl::ClearLED2();
			InterfaceControl::UpdateLatch();

			continue;
		}


		if (strcasecmp(argv[argPos], "--writeserial") == 0 || strcasecmp(argv[argPos], "-ws") == 0)
		{
			bool gotError = false;
			argPos++;

			InterfaceControl::SetLED1();
			InterfaceControl::UpdateLatch();

			printf("Writing serial...\n");

			// Write some data to the flash, using the program command sequence
			fp = fopen(argv[argPos], "rb");
			argPos++;
			if (fp == 0)
			{
				printf("Error reading input file\n");
				continue;
			}

			size_t numBytes = fread(bankData, sizeof(bankData[0]), sizeof(bankData), fp);
			fclose(fp);

			if (numBytes == 0)
			{
				break;
			}

			printf("Got bytes %d\n", numBytes);

			InterfaceControl::SetActiveSlot(0);
			InterfaceControl::UpdateLatch();

			SerialEEPROM_Init();

			SerialEEPROM_SendWriteEnable();

			for (int address = 0; address < (int)numBytes; address += 2)
			{
				if ((address & 0xff) == 0)
				{
					printf(".");
					fflush(stdout);
				}

				// Reset instruction
				SerialEEPROM_Reset();
				// Start bit
				SerialEEPROM_SendBit(1);

				// Write command
				SerialEEPROM_SendBit(0);
				SerialEEPROM_SendBit(1);

				// Send address...
				for (int i = kSerialEEPROM_AddressBits - 1; i >= 0; i--)
				{
					SerialEEPROM_SendBit((address >> 1) & (1 << i));
				}

				SerialEEPROM_SendByte(bankData[address]);
				SerialEEPROM_SendByte(bankData[address+1]);

				SerialEEPROM_WaitForReadyAllslots(gotError);

				if (gotError)
				{
					break;
				}
			}

			printf("\nWrite serial done\n");

			InterfaceControl::SetActiveSlot(0);
			InterfaceControl::UpdateLatch();

			InterfaceControl::ClearLED1();
			InterfaceControl::UpdateLatch();

			continue;
		}

		printf("Unknown argument: %s\n", argv[argPos]);
		argPos++;
	}

	// Final power off
	InterfaceControl::ClearRelay1();
	InterfaceControl::UpdateLatch();

	auto end = std::chrono::steady_clock::now();

	// Calculate elapsed time as a double in seconds
	std::chrono::duration<double> elapsed = end - start;

	printf("Elapsed time: %f seconds\n" , elapsed.count());
	return 0;
}
