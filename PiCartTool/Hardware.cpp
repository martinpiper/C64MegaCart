#include <wiringPi.h>
#include <atomic>
#include "Hardware.h"

static volatile std::atomic<int> sCachedSignals[28];

void HardwareInit()
{
	wiringPiSetupGpio();
	for (int i = 0; i < 28; i++)
	{
		sCachedSignals[i] = 0;
	}

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

void safeDelayMicroseconds(int delay)
{
	if (delay > 0)
	{
		delayMicroseconds(delay);
	}
}
