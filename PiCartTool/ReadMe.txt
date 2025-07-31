Using: https://github.com/WiringPi/WiringPi

sudo apt install git
git clone https://github.com/WiringPi/WiringPi.git
Or if that disappears: https://github.com/martinpiper/WiringPi.git
cd WiringPi
./build



MSDev->Tools->Options->Cross Platform->Connection Manager
Default Raspberry Pi login: user:pi password:raspberry



cd projects/PiCartTool/bin/ARM/Release/
time ./PiCartTool.out
diff -q ../../../scrollerbanks.bin ../../../readdata.bin
