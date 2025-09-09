Using: https://github.com/WiringPi/WiringPi

sudo apt install git
git clone https://github.com/WiringPi/WiringPi.git
Or if that disappears: https://github.com/martinpiper/WiringPi.git
cd WiringPi
./build


gcc main.cpp -o t.bin -lwiringPi



MSDev->Tools->Options->Cross Platform->Connection Manager
Default Raspberry Pi login: user:pi password:raspberry



cd ~/projects/PiCartTool/bin/ARM/Release/
time ./PiCartTool.out
diff -q ../../../scrollerbanks.bin ../../../readdata.bin
cmp -l ../../../scrollerbanks.bin ../../../readdata.bin | mawk 'function oct2dec(oct,     dec) {for (i = 1; i <= length(oct); i++) {dec *= 8; dec += substr(oct, i, 1)}; return dec} {printf "%08X %02X %02X\n", $1-1, oct2dec($2), oct2dec($3)}'



RaspberryPi5 erase 2MB + write 2MB + read/verify 2MB cycle = 1m17s
The PiZero took about 2m for the same erase/write
The full read/verify on its own takes 17s
It's probably not needed since the write uses a read to verify the write has completed.
