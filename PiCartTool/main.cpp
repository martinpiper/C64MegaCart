#include <stdio.h>
#include <wiringPi.h>

int main(void)
{
	printf("wiringPiSetupSys\n");
	wiringPiSetupSys();

	printf("pinMode\n");
	// Don't forget the gpio exportx in the project post build config
	for (int i = 0; i < 8; i++)
	{
		pinMode(i, INPUT);	// D0..7
	}
	for (int i = 8; i < 27; i++)
	{
		pinMode(i, OUTPUT);
	}
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

#if 1
		for (int i = 8; i < 27; i++)
		{
			printf("on %d\n" , i);
			digitalWrite(i, HIGH);
			delay(250);
			digitalWrite(i, LOW);
		}
#endif

	}
	return 0;
}
