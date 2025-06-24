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
		pinMode(i, INPUT);
	}
	for (int i = 0; i < 28; i++)
	{
		pinMode(8+i, OUTPUT);
	}

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
		for (int i = 8; i < 28; i++)
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
