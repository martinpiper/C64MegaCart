#include <stdio.h>
#include <string.h>
#include <wiringPi.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include "Hardware.h"
#include "CartridgePort.h"
#include "EF3.h"
#include "EFRam.h"

// The RAM of an EasyFlash 1 and of an EasyFlash 3 is seen only at $df00-$dfff (IO2), 256 bytes. The test checks that
// the data lines, the address lines A0-A7 and every cell work, and that the RAM keeps its content for some seconds.
// Every write goes to all programmer slots, the reads come from one slot at a time.
// Its limits: only these 256 bytes are seen (a bigger chip has more, unseen), and the programmer is not a C64: a chip
// that is only a little too slow for the timing of a C64 can pass here.

static const int kRamSize = 256;
static const int kRetentionSeconds = 3;
static const int kShownErrors = 8;
static const int kMaxProgrammerSlots = 5;
// A write of the programmer was lost now and then with IO2 active for 1 us (see kWriteMicroseconds). Should it still
// happen, up to this many wrong values per slot are written once more; a stuck bit stays wrong, and a broken address
// line gives far more wrong cells.
static const int kMaxRewrites = 2;
static const int kWriteMicroseconds = 5;

// Writes IO2 as --detect does: PHI2 rises together with IO2 (the CPLD of the EF3 takes the write after the rising
// edge), and the data stays on the bus until IO2 ends (the TTL logic of the EF1 takes it at the end of WE, which is
// PHI2 high and R/W low).
static void RamWrite(int address, int value)
{
	DataLatchOut::SetAddress(address);
	DataLatchOut::SetData(value);
	C64Control::ClearPHI2();
	C64Control::SetDataLatchOut();
	C64Control::SetWrite();
	C64Control::UpdateLatch();
	C64Control::SetPHI2();
	C64Control::SetIO2();
	C64Control::UpdateLatch();
	// IO2 and the data for 5 us: with 1 us about one write in 50000 was lost (in all slots at the same moment, EF1 and
	// EF3), with 2 us still some; with 5 us none in 100 runs (870000 writes) on piper1
	delayMicroseconds(kWriteMicroseconds);
	C64Control::ClearIO2();
	C64Control::UpdateLatch();
	C64Control::SetRead();
	C64Control::ClearDataLatchOut();
	C64Control::UpdateLatch();
}

// Reads IO2 with PHI2 high (the EF1 drives the bus only then: OE is PHI2 high and R/W high)
static int RamRead(int address)
{
	DataLatchOut::SetAddress(address);
	C64Control::SetRead();
	C64Control::SetPHI2();
	C64Control::SetIO2();
	C64Control::UpdateLatch();
	delayMicroseconds(1);
	// D0 once more after the other bits: as for the flash of the EF3 (EF3ReadCurrentChip()), D0 sometimes still had the
	// value of the read before (piper1, 3 EF3: about one read in 100000)
	int value = GetInputByte();
	value = (value & 0xfe) | (digitalRead(0) == HIGH ? 1 : 0);
	C64Control::ClearIO2();
	C64Control::UpdateLatch();
	return value;
}

struct RamSlotResult
{
	int errors = 0;
	int reads = 0;
	int readsFF = 0;
	int dataBits = 0;		// Data bits that were wrong in any read
	int addressBits = 0;	// From the address test: the bits in which the value of another address came back
	int rereads = 0;		// Wrong reads that were right when read again (the programmer, not the RAM)
	int rewrites = 0;		// Wrong values that were right after one more write (a write of the programmer that was lost)
	std::vector<std::string> shown;
	std::vector<std::string> failedTests;
};

static RamSlotResult sResults[kMaxProgrammerSlots];
static std::vector<int> sSlots;

static std::string BitList(int bits, const char* prefix)
{
	std::string text;
	for (int bit = 0; bit < 8; bit++)
	{
		if (bits & (1 << bit))
		{
			text += (text.empty() ? "" : ", ") + std::string(prefix) + std::to_string(bit);
		}
	}
	return text;
}

