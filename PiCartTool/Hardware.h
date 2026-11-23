#pragma once

void HardwareInit();
int GetInputByte(void);
void SetOutputByte(int value);
void WriteLatch(int latch);

void safeDelayMicroseconds(int delay);

// --gpiofast: reads and writes the 8 data pins with one register access each
extern bool gpioFast;
// --gpiotest <iterations>: measures the GPIO accesses with and without --gpiofast and compares the levels
void GpioTest(int iterations);
