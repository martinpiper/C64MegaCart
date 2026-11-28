#include <wiringPi.h>
#include <algorithm>
#include <atomic>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include "Hardware.h"

// The data bus needs about 100ns after the latch update that selects the cartridge (five slot programmer, Pi Zero 2W).
// Read at once, D0 (read first) still showed the level before in about a fifth of the bytes.
static const long kDataBusSettleNanoseconds = 200;

static volatile std::atomic<int> sCachedSignals[28];

// --gpiofast: the data pins D0..7 (GPIO 0-7) are read with one access of the level register, the output byte (GPIO 8-15)
// is written with one access of the set and one of the clear register, instead of 8 calls of digitalRead()/digitalWrite().
// The registers are the ones wiringPi maps: BCM2835 to BCM2711 (Pi 1 to 4, Zero 2W) or the RIO block of the RP1 (Pi 5).
// On by default; --gpioslow turns it off. Without mapped registers (the simulator, an unknown Pi) the single calls are
// used, with a note only when --gpiofast was given.
bool gpioFast = true;
bool gpioFastRequested = false;

// BCM283x/BCM2711: GPSET0, GPCLR0, GPLEV0 (in 32 bit words)
static const int kBcmSet0 = 7;
static const int kBcmClr0 = 10;
static const int kBcmLev0 = 13;
// RP1 RIO: OUT and IN, the set and clear aliases of a register are at +0x2000 and +0x3000 bytes
static const int kRioOut = 0;
static const int kRioIn = 2;
static const int kRioSet = 0x2000 / 4;
static const int kRioClr = 0x3000 / 4;

// Exported by wiringPi (wiringPi.c), but not declared in wiringPi.h. NULL when the Pi has no RP1.
extern volatile unsigned int* _wiringPiRio;

static volatile unsigned int* sSetRegister = nullptr;
static volatile unsigned int* sClearRegister = nullptr;
static volatile unsigned int* sLevelRegister = nullptr;

static void SetupRegisters(void)
{
	sSetRegister = nullptr;
	sClearRegister = nullptr;
	sLevelRegister = nullptr;
	if (_wiringPiRio)
	{
		sSetRegister = _wiringPiRio + kRioOut + kRioSet;
		sClearRegister = _wiringPiRio + kRioOut + kRioClr;
		sLevelRegister = _wiringPiRio + kRioIn;
	}
	else if (_wiringPiGpio)
	{
		sSetRegister = _wiringPiGpio + kBcmSet0;
		sClearRegister = _wiringPiGpio + kBcmClr0;
		sLevelRegister = _wiringPiGpio + kBcmLev0;
	}
}

static void SetupCounter(void);

static bool UseRegisters(void)
{
	return gpioFast && sLevelRegister != nullptr;
}

