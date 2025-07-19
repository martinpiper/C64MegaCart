gpio export 0 in
gpio export 1 in
gpio export 2 in
gpio export 3 in
gpio export 4 in
gpio export 5 in
gpio export 6 in
gpio export 7 in

gpio export 8 out
gpio export 9 out
gpio export 10 out
gpio export 11 out
gpio export 12 out
gpio export 13 out
gpio export 14 out
gpio export 15 out

gpio export 16 out
gpio export 17 out
gpio export 18 out
gpio export 19 out

gpio export 20 out
gpio export 21 out
gpio export 22 out
gpio export 23 out

gpio export 24 out
gpio export 25 in
gpio export 26 in
gpio export 27 in



gpio export 0 in && gpio export 1 in && gpio export 2 in && gpio export 3 in && gpio export 4 in && gpio export 5 in && gpio export 6 in && gpio export 7 in && gpio export 8 out && gpio export 9 out && gpio export 10 out && gpio export 11 out && gpio export 12 out && gpio export 13 out && gpio export 14 out && gpio export 15 out && gpio export 16 out && gpio export 17 out && gpio export 18 out && gpio export 19 out && gpio export 20 out && gpio export 21 out && gpio export 22 out && gpio export 23 out && gpio export 24 out && gpio export 25 in && gpio export 26 in && gpio export 27 in



Using: https://github.com/WiringPi/WiringPi

sudo apt install git
git clone https://github.com/WiringPi/WiringPi.git
cd WiringPi
./build



cd projects/PiCartTool/bin/ARM/Release/
time ./PiCartTool.out
diff -q ../../../scrollerbanks.bin ../../../readdata.bin
