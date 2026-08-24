#include <stdio.h>
#include <wiringPi.h>
#include "CartridgePort.h"

void AlternateAllLED(void)
{
	static bool alternate = false;
	alternate = !alternate;
	if (alternate)
	{
		InterfaceControl::ClearLED0();
		InterfaceControl::ClearLED1();
		InterfaceControl::ClearLED2();
	}
	else
	{
		InterfaceControl::SetLED0();
		InterfaceControl::SetLED1();
		InterfaceControl::SetLED2();
	}
	InterfaceControl::UpdateLatch();
}

void AlternateLED2(void)
{
	static bool alternate = false;
	alternate = !alternate;
	if (alternate)
	{
		InterfaceControl::ClearLED2();
	}
	else
	{
		InterfaceControl::SetLED2();
	}
	InterfaceControl::UpdateLatch();
}

void ReportCartridgeError(void)
{
	InterfaceControl::ClearRelay1();
	InterfaceControl::UpdateLatch();

	printf("Error! Waiting for button...\n");
	while (digitalRead(24) == HIGH)
	{
		delay(100);
		AlternateAllLED();
	}
	while (digitalRead(24) == LOW)
	{
		delay(100);
		AlternateAllLED();
	}
	InterfaceControl::ClearLED0();
	InterfaceControl::ClearLED1();
	InterfaceControl::ClearLED2();
	InterfaceControl::UpdateLatch();
}
