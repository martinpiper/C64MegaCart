#pragma once

// --testefram: tests the 256 bytes of RAM of an EasyFlash 1 or EasyFlash 3, seen at $df00-$dfff (IO2), in the
// programmer slot of --activeslot or in all slots of --slots/--slotlist. Returns false when a slot fails.
bool EFRamTest(void);