void HardwareInit()
{
	wiringPiSetupGpio();
	SetupRegisters();
	if (gpioFast)
	{
		SetupCounter();
	}
	if (gpioFast && gpioFastRequested && sLevelRegister == nullptr)
	{
		printf("--gpiofast: the GPIO registers are not mapped by wiringPi, using digitalRead()/digitalWrite()\n");
	}
	// Unknown: the GPIO outputs keep the levels of the last run, so SetOutputByte() must write every pin once
	for (int i = 0; i < 28; i++)
	{
		sCachedSignals[i] = -1;
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

// The counter of the ARM generic timer (19.2 MHz on the Pi Zero 2W to Pi 4, 54 MHz on the Pi 5) can be read in a few ns,
// clock_gettime() takes about 174 ns on the Zero 2W, so a wait of 200 ns with it is really about 500 ns.
// The DSB lets the read of the level register before it complete first, the ISB keeps the counter read after it.
static inline unsigned long long ReadCounter(void)
{
#if defined(__aarch64__)
	unsigned long long value;
	asm volatile("dsb sy\n\tisb\n\tmrs %0, cntvct_el0" : "=r"(value) : : "memory");
	return value;
#elif defined(__arm__)
	unsigned long long value;
	asm volatile(".arch armv7-a\n\tdsb sy\n\tisb\n\tmrrc p15, 1, %Q0, %R0, c14" : "=r"(value) : : "memory");
	return value;
#else
	return 0;
#endif
}

static inline unsigned long long ReadCounterFrequency(void)
{
#if defined(__aarch64__)
	unsigned long long value;
	asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
#elif defined(__arm__)
	unsigned int value;
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(value));
	return value;
#else
	return 0;
#endif
}

static double NanosecondsSince(const timespec& start);

// Counter ticks the data bus wait needs, 0 when the counter can not be used
static unsigned long long sDataBusSettleTicks = 0;
static unsigned long long sCounterFrequency = 0;

// The counter is only used when its frequency is plausible and it runs at that rate against CLOCK_MONOTONIC
// Checked once, HardwareInit() runs for every command
static void SetupCounter(void)
{
	static bool checked = false;
	if (checked)
	{
		return;
	}
	checked = true;
	sDataBusSettleTicks = 0;
	sCounterFrequency = 0;
#if defined(__arm__)
	// A 32 bit build runs on the Pi 1 and Zero too: the ARM1176 (ARMv6) has no generic timer, its instructions would
	// stop PiCartTool with SIGILL
	utsname name;
	if (uname(&name) != 0 || strncmp(name.machine, "armv6", 5) == 0)
	{
		return;
	}
#elif !defined(__aarch64__)
	return;
#endif
	sCounterFrequency = ReadCounterFrequency();
	if (sCounterFrequency < 1000000 || sCounterFrequency > 1000000000)
	{
		return;
	}
	timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	unsigned long long first = ReadCounter();
	while (NanosecondsSince(start) < 2000000)
	{
	}
	unsigned long long ticks = ReadCounter() - first;
	double measured = ticks / (NanosecondsSince(start) / 1e9);
	if (measured < sCounterFrequency * 0.9 || measured > sCounterFrequency * 1.1)
	{
		printf("--gpiofast: the ARM timer runs at %.0f Hz instead of %llu Hz, using clock_gettime()\n", measured, sCounterFrequency);
		return;
	}
	// The first tick may be almost over when the wait starts, so one tick more
	sDataBusSettleTicks = (kDataBusSettleNanoseconds * sCounterFrequency + 999999999ULL) / 1000000000ULL + 1;
}

static void WaitForDataBus(void)
{
	if (UseRegisters())
	{
		// Writes to the GPIO block are posted (on the Pi 5 over PCIe): a read only completes after them, so the wait
		// starts when the latch update is at the pins
		(void)*sLevelRegister;
		if (sDataBusSettleTicks)
		{
			unsigned long long start = ReadCounter();
			while (ReadCounter() - start < sDataBusSettleTicks)
			{
			}
			return;
		}
	}
	timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	do
	{
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while ((now.tv_sec - start.tv_sec) * 1000000000L + now.tv_nsec - start.tv_nsec < kDataBusSettleNanoseconds);
}

static int ReadDataPinsSingly(void)
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

int GetInputByte(void)
{
	WaitForDataBus();
	if (UseRegisters())
	{
		return *sLevelRegister & 0xff;
	}
	return ReadDataPinsSingly();
}

static void WriteOutputPinsSingly(int value)
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

static void WriteOutputPinsAtOnce(int value)
{
	int setMask = 0;
	int clearMask = 0;
	for (int i = 8; i < 16; i++)
	{
		int intendedSignal = (value >> (i - 8)) & 0x01;
		if (sCachedSignals[i] != intendedSignal)
		{
			if (intendedSignal)
			{
				setMask |= 1 << i;
			}
			else
			{
				clearMask |= 1 << i;
			}
			sCachedSignals[i] = intendedSignal;
		}
	}
	if (setMask)
	{
		*sSetRegister = setMask;
	}
	if (clearMask)
	{
		*sClearRegister = clearMask;
	}
}

void SetOutputByte(int value)
{
	if (UseRegisters())
	{
		WriteOutputPinsAtOnce(value);
	}
	else
	{
		WriteOutputPinsSingly(value);
	}
}

void WriteLatch(int latch)
{
	// The latch takes the output byte with the rising edge. The four writes of LOW make the pulse long enough,
	// they stay calls of digitalWrite() so the pulse is as long as before, also with --gpiofast.
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

static double NanosecondsSince(const timespec& start)
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - start.tv_sec) * 1e9 + (now.tv_nsec - start.tv_nsec);
}

// --gpiotest: measures the GPIO accesses with and without --gpiofast and checks that both give the same levels.
// Only the output byte (GPIO 8-15) changes, no latch takes it, so the programmer and the cartridges stay as they are.
void GpioTest(int iterations)
{
	if (iterations < 1)
	{
		iterations = 1;
	}
	bool savedFast = gpioFast;
	printf("GPIO test, %d iterations each\n", iterations);
	printf("Registers: %s\n", _wiringPiRio ? "RP1 RIO (Pi 5)" : _wiringPiGpio ? "BCM283x/BCM2711" : "not mapped");

	timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < iterations; i++)
	{
		timespec t;
		clock_gettime(CLOCK_MONOTONIC, &t);
	}
	printf("clock_gettime():                          %8.1f ns\n", NanosecondsSince(start) / iterations);

	gpioFast = false;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < iterations; i++)
	{
		WaitForDataBus();
	}
	printf("Data bus wait (%ld ns), clock_gettime():  %8.1f ns\n", kDataBusSettleNanoseconds, NanosecondsSince(start) / iterations);

	gpioFast = true;
	SetupCounter();
	if (sDataBusSettleTicks && sLevelRegister)
	{
		clock_gettime(CLOCK_MONOTONIC, &start);
		for (int i = 0; i < iterations; i++)
		{
			(void)ReadCounter();
		}
		printf("ARM timer read (%llu Hz):           %8.1f ns\n", sCounterFrequency, NanosecondsSince(start) / iterations);

		clock_gettime(CLOCK_MONOTONIC, &start);
		for (int i = 0; i < iterations; i++)
		{
			WaitForDataBus();
		}
		printf("Data bus wait (%ld ns), ARM timer:        %8.1f ns (%llu ticks)\n", kDataBusSettleNanoseconds, NanosecondsSince(start) / iterations, sDataBusSettleTicks);

		// The shortest wait: each one measured with CLOCK_MONOTONIC, the call itself is counted too
		double shortest = 1e18;
		for (int i = 0; i < 1000; i++)
		{
			timespec t;
			clock_gettime(CLOCK_MONOTONIC, &t);
			WaitForDataBus();
			shortest = std::min(shortest, NanosecondsSince(t));
		}
		printf("Data bus wait, ARM timer, shortest of 1000:  %8.1f ns (with clock_gettime())\n", shortest);
	}
	else
	{
		printf("ARM timer: not usable, the data bus wait uses clock_gettime()\n");
	}
	gpioFast = savedFast;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < iterations; i++)
	{
		(void)digitalRead(i & 7);
	}
	printf("digitalRead():                            %8.1f ns\n", NanosecondsSince(start) / iterations);

	// GPIO 8 to the level it has, nothing changes
	int pin8 = digitalRead(8);
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < iterations; i++)
	{
		digitalWrite(8, pin8);
	}
	printf("digitalWrite():                           %8.1f ns\n", NanosecondsSince(start) / iterations);

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < iterations; i++)
	{
		(void)ReadDataPinsSingly();
	}
	printf("D0..7 read, 8 x digitalRead():            %8.1f ns\n", NanosecondsSince(start) / iterations);

	// Every byte changes all 8 pins, so the cache leaves nothing out
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < iterations; i++)
	{
		WriteOutputPinsSingly((i & 1) ? 0xaa : 0x55);
	}
	printf("Output byte, 8 x digitalWrite():          %8.1f ns\n", NanosecondsSince(start) / iterations);

	if (sLevelRegister == nullptr)
	{
		printf("No registers, nothing to compare\n");
		gpioFast = savedFast;
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < iterations; i++)
	{
		(void)(*sLevelRegister & 0xff);
	}
	printf("D0..7 read, level register:               %8.1f ns\n", NanosecondsSince(start) / iterations);

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < iterations; i++)
	{
		WriteOutputPinsAtOnce((i & 1) ? 0xaa : 0x55);
	}
	printf("Output byte, set and clear register:      %8.1f ns\n", NanosecondsSince(start) / iterations);

	// The same levels with both ways: the output pins after each way of writing, read singly and from the register
	int errors = 0;
	int immediate = 0;
	static const int kPatterns[] = { 0x00, 0xff, 0x55, 0xaa, 0x01, 0x80, 0x0f, 0xf0, 0x3c, 0xc3 };
	for (int i = 0; i < iterations; i++)
	{
		int value = kPatterns[i % (sizeof(kPatterns) / sizeof(kPatterns[0]))] ^ ((i / 10) & 0xff);
		if (i & 1)
		{
			WriteOutputPinsAtOnce(value);
		}
		else
		{
			WriteOutputPinsSingly(value);
		}
		// Read at once the level register may still show the level before (the input synchronisers of the pins),
		// that is only counted. The check is after 1 us, the latches take the byte later than that anyway.
		if ((int)((*sLevelRegister >> 8) & 0xff) != value)
		{
			immediate++;
		}
		delayMicroseconds(1);
		int single = 0;
		for (int pin = 8; pin < 16; pin++)
		{
			single |= (digitalRead(pin) == HIGH ? 1 : 0) << (pin - 8);
		}
		int fromRegister = (*sLevelRegister >> 8) & 0xff;
		if (single != value || fromRegister != value)
		{
			if (errors < 10)
			{
				printf("Output byte $%02x (%s): read singly $%02x, level register $%02x\n", value, (i & 1) ? "registers" : "digitalWrite", single, fromRegister);
			}
			errors++;
		}
	}
	// D0..7 are inputs: with the cartridges off they may float, so only the number of differences is shown
	int inputDifferences = 0;
	for (int i = 0; i < iterations; i++)
	{
		if (ReadDataPinsSingly() != (int)(*sLevelRegister & 0xff))
		{
			inputDifferences++;
		}
	}
	printf("Output pins: %d of %d checks wrong after 1 us, %d read at once still showed the level before\n", errors, iterations, immediate);
	printf("D0..7: %d of %d reads differ between digitalRead() and the level register (inputs, may float)\n", inputDifferences, iterations);

	// Put the output byte back to a known state: the next SetOutputByte() writes every pin
	for (int i = 8; i < 16; i++)
	{
		sCachedSignals[i] = -1;
	}
	gpioFast = savedFast;
	printf(errors == 0 ? "GPIO test passed\n" : "GPIO test FAILED\n");
}
