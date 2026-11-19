# EasyFlash 3 support for PiCartTool

EasyFlash 3 (EF3) support has been added to PiCartTool. With it, PiCartTool can:

- read the flash chip ID (`--ef3id`)
- read the whole 8 MB flash (`--readef3`)
- read the whole flash and extract it into `.crt` per slot, KERNALs and freezers (`--dumpef3`, `--extractef3`)
- read a single slot as `.crt`, a KERNAL or a freezer (`--readef3slot`, `--readef3kernal`, `--readef3freezer`)
- erase, write and verify a single EasyFlash slot from a `.crt` (`--writeef3slot`)
- erase, write and verify a single KERNAL or freezer in the system area (`--writeef3kernal`, `--writeef3freezer`)
- write a whole image back, with or without the EF3 menu and EasyProg (`--writeef3`, `--writeef3all`)
- erase a slot, KERNAL or freezer including its menu name, or everything except the EF3 menu and EasyProg (`--eraseef3slot`, `--eraseef3kernal`, `--eraseef3freezer`, `--eraseef3all`)
- set all menu names back without erasing (`--ef3resetnames`)
- set the name of a slot, KERNAL or freezer in the EF3 menu (`--ef3slotname`, `--ef3kernalname`, `--ef3freezername`)
- write to the system area without confirmation, for scripts (`--ef3noconfirm`)
- write several EasyFlash 3 at once in the five slot programmer (`--slots`)

