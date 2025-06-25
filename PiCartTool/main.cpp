#include <stdio.h>
#include <stdlib.h>
#include <wiringPi.h>

// projects/PiCartTool/bin/ARM/Release/PiCartTool.out

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

	printf("Running...\n");
	while (true)
	{
#if 0
		printf("on\n");
		for (int i = 0; i < (8 + 4 + 4 + 1); i++)
		{
			digitalWrite(8+i, HIGH);
		}
		delay(500);
		printf("off\n");
		for (int i = 0; i < (8 + 4 + 4 + 1); i++)
		{
			digitalWrite(8 + i, LOW);
		}
		delay(500);
#endif

#if 0
		// Toggle each output signal in sequence
		for (int i = 8; i < 25; i++)
		{
			printf("on %d\n" , i);
			digitalWrite(i, HIGH);
			delay(250);
			digitalWrite(i, LOW);
		}
#endif

#if 1
		// Simulate: Send 1MB of data, in 8KB chunks
		// TODO: Set proper signals for thir values
		for (int bank = 0; bank < 256; bank++)
		{
			printf("Sending bank %d\n", bank);
			SetOutputByte(bank);
			WriteLatch(0);
			SetOutputByte(0x00);
			WriteLatch(3);	// TODO: Proper signals for R_W toggle
			SetOutputByte(0x00);
			WriteLatch(3);	// TODO: Proper signals for R_W toggle
			for (int addressHi = 0; addressHi < 0x20; addressHi++)
			{
				SetOutputByte(addressHi);
				WriteLatch(2);
				for (int addressLo = 0; addressLo < 0x100; addressLo++)
				{
					SetOutputByte(addressLo);
					WriteLatch(1);
					SetOutputByte(rand() & 0xff);
					WriteLatch(0);
					SetOutputByte(0x00);
					WriteLatch(3);	// TODO: Proper signals for R_W toggle
					SetOutputByte(0x00);
					WriteLatch(3);	// TODO: Proper signals for R_W toggle
				}
			}
		}

#endif

	}
	return 0;
}