static void Check(int programmerSlot, const char* test, int address, int expected, int got, bool addressTest)
{
	RamSlotResult& r = sResults[programmerSlot];
	r.reads++;
	if (got == 0xff)
	{
		r.readsFF++;
	}
	if (got == expected)
	{
		return;
	}
	// A wrong read is read twice more: a RAM that holds a wrong value gives it again, a read error of the programmer
	// not. The slot of the read is still selected.
	for (int again = 0; again < 2; again++)
	{
		if (RamRead(address) == expected)
		{
			r.rereads++;
			return;
		}
	}
	// Not in the retention test: there a cell that lost its content must count
	if (r.rewrites < kMaxRewrites && strcmp(test, "retention") != 0)
	{
		RamWrite(address, expected);
		if (RamRead(address) == expected)
		{
			r.rewrites++;
			return;
		}
	}
	r.errors++;
	// The data bits only from the tests of the data lines: in the other tests a wrong cell gives other bits
	if (strcmp(test, "data") == 0 || strncmp(test, "walking", 7) == 0)
	{
		r.dataBits |= got ^ expected;
	}
	if (addressTest)
	{
		r.addressBits |= got ^ expected;
	}
	if (std::find(r.failedTests.begin(), r.failedTests.end(), test) == r.failedTests.end())
	{
		r.failedTests.push_back(test);
	}
	if ((int) r.shown.size() < kShownErrors)
	{
		char line[160];
		snprintf(line, sizeof(line), "%s: $df%02x expected $%02x, read $%02x (wrong bits %s)", test, address, expected, got,
			BitList(got ^ expected, "D").c_str());
		r.shown.push_back(line);
	}
}

static void Fill(int (*value)(int address, int param), int param)
{
	for (int address = 0; address < kRamSize; address++)
	{
		RamWrite(address, value(address, param));
	}
}

static void Verify(const char* test, int (*value)(int address, int param), int param, bool addressTest = false)
{
	for (int programmerSlot : sSlots)
	{
		EF3SelectProgrammerSlot(programmerSlot);
		for (int address = 0; address < kRamSize; address++)
		{
			Check(programmerSlot, test, address, value(address, param), RamRead(address), addressTest);
		}
	}
}

static int Constant(int address, int param) { return param; }
static int AddressValue(int address, int param) { return (address ^ param) & 0xff; }

// March C-: up (w0); up (r0, w1); up (r1, w0); down (r0, w1); down (r1, w0); up (r0). Finds cells that change other
// cells (coupling) and cells that do not change in one direction.
static void March(int zero, int one)
{
	const char* test = "march";
	Fill(Constant, zero);
	for (int pass = 0; pass < 4; pass++)
	{
		bool down = pass >= 2;
		int expected = (pass % 2) == 0 ? zero : one;
		int write = (pass % 2) == 0 ? one : zero;
		for (int i = 0; i < kRamSize; i++)
		{
			int address = down ? kRamSize - 1 - i : i;
			for (int programmerSlot : sSlots)
			{
				EF3SelectProgrammerSlot(programmerSlot);
				Check(programmerSlot, test, address, expected, RamRead(address), false);
			}
			RamWrite(address, write);
		}
	}
	Verify(test, Constant, zero);
}