All options were tested on a real EF3, and also with an emulation of the EF3, see [Tests](#tests).

The changes are additions only. The existing options and code paths are unchanged.

## Why the existing options do not work with the EF3

Everything below comes from the CPLD source in [easyflash3-core](https://gitlab.com/easyflash/easyflash3-core) (`src/exp_bus_ctrl.vhdl`, `src/ef3.vhdl`, `src/cart_easyflash.vhdl`). The flash command sequences come from [eapi](https://gitlab.com/easyflash/eapi) (`eapi-mx29640b.s`).

### Writes need a rising PHI2 edge

The EF3 CPLD runs on its own 25 MHz clock. It measures the time since the last PHI2 edge with a shift register, `cycle_time`, which is reset on every PHI2 edge (`exp_bus_ctrl.vhdl`). Every write uses the write pulse

```
wp <= not n_wr and phi2_s and cycle_time_i(7);
```

This pulse is active only about 280 to 320 ns after a rising PHI2 edge. It is used for:

- the register writes `$de00`, `$de01`, `$de02` and `$de0f`
- the flash write signal `n_mem_wr`, which goes active at `wp` and inactive at `wp_end`

`InitCartTool()` sets PHI2 high and never changes it, so there is no edge and the EF3 ignores every write. As a result:

- `--read` gets bank 0 again and again, because the write to `$de00` is lost
- `--erase` and `--write` have no effect

### Reads work without PHI2 edges

The CPLD decodes reads asynchronously (`rd` in `exp_bus_ctrl.vhdl`, `rw_mem` in `cart_easyflash.vhdl`). The flash output is enabled as long as ROML or ROMH is low and R/W is high. Static reads therefore work as they are.

### The CPLD drives the data bus while PHI2 is low

`data_out_enable` in `ef3.vhdl` drives the C64 data bus when IO1, IO2, ROML or ROMH is active and either PHI2 is low, or PHI2 is high and R/W is high. This is needed for the C128. PHI2 must therefore never go low while a select line is active and the programmer drives the bus.

### Reading `$de01` keeps the value on the bus

Reading `$de01` returns the slot number. After the read, `data_out_valid` stays set until the next PHI2 edge (`cycle_start`). Until then, every following read from ROML or ROMH also returns the slot number instead of the flash data.

### State after reset

After a reset from the expansion port, the EF3 is in EasyFlash mode with the menu registers enabled (`enable_ef = 1`, `enable_menu = 1`), slot 0 and bank 0. EasyFlash mode is therefore available at once, and `$de0f` (the cartridge mode register) is never written.

### Address mapping

In EasyFlash mode the 23-bit flash address is put together like this (`set_mem_addr` in `ef3.vhdl`):

```
A22..A20  slot      ($de01, bits 2..0)
A19..A17  bank_hi   ($de00, bits 5..3)
A16       ROMH      (1 when ROML is not active)
A15..A13  bank_lo   ($de00, bits 2..0)
A12..A0   address
```

All 8 MB can be reached with slots 0 to 7 and banks 0 to 63, with ROML and ROMH.

## How the patch solves this

### Writes with a PHI2 edge (`CartridgePort.cpp`)

PHI2 (`kPHI2`) and the select lines (`kNotIO1`, `kNotROML`, `kNotROMH`) are all in latch 3. One `UpdateLatch()` can therefore set PHI2 high and the select line active at the same moment. `WriteWithPHI2Edge()` does this:

1. Set the address and data latches.
2. With one latch update, set PHI2 low, R/W = write and the data output. No select line is active, so the EF3 does not drive the bus.
3. With one latch update, set PHI2 high and the select line active. The CPLD sees a rising edge with stable signals, and `wp` fires.
4. Hold for 600 ns with a busy wait on `clock_gettime()` (`DelayNanoseconds()`). The CPLD writes about 280 to 480 ns after the edge (`wp` to `wp_end`). `delayMicroseconds(1)` waits at least 1 µs and often longer.
5. Release the select line, then R/W and the data output. PHI2 stays high.

That makes 4 latch updates per write. The first version used 5, with PHI2 low and the write setup in two separate updates.

The speed of the Pi does not matter, because the CPLD times the pulse itself after the edge. The functions built on it:

- `SetDataIO1WithPHI2Edge()` for the registers `$de00` and `$de01`
- `SetDataROMLWithPHI2Edge()` and `SetDataROMHWithPHI2Edge()` for the flash

### Reads

`GetDataIO1()` reads an IO1 register and then toggles PHI2 low and high with nothing selected. This clears `data_out_valid`, see [Reading `$de01` keeps the value on the bus](#reading-de01-keeps-the-value-on-the-bus). `GetDataROML()` and `GetDataROMH()` read one byte after a 1 µs delay; they are used for polling the flash status.

All reads of whole banks go through `EF3ReadCurrentChip()`. These are the full read, the single reads, the verify after writing and the bank proof. Because the CPLD decodes reads asynchronously, it keeps ROML or ROMH active for the whole 8 KB and only changes the address from byte to byte. Toggling the select line for every read costs two latch updates, and dropping that is the main speedup for reading.

### Minimum read delay (D0)

The first version of `--readef3` used the same read loop as `--read`. That loop reads without delay until two reads in a row are equal. Bit 0 then came back as 1 in 773 of 788 used 8 KB blocks. For example, JMP `$4c` was read as `$4d`, and "BASIC BYTES FREE" as "CASIC CYUES GSEE". Two early reads gave the same wrong value, so the "two equal reads" check did not catch it.

Reads of `$de01` already had a 1 µs delay and returned bit 0 correctly. So the reads now wait at least 1 µs after each address change. After that, every block was correct and `maxDelayNeeded` was 0.

`EF3ReadCurrentChip()` now waits 1 µs once per byte and then reads twice in a row. If the two reads differ, it reads again with a growing delay. The generic `--read` may need the same minimum delay for some cartridges.

### Flash commands (MX29LV640EB)

The EF3 has one MX29LV640EB: 8 MB, bottom boot, manufacturer `$c2`, device `$cb`.

- The first 64 KB (slot 0, bank 0, ROML) are eight 8 KB boot sectors. They hold the KERNAL images.
- All other sectors have 64 KB. In EasyFlash terms, a sector is 8 banks of either ROML or ROMH within one slot, starting at a bank that is a multiple of 8.

The command sequences come from `eapi-mx29640b.s`. The unlock cycles are written over ROML:

- `$aa` to `$0aaa`
- `$55` to `$0555`
- the command to `$0aaa`

These are `$8aaa` and `$8555` on the C64.

EAPI always sends the unlock cycles to slot 0, bank 0 and only then switches to the target slot and bank. The patch does this only for the autoselect of `--ef3id` (`EF3FlashCommand()`). For erasing and programming, it sends all cycles to the target slot and bank, because the flash ignores the upper address bits of the unlock cycles:

- This halves the writes per programmed byte: no switch to slot 0, bank 0 and back for every byte.
- It is also safer, because the commands never go through slot 0 unless the target is slot 0.

The first version used the EAPI way for slots. The current way was already used on the real EF3 for the KERNAL, freezer and directory writes. For slot writes it came with the [speedup](#speed) and was then tested on the real EF3 as well.

| Operation | Sequence |
| --- | --- |
| Autoselect | `EF3FlashCommand($90)` in slot 0, bank 0, read `$0000` (manufacturer) and `$0002` (device), then `$f0` to reset |
| Sector erase (`EF3EraseSectorInCurrentBank()`) | In the target slot and bank: `$aa`, `$55`, `$80`, `$aa`, `$55` as above, then `$30` to address 0 of ROML or ROMH |
| Program byte (`EF3ProgramInCurrentBank()`) | In the target slot and bank: `$aa`, `$55`, `$a0` as above, then the data to the target address of ROML or ROMH |
| Unlock bypass | Enter with `$aa`, `$55`, `$20`. Then each byte is `$a0` and the data, 2 writes instead of 4. `$a0` may go to any address; it goes to the target address, so the address latches do not change between the two writes. Exit with `$90`, `$00`, then `$f0` |

`EF3ProgramInCurrentBank()` uses the unlock bypass mode for each 8 KB bank. The MX29LV640EB of the tested EF3 supports it (`Flash unlock bypass mode works`). If a flash does not, it ignores the bypass commands, and the very first byte times out. Then PiCartTool prints `Flash unlock bypass mode does not work, using the normal program command`, writes this byte again with the normal command, and uses the normal command for the rest of the run. The verify after each bank catches any wrong byte either way.

`EF3WaitForFlash()` polls until the byte reads as the expected value: `$ff` after an erase, the data after a write. While the flash is busy, DQ7 is the complement, so the value only matches when the operation is complete. EAPI uses the same method.

The timeout parameter counts read attempts. Each attempt takes at least 1 µs, so the real timeout is somewhat longer than the number: 30,000,000 for an erase, 10,000 for a byte. After an error the chip is reset with `$f0`.

`EF3SetSlotAndBank()` writes `$de01` and `$de00` and then reads back `$de01`. If the slot did not change, the CPLD could still be on slot 0, for example after the reset before each command, and the next erase or program cycle would hit the system area. Every bank switch before erasing or programming a slot therefore checks the returned value and stops on a mismatch. Once, with three EF3s in the programmer, the first readback after the reset returned 2 instead of 0, while the same EF3 worked in every later test. So a wrong value is written and read once more before giving up.

All writes go through `EF3WriteSector()`. It writes one 64 KB sector (8 banks of ROML or ROMH) or one 8 KB boot sector:

1. **Compare:** It reads the sector first. If the sector already holds the data, it is skipped. If it is empty, it is not erased.
2. **Bank proof:** Before an erase, `EF3SelectBankChecked()` proves the bank switch, see [`--writeef3kernal`](#--writeef3kernal-kernal-filename---writeef3freezer-freezer-filename). In slot 0 it also does this before writing an empty sector.
3. **Write and verify:** It programs and verifies bank by bank.

A lost bank switch while comparing can make an unchanged sector look changed. The bank proof then stops before the erase. This came up in the emulation, see [Tests with an emulated EF3](#tests-with-an-emulated-ef3).

**Exception for empty EF3 slots:** The proof needs two banks with different content. In slots 1–7, a programmer slot where every bank of this chip half reads `$ff` cannot be proven, but it also needs no proof. Whatever bank is selected there, the commands can only hit empty banks of the same EF3 slot, and the slot is proven by the `$de01` readback.

- **Why it is needed:** On the hardware, three EF3s were written together. In one of them EF3 slot 1 was completely empty, and the write stopped before erasing anything.
- **Every reference must be empty:** Comparing with the target is not enough. If the switch to the target bank is lost, the target reads the same as each reference although the slot holds data. The first version of this exception only looked at the last reference. The emulation showed that it then erased a sector with data in exactly that case.
- **Remaining risk:** If the bank register of one EF3 were completely stuck on an empty bank, all reads would be empty there too. Damage would stay inside the chip half of the EF3 slot that is being written, and the verify of the second bank would stop the write.

## Several EasyFlash 3 at once (five slot programmer)

As far as I can see from `--write` and `--dump`, the five slot programmer sends every write to all slots at the same time. Reads come from one slot at a time, selected with `InterfaceControl::SetActiveSlot()`. The EF3 options follow this:

- **Writes** go to all slots. With `--slots n` every check is done for each of the programmer slots 1 to n:
  - the flash ID (`EF3CheckChips()`)
  - the slot register readback (`EF3SelectSlotAndBank()`)
  - the bank proof (`EF3SelectBankChecked()`)
  - waiting for the flash (`EF3WaitForFlashAll()`)
  - the verify
- **Without `--slots`** only the slot of `--activeslot` (default: the front slot) is checked, as with the existing options.
- **Other slots:** All slots still get every write, also the ones outside `--slots`. So `EF3CheckOtherProgrammerSlots()` first reads the flash ID in all 5 slots. An empty slot reads `$ff $ff` on the hardware. Nothing is written if another slot holds:
  - another flash ID, for example another cartridge type
  - with `--slots`, an EF3 that is not part of it
  - without `--slots`, an EF3 with different content. The content is compared using the menu directory and bank 0 ROML of each EF3 slot

  An EF3 with the same content only gives a note, because a programmer with one slot reads the same cartridge in every slot. A Gmod2 does not answer the autoselect command with these addresses. It is still detected, unless the bytes it returns at `$0000` and `$0002` are both `$ff`.

  This came from a real mistake. Three backups were written back one after the other without `--slots` while three EF3s were in the programmer, each restore meant for one of them. The front EF3 ended up correct. The other two got the writes that the front one needed, without their own compare, erase decisions or verify, and ended up mixed.
- **Same data:** All EF3s get the same data. A sector is skipped only if it holds the data in all slots, and it is erased if it is not empty in any slot.
- **Renaming** writes the whole directory sector, so it needs the same directory in all slots, otherwise it is refused.
- **Reads** use the slot of `--activeslot`.
- **Messages** about a single cartridge name the programmer slot, counted from 1 like the existing `--write` messages.

On the hardware, three EF3s with different content were found and their slot registers worked in all three programmer slots. The existing `--dump` read bit 0 wrong in all three slots, as in the [first version of `--readef3`](#minimum-read-delay-d0); the EF3 options read them correctly.

I could not check this against the schematic of the five slot board, only against the code. It was tested with an emulation of up to 5 EF3s with different content, see [Tests with an emulated EF3](#tests-with-an-emulated-ef3), and then with three EF3s in the real programmer, see [Tests](#tests).

## New options

The new options do not use `--cfwch`/`--cfwcl`, `--cfwdci`, `--crdms`, `--cfwec` or `--cfct`. All tests were run with `--cfwec`, which only affects the existing flash functions in `CartridgeHardware.cpp`. All new options work on the active programmer slot only (default: the front slot).

### `--ef3id`

Sends the autoselect command and prints the manufacturer and device ID of each programmer slot. Then it writes 0 to 7 into the slot register `$de01` and reads each value back in each programmer slot, because all reads and writes rely on this readback. It reports whether the chip is the expected MX29LV640EB. All write options run the same check first (`EF3CheckChips()`) and write nothing if one slot does not hold an MX29LV640EB. Nothing is erased or written, but it uses the same flash command writes as erasing and writing, so it is a quick check that flash writes reach the chip.

```
EasyFlash 3 flash manufacturer $c2 device $cb
MX29LV640EB found
```

### `--readef3 <filename>`

Reads all 8 slots × 64 banks × (ROML, ROMH) into one 8 MB file. The file order is: slot, bank, 8 KB ROML, 8 KB ROMH. This is the same order as the EF3 tools (`mkimages.c` in [easyflash3-bootimage](https://gitlab.com/easyflash/easyflash3-bootimage)) and as an EasyFlash `.crt`.

After selecting each slot, it reads back `$de01` and stops if the value does not match.

### `--dumpef3 <folder>`

Reads the EF3 like `--readef3` into `<folder>/ef3.bin` and then extracts it like `--extractef3` into the same folder. Nothing is written if the read fails.

### `--extractef3 <filename> <folder>`

Extracts an image from `--readef3`. It does not access the cartridge. The names come from the menu directory, for example `slot1_prince_of_persia.crt`:

- `slot<n>_<name>.crt` for slots 1–7, as EasyFlash `.crt` (hardware type 32, EXROM inactive, GAME active, chips that are all `$ff` are left out, ROMH chips at `$a000`)
- `kernal<n>_<name>.bin` with 8 KB each, for the used KERNAL banks
- `freezer<n>_<name>.bin` with the Retro Replay, Action Replay and SS5 areas, without empty 8 KB blocks at the end

### `--writeef3slot <slot> <filename>`

Writes one EasyFlash slot, 1 to 7. The file is either:

- an EasyFlash `.crt` (hardware type 32) with 8 KB chips at `$8000` (ROML) and `$a000`/`$e000` (ROMH), or 16 KB chips at `$8000`
- a raw slot file of up to 1 MB in the same order as one slot of `--readef3`

Unused bytes are `$ff`.

1. Refuses slot 0 (system area).
2. Goes through the 16 sectors of the slot (banks 0–7, 8–15, … 56–63, ROML and ROMH each) and reads each sector first:
   - If the sector already holds the data, it is skipped (`unchanged`).
   - If it is empty, it is only written.
   - Otherwise it is erased and written.
3. Programs every byte that is not `$ff`.
4. Reads back every 8 KB bank and compares it.

The other slots are not touched. The name in the EF3 menu is not changed; use `--ef3slotname` for that.

### `--readef3slot <slot> <filename>`, `--readef3kernal <kernal> <filename>`, `--readef3freezer <freezer> <filename>`

Read one EasyFlash slot (1–7) as `.crt`, one KERNAL (1–8) or one freezer (1 = Retro Replay, 2 = Action Replay, 3 = Super Snapshot 5). The files are the same as the ones from `--extractef3`. Empty KERNALs and freezers are reported and not written.

### `--writeef3kernal <kernal> <filename>`, `--writeef3freezer <freezer> <filename>`

Write one KERNAL or freezer in the system area:

- A KERNAL file has up to 8 KB and goes into its own 8 KB boot sector (bank `kernal - 1`, ROML).
- A freezer file has the format of `--readef3freezer`: up to 64 KB for Retro Replay (banks `$10`–`$17` ROMH) and Action Replay (banks `$18`–`$1f` ROMH), up to 128 KB for Super Snapshot 5 (banks `$20`–`$27`, each 8 KB ROML followed by 8 KB ROMH). Each of these is one or two whole 64 KB sectors.

Only these sectors are erased. As with slots, a sector that already holds the data is skipped, and an empty one is not erased. The steps:

1. **Checks:** Reads the menu directory and refuses if it is missing. Refuses files that are too big.
2. **Confirmation:** Shows the current name and the file, and continues only after typing `yes` (see [`--ef3noconfirm`](#--ef3noconfirm)).
3. **Bank proof (also when there is nothing to erase):** The bank register cannot be read back, and a lost bank write in slot 0 would erase the wrong part of the system area. So `EF3SelectBankChecked()` first reads another bank of the same chip, then selects the target bank and reads it again. Only if the two differ is the bank switch proven. Otherwise it tries the next reference bank, and stops without erasing if no bank differs.
4. **Erase:** All unlock cycles and the erase command stay in this bank, with no bank switch in between, as with `--ef3slotname`.
5. **Program and verify** bank by bank.

The menu name is not changed.

### `--ef3slotname <slot> <name>`, `--ef3kernalname <kernal> <name>`, `--ef3freezername <freezer> <name>`

Set the name of slot 1 to 15, KERNAL 1 to 8 or freezer 1 to 3 in the EF3 menu directory. The directory entries are at index 1–15 (slots), 16–23 (KERNALs) and 24–26 (freezers).

- **Name check:** The name must have 1 to 16 printable ASCII characters (`$20`–`$7e`). Anything else is refused before the cartridge is touched. The name is converted to PETSCII for the upper/lower case character set:
  - `A`–`Z` become `$c1`–`$da`
  - `a`–`z` become `$41`–`$5a`
  - the entry is padded with `$00`
- **Directory location:** The directory is in slot 0, bank `$10`, ROML. It is alone in its 64 KB sector (banks `$10`–`$17`, ROML). The format seen on the tested cartridge:
  - `EF-Directory V2:` (16 bytes)
  - 16 slot names, 8 KERNAL names and 4 freezer names, 16 bytes each
  - the bytes `$00 $11 $47` (the V1 source in `easyflash3-bootimage/directory.s` ends with `$4711`, meaning no checksum)
- **Sector check:** It reads the whole sector, banks `$17` down to `$10`, so that bank `$10` stays selected. It refuses unless banks `$11`–`$17` are empty and bank `$10` starts with the `EF-Directory` signature. Reading the signature also proves that the bank register is really set to `$10`.
- **Confirmation:** It shows the old and the new name and continues only after typing `yes`:
  ```
  WARNING: This erases and rewrites the EF3 menu directory in slot 0 (system area)
  Slot 1: "Prince of Persia" -> "Toutrun"
  Type yes to continue:
  ```
  Any other answer gives `Cancelled, nothing was changed`.
- **Rewrite:** It erases the sector and writes back bank `$10` with only the 16 bytes of the name changed, then verifies it. **This deviates from EAPI:** all unlock cycles go to bank `$10` (flash address `$40aaa`/`$40555`) instead of bank 0, and there is no bank switch at all. A failed bank switch can therefore never erase the KERNAL boot sector in bank 0. The chip accepted the commands this way, see [Tests](#tests).

### `--writeef3 <filename>`, `--writeef3all <filename>`

Write a whole 8 MB image from `--readef3` back, for example to restore a backup. The image must contain the menu directory, otherwise it is refused.

- **`--writeef3`** writes:
  - slots 1–7
  - the 8 KERNAL boot sectors
  - the directory sector
  - the freezer sectors

  The EF3 menu, EasyProg and all other parts of slot 0 are not written. It asks for `yes`.
- **`--writeef3all`** writes all 8 MB, including the EF3 menu and EasyProg. If it is interrupted while the menu is written, the EF3 may not start its menu any more. It asks for the text `WRITE ALL`.

Both write slots 1–7 first and slot 0 last. Every sector goes through `EF3WriteSector()`, so unchanged sectors are skipped. When a backup is restored, the menu is usually unchanged and therefore not touched at all. `--ef3noconfirm` also turns off both confirmations.

### `--eraseef3slot <slot>`, `--eraseef3kernal <kernal>`, `--eraseef3freezer <freezer>`, `--eraseef3all`

These erase slot 1–7, KERNAL 1–8 or freezer 1–3 and set the menu name back to the name EasyProg uses for an empty entry:

- **Slots:** "Slot n" (`checkEraseSlot()` in EasyProg's `write.c`)
- **KERNALs:** "KERNAL n" (`slotsFillEFDir()` in `slots.c`)
- **Freezers:** "RR Slot", "AR Slot" and "SS5 Slot". This one is an assumption. EasyProg has no default name for the freezers, but the tested cartridges show "SS5 Slot" and "FC3 Slot" for unused entries.

`--eraseef3all` erases slots 1–7, all KERNALs and all freezers and sets the names of entries 1–26 back, like a new EasyFlash 3. The EF3 menu and EasyProg stay.

All of them go through `EF3WriteSector()` with `$ff` as the data, so they use the same checks:

- **Skip:** empty sectors are skipped
- **Bank proof:** before every erase
- **Chip check:** before anything is touched
- **Several slots:** with `--slots` every programmer slot is erased

The menu directory is checked before anything is erased, and rewritten at the end only if a name changes. With `--slots` it must be the same in all programmer slots. The erase options ask for `yes`, and `--eraseef3all` for the text `ERASE ALL`.

### `--ef3resetnames`

Sets the names of entries 1–26 back to the default names, like `--eraseef3all`, but erases nothing else. Only the directory sector is rewritten, so it takes a few seconds instead of about 100. The slots, KERNALs and freezers keep their content, and the menu still starts them. It asks for `yes`. If all names are the defaults already, nothing is written.

### `--ef3noconfirm`

Turns off the confirmation for all following writes to the system area in the same command line (`--writeef3kernal`, `--writeef3freezer`, `--writeef3`, `--writeef3all`, the erase options, `--ef3resetnames` and the rename options), for scripts. The warning is still printed, followed by `No confirmation (--ef3noconfirm)`. All other checks, including the bank proof, stay active.

## System area layout (slot 0)

The layout is taken from the `Makefile` of easyflash3-bootimage and confirmed with the read image:

| Banks | Chip | Content |
| --- | --- | --- |
| `$00`–`$07` | ROML | KERNAL 1–8 (8 KB boot sectors) |
| `$10` | ROML | Menu directory (alone in its 64 KB sector) |
| `$10`–`$17` | ROMH | Retro Replay |
| `$18`–`$1f` | ROMH | Action Replay |
| `$20`–`$27` | ROML and ROMH | Super Snapshot 5 |
| other used banks | | EF3 menu, EasyProg and the tools of the boot image |

## Helper functions in `main.cpp`

The EF3 functions are static functions at the top of `main.cpp`, so the build command does not change:

- **Image and `.crt` handling:** `EF3ReadImage()`, `EF3Extract()`, `EF3WriteCRT()`, `EF3LoadSlotFile()`, `PETSCIIToASCII()`, `EF3FileName()`
- **System area:**
  - `EF3GetArea()` holds the KERNAL and freezer layout
  - `EF3ReadArea()` and `EF3WriteArea()` read and write a KERNAL or freezer
  - `EF3SelectBankChecked()` proves the bank switch
  - `EF3EraseSectorInCurrentBank()` and `EF3ProgramInCurrentBank()` send the flash commands without a bank switch
  - `EF3WriteSector()` compares, erases, writes and verifies one sector, for all write options
  - `EF3WriteImage()` writes a whole image
  - `EF3SetName()` writes a directory entry
- **Slots:** `EF3ReadSlot()`

## Tests

The hardware was a Raspberry Pi with the five-slot programmer and the EF3 in the front slot. All runs used `--cfwec`.

| Test | Result |
| --- | --- |
| `--ef3id` | `$c2 $cb`, MX29LV640EB, 0.1 s |
| `--readef3` twice | Both images identical, `maxDelayNeeded = 0`, 61 s for 8 MB |
| `.crt` from the extraction | All 7 slots convert back to exactly the slot data in the image. Two of them start in VICE. |
| `--writeef3slot 3` with its own content (16 KB of data) | Written and verified in 12.5 s. A new full read is identical to the old one in all 8 MB. |
| `--writeef3slot 1` with a different 1 MB game (965,602 bytes not `$ff`) | Written and verified in 77 s. Slot 1 equals the file, slots 0 and 2–7 are unchanged. |
| `--ef3slotname 1 "Toutrun"` | Exactly the 16 bytes of the slot 1 entry changed in the whole 8 MB image |
| Name longer than 16 characters, confirmation answered with Enter | Refused or cancelled, nothing changed |
| `--dumpef3` | 8 MB read and extracted in 60.6 s: 7 `.crt`, 6 KERNALs, 3 freezers |
| `--writeef3slot 1` with a `.crt` (128 chips) | Loaded directly, written and verified in 76.5 s |
| `--writeef3` with the current image | 125 sectors unchanged, nothing written |
| `--writeef3` with the first image of the cartridge (other game in slot 1, KERNAL 6 empty, old names) | Exactly 18 sectors written: the 16 of slot 1, the KERNAL 6 boot sector and the directory; 107 unchanged. At most 91 s, including the time to type `yes` |
| `--writeef3` with the current image again | The same 18 sectors back; the empty KERNAL 6 sector was only written, not erased |
| `--writeef3all` with the current image | All 135 sectors unchanged, including the EF3 menu and EasyProg: the cartridge equals the image byte by byte in all 8 MB. At most 46 s, including the time to type `WRITE ALL` |
| `--ef3noconfirm --writeef3` with an image that has a test pattern in slot 0, bank `$1c` ROML (outside the known areas) | 125 sectors unchanged in 24.9 s; the menu area is not touched |
| `--ef3noconfirm --writeef3all` with the same image | Exactly the sector of banks `$18`–`$1f` ROML of slot 0 erased, written and verified, 134 unchanged |
| `--ef3noconfirm --writeef3all` with the image before | The same sector found changed, written back and verified, 134 unchanged; the cartridge equals the image before in all 8 MB |
| `--readef3slot 4`, `--readef3kernal 2`, `--readef3freezer 2` in one command line | 5.1 s; names and sizes as in the full dump |
| `--ef3kernalname 6 "Test"`, then `--writeef3kernal 6` with KERNAL 2 (KERNAL 6 was empty), then `--writeef3freezer 2` with its own content | All written and verified. Compared with the image before, only bank `$05` ROML (now equal to KERNAL 2) and the KERNAL 6 entry of the directory changed. Freezer 2 and everything else are unchanged. |
| `--ef3noconfirm --ef3freezername 2 "AR Test" --dumpef3 ... --ef3freezername 2 "Action Replay 6"` | No question; both names written and verified; the dump in between names the file `freezer2_ar_test.bin` |

Then with three EF3s with different content in programmer slots 1–3:

| Test | Result |
| --- | --- |
| `--slots 3 --ef3id` | 3 chips found; the slot register reads back 0 to 7 in all three slots |
| `--slots 3`, `--writeef3kernal 6` with a KERNAL, then with an empty 8 KB file (KERNAL 6 was empty in all three) | Written and verified in all three, about 1 s each. Read back afterwards, each EF3 is byte-identical to its backup |
| `--slots 3 --writeef3slot 1` with a 1 MB game; EF3 slot 1 was completely empty in one of the EF3s | The first version stopped before erasing, because the bank switch could not be proven in the empty slot. With the [exception for empty EF3 slots](#how-the-patch-solves-this): written and verified in all three in 66 s |
| A write without `--slots` while three different EF3s are inserted | Refused before writing: "Programmer slot 2 holds another EasyFlash 3 with different content", nothing changed |
| `--writeef3all` with each backup, one EF3 at a time in the front slot | Each EF3 byte-identical to its backup afterwards |
| Empty programmer slots 4 and 5 | Flash ID reads `$ff $ff` |
| A write while the EF3 is in slot 1 and another cartridge type in slot 2 | Refused: "Programmer slot 2 holds another cartridge (flash ID $09 $09)". That cartridge did not answer the autoselect command, so these are its normal data |
| A write without `--slots`, then `--slots 3 --eraseef3slot 3`, with the three different EF3s | Both refused, nothing changed |
| One EF3 made a copy of another with `--writeef3all`, both in programmer slots 1 and 2, then `--writeef3kernal 6` without `--slots` | Note "1 other programmer slot(s) read the same EasyFlash 3", then written. KERNAL 6 read from programmer slot 2 is byte-identical to the written file: the other EF3 got the writes without checks, as the note says. Both EF3s restored from their backups afterwards |

With one EF3 alone:

| Test | Result |
| --- | --- |
| `--writeef3slot 1` with two different 1 MB games | 33.0 s and 34.0 s |
| `--eraseef3slot 3` (16 KB used) | 2 sectors erased, the name set back to "Slot 3", 6.1 s |
| `--eraseef3all` | 103 sectors erased in 98.6 s. Read back afterwards: slots 1–7, all KERNALs and all freezers empty, all 26 names set back. Everything else in slot 0 (EF3 menu, EasyProg, the rest of the directory) unchanged |
| `--writeef3all` with the backup, then again | The cartridge restored (106 sectors in 158.5 s); the second run found all 135 sectors unchanged |
| `--ef3resetnames` on the restored EF3 | 15 names set back to the defaults and verified in 1.4 s, nothing else erased |

The existing `--dump` read bit 0 wrong in all three slots, as in the first version of `--readef3`.

### Tests with an emulated EF3

Before they ran on the real EF3, all system area options were tested with an emulation that replaces `Hardware.cpp`. It works at the level of the latches and emulates:

- the CPLD registers, which only take writes on a rising PHI2 edge with the select line active
- `data_out_valid` after reading `$de01`
- bus contention between the Pi and the CPLD
- the MX29LV640EB command sequences, with 8 KB boot sectors and 64 KB sectors

It starts with the real 8 MB image of the tested cartridge.

To check the emulation itself, the options that already worked on the hardware were run first. `--ef3id`, `--readef3` and `--writeef3slot` gave the same results as on the real EF3, down to the same number of programmed bytes.

| Test (emulation) | Result |
| --- | --- |
| `--readef3slot`, `--readef3kernal`, `--readef3freezer` | Byte-identical to the files from `--extractef3`. An empty KERNAL is reported and not written. |
| `--writeef3kernal 6` (empty) with another KERNAL | Only boot sector `$00a000` erased; only bank `$05` ROML changed |
| `--writeef3kernal 1` with its own content | Image unchanged |
| `--writeef3freezer 2` with the 64 KB Retro Replay | Only sector `$070000` erased; only banks `$18`–`$1f` ROMH changed |
| `--writeef3freezer 3` with its own content | Two sectors erased, image unchanged |
| KERNAL, freezer and slot renames | Only the bytes of the entry changed |
| Answer other than `yes`, file too big, KERNAL 9, freezer 4 | Refused, nothing changed |
| Writes to the bank register lost on purpose (bank 0, 5 and `$18`) | Bank proof fails, nothing erased, image unchanged, also with `--ef3noconfirm` |
| One command line with `--ef3noconfirm`: rename and write KERNAL 6, rename and rewrite freezer 2, write slot 2, `--dumpef3` | No question; only bank `$05` ROML, the directory and slot 2 changed; the final dump equals the emulated flash |

| `--writeef3` with the same image | All sectors unchanged, no erase |
| `--writeef3` with an image that differs in slot 5, KERNAL 6, Retro Replay, a directory name and the menu area | The result equals the image except the menu area, which is not written |
| `--writeef3all` with the same modified image | The result equals the image in all 8 MB; 19 sectors erased |
| `--writeef3all` answered with `yes` instead of `WRITE ALL`, a wrong file, an image without directory | Refused, nothing changed |
| Bank `$10` lost during `--writeef3` | A sector of slot 1 looked changed because the wrong bank was read. The first version erased it; now the bank proof stops before the erase, and nothing is changed |
| Bank 5 lost while writing a slot | Stops with a verify error in banks 0–7 of this slot; nothing outside this sector changes |

| `--slots 5 --ef3id` | 5 chips found |
| `--slots 5` with `--writeef3` and `--writeef3slot` | All 5 EF3s end up with the same data, each one verified. If one already holds the data, the others still get it. If all hold it, all sectors are skipped |
| `--slots 5`, bank switch lost only in programmer slot 4 | Bank proof fails for slot 4; nothing is erased in any slot |
| `--slots 5`, programmer slot 2 without unlock bypass | Fallback to the normal program command for all; all 5 correct |
| `--slots 5`, different directory in programmer slot 5, rename | Refused; nothing changed |
| `--slots 5` with only 4 EF3s | Refused before writing; nothing written |
| `--slots 5 --activeslot 2 --readef3` | Reads programmer slot 3 |
| `--slots 3`, target EF3 slot empty in programmer slot 3 | No proof needed there, all 3 written and verified |
| `--slots 3`, same, and switching to bank 0 lost in programmer slot 2 | Proof fails for slot 2 (its references are not empty), nothing changed |
| No `--slots`, three EF3s with different content | Refused, nothing changed in any of them |
| No `--slots`, three identical EF3s | Note, then written; all three correct |
| No `--slots`, another cartridge type in programmer slot 2 | Refused, nothing changed |
| `--eraseef3slot 3`, `--eraseef3kernal 2`, `--eraseef3freezer 3` | Exactly the area erased and its name set back; a name that is already the default does not rewrite the directory |
| `--eraseef3all` | Slots 1–7, all KERNALs and freezers empty, names 1–26 back to the defaults; the EF3 menu, EasyProg and the rest of slot 0 unchanged. A second run has nothing to do. `yes` instead of `ERASE ALL` cancels |
| `--slots 3 --eraseef3all` | Refused with different directories; with identical EF3s all three erased |
| `--ef3resetnames` | Names 1–26 set back, one sector erased (the directory), all contents unchanged. On an erased EF3 nothing to do |
| `--slots 2` with a third EF3 in programmer slot 3, or another cartridge in slot 4 | Refused, nothing changed |

In all tests there was no bus contention and no flash write outside a command sequence.

### Speed

The reads with ROML/ROMH kept active, the commands in the target bank and the skipping of unchanged sectors came last. In the emulation they were tested with the same cases, plus:

- an unchanged slot: 16 sectors skipped, no erase
- an empty slot: written without erase
- a slot with one changed byte: one sector erased

Then they were measured on the real EF3:

| Operation | Before | Now |
| --- | --- | --- |
| `--readef3`, 8 MB | 61 s | 27.1 s |
| `--writeef3slot`, full new slot (127 and 128 chips) | 77 s | 52.0 s and 49.7 s |
| `--writeef3slot`, unchanged slot (16 sectors skipped) | 77 s | 3.5 s |

- **Same data:** The image read with the faster reads is byte-identical to the image read before the speedup, and `maxDelayNeeded` stayed 0.
- **Two full writes:** Slot 1 was written with a different game and then with the original game again. A full read afterwards is identical to the image before, in all 8 MB.
- **Hardware accesses:** The emulation also counts them. The latch updates drop from 42 M to 8.4 M for a full read and from 68 M to 35 M for a full slot write.
- **What is left:** In a full slot write, a part of the time is the erase time of the 16 sectors in the flash chip.

### Second speedup

The second speedup has 4 instead of 5 latch updates per write, a 600 ns hold instead of at least 1 µs, and the unlock bypass. It was first tested in the emulation:

- **Both chip variants:** The emulation now knows the unlock bypass and can also behave like a flash without it. All 31 test cases pass with both: the bypass is used and the flash is back in read mode afterwards, and without bypass the fallback writes a correct slot.
- **Latch updates for a full slot:** 35.1 M before, 31.3 M without bypass, 17.8 M with bypass.

Then it was measured on the real EF3:

| Operation | First version | First speedup | Second speedup |
| --- | --- | --- | --- |
| `--writeef3slot`, full slot (128 and 127 chips) | 77 s | 50–52 s | 36.1 s and 37.4 s |

- The flash reported `Flash unlock bypass mode works`.
- A slot was written with another game and then with the original one again. After that, `--writeef3all` with the image from before found all 135 sectors unchanged, in 27.3 s. So the cartridge equals the image byte by byte.
- About 12 s of a full slot is the erase time of the flash, which cannot be made shorter.

### Target address for `$a0`

Sending `$a0` to the target address in bypass mode lowers the latch updates for a full slot from 17.8 M to 15.9 M. A full slot then took 33.0 s and 34.0 s on the hardware, instead of 36.1 s and 37.4 s.

### Not measured yet

- **5 EF3s at once:** With `--slots 5` a full slot takes 41.8 M latch updates and 28.4 M data reads for all 5 together. The erase runs in all of them at the same time. That is about 80 s for 5 EF3s, about 16 s each, estimated. With 3 EF3s it was measured: 66 s, about 22 s each, see [Tests](#tests).

## Limitations and ideas

- Only the active programmer slot; the slots of the five-slot programmer are not used in parallel.
- In slot 0 only the KERNALs, the freezers and the directory can be written on their own. The menu and EasyProg are only written by `--writeef3all`, on purpose.
- Only tested with one EF3 and the MX29LV640EB. `--ef3id` should be run before writing to another cartridge.
- The original EasyFlash (EF1) uses two AM29F040 chips and different logic. It was not tested.
- Writes to the system area wait for keyboard input unless `--ef3noconfirm` is given.
- PiCartTool calls `HardwareInit()` and `InitCartTool()` (with a cartridge reset) before every command. After the reset the EF3 is on slot 0, bank 0 again, so every EF3 option selects its slot and bank itself.
- A possible integration step is a cartridge type option for the EF3, so that the generic `--read`, `--erase` and `--write` could use the PHI2 edge writes and the EF3 command sequences.

## References

- EasyFlash 3 CPLD: https://gitlab.com/easyflash/easyflash3-core
- EAPI flash drivers: https://gitlab.com/easyflash/eapi
- EF3 boot image (menu, directory, layout): https://gitlab.com/easyflash/easyflash3-bootimage
- EasyProg: https://gitlab.com/easyflash/easyprog
