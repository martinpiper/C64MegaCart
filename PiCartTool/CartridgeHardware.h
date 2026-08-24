#pragma once
// Some cartridges need a slightly longer delay for accurate writes
extern int RegisterDelayMicroSeconds;
// Some cartridges need extra clocking of output signals to simulate slightly staggered bus signals from the C64.
// This is slower but more accurate.
extern bool flashWriteCommandExtraClocking;

enum FlashChipType
{
	M29F160 = 0,	// Also M29F200, M29F400, M29F800
	M29F040
};

extern FlashChipType flashChipType;

void ResetCartridge(void);
void ClearCartridgeIO(void);

void SendChipCommand(int address, int data);
void SendChipCommandErase(void);
void SendChipCommandBlockErase(void);
void SendChipCommandProgram(void);
void PerformCartridgeWriteCycle(void);
void WaitForStatusRegisterEqual(int waitFor);


extern int kSerialEEPROM_DataOut;
extern int kSerialEEPROM_ChipSelect;
extern int kSerialEEPROM_Clock;
extern int kSerialEEPROM_DataIn;
extern int kSerialEEPROM_IOAddress;
extern int kSerialEEPROM_AddressBits;

void SerialEEPROM_Init(void);
void SerialEEPROM_DoClock(void);
void SerialEEPROM_Reset(void);
void SerialEEPROM_SendBit(const int bit);
void SerialEEPROM_SendAddress(const int address);
int SerialEEPROM_ReadByte(void);
bool SerialEEPROM_WaitForReady(void);
void SerialEEPROM_WaitForReadyAllslots(bool& gotError);
void SerialEEPROM_SendWriteEnable(void);
void SerialEEPROM_SendByte(int byte);