bool EFRamTest(void)
{
	sSlots = EF3ProgrammerSlots();
	for (auto& r : sResults)
	{
		r = RamSlotResult();
	}
	printf("RAM test at $df00-$dfff (IO2, 256 bytes) of the EasyFlash in programmer slot(s) %s\n",
		sSlots.size() > 1 ? UsedSlotsText().c_str() : std::to_string(sSlots[0] + 1).c_str());
	printf("Every programmer slot gets the writes to $df00-$dfff: other cartridges with a register there (Super Games, Final Cartridge III) should not be in the programmer\n");

	// The old content, to put it back when only one slot is tested (the writes go to all of them)
	unsigned char old[kRamSize];
	EF3SelectProgrammerSlot(sSlots[0]);
	for (int address = 0; address < kRamSize; address++)
	{
		old[address] = (unsigned char) RamRead(address);
	}

	printf("Data lines: $00, $ff, $55, $aa\n");
	const int patterns[] = { 0x00, 0xff, 0x55, 0xaa };
	for (int pattern : patterns)
	{
		Fill(Constant, pattern);
		Verify("data", Constant, pattern);
	}
	printf("Data lines: walking one and walking zero\n");
	for (int bit = 0; bit < 8; bit++)
	{
		Fill(Constant, 1 << bit);
		Verify("walking one", Constant, 1 << bit);
		Fill(Constant, 0xff ^ (1 << bit));
		Verify("walking zero", Constant, 0xff ^ (1 << bit));
	}
	printf("Address lines: every byte its own address, then the inverse\n");
	Fill(AddressValue, 0);
	Verify("address", AddressValue, 0, true);
	Fill(AddressValue, 0xff);
	Verify("address inverse", AddressValue, 0xff, true);
	printf("March C- with $00/$ff and $55/$aa\n");
	March(0x00, 0xff);
	March(0x55, 0xaa);
	printf("Retention: a pattern, %d seconds with the power on\n", kRetentionSeconds);
	Fill(AddressValue, 0xa5);
	// Checked right after the writes too: a write that was lost there (the cell still holds the $55 of the march test)
	// is written once more, and only a cell that loses its content while waiting counts as a retention error
	Verify("retention pattern", AddressValue, 0xa5);
	fflush(stdout);
	delay(kRetentionSeconds * 1000);
	Verify("retention", AddressValue, 0xa5);

	if (sSlots.size() == 1)
	{
		for (int address = 0; address < kRamSize; address++)
		{
			RamWrite(address, old[address]);
		}
	}
	else
	{
		Fill(Constant, 0);
	}
	EF3SelectProgrammerSlot(theActiveSlot);

	bool allOk = true;
	for (int programmerSlot : sSlots)
	{
		RamSlotResult& r = sResults[programmerSlot];
		if (r.errors == 0)
		{
			printf("Programmer slot %d: RAM test passed (%d reads: data lines, address lines A0-A7, march, %d s retention)\n",
				programmerSlot + 1, r.reads, kRetentionSeconds);
			if (r.rereads)
			{
				printf("  %d read(s) were wrong once and right when read again: a read error of the programmer, not of the RAM\n", r.rereads);
			}
			if (r.rewrites)
			{
				printf("  %d write(s) were lost and right after one more write: a lost write of the programmer, not of the RAM (the write goes to all slots, the first slot read sees it; a RAM that needs this in every test may be weak)\n", r.rewrites);
			}
			continue;
		}
		allOk = false;
		if (r.readsFF == r.reads)
		{
			printf("Programmer slot %d: RAM test FAILED: every read of $df00-$dfff gave $ff, no RAM is seen (no EasyFlash 1 or 3 in this slot?)\n",
				programmerSlot + 1);
			continue;
		}
		std::string tests;
		for (const std::string& test : r.failedTests)
		{
			tests += (tests.empty() ? "" : ", ") + test;
		}
		printf("Programmer slot %d: RAM test FAILED: %d of %d reads wrong (tests: %s)\n", programmerSlot + 1, r.errors, r.reads,
			tests.c_str());
		// When the data lines are right, a wrong value in the address test is the value of another address: the bits
		// in which it differs are the address lines that do not get through
		bool dataLinesOk = r.dataBits == 0;
		if (!dataLinesOk)
		{
			printf("  Data lines: the bits %s were wrong\n", BitList(r.dataBits, "D").c_str());
		}
		else
		{
			printf("  Data lines: right\n");
			if (r.addressBits)
			{
				printf("  Address lines that do not seem to get through: %s\n", BitList(r.addressBits, "A").c_str());
			}
			else if (std::find(r.failedTests.begin(), r.failedTests.end(), "retention") != r.failedTests.end() &&
				r.failedTests.size() == 1)
			{
				printf("  Only the retention failed: the RAM loses its content within %d seconds\n", kRetentionSeconds);
			}
		}
		for (const std::string& line : r.shown)
		{
			printf("  %s\n", line.c_str());
		}
		if (r.errors > (int) r.shown.size())
		{
			printf("  ... %d more\n", r.errors - (int) r.shown.size());
		}
	}
	printf(sSlots.size() == 1 ? "The old content of the RAM is written back\n" : "The RAM holds $00 now\n");
	return allOk;
}
