# sysmonF401

USB system monitor for a FreeBSD server: a **WeAct Studio Black Pill STM32F401** (USB-C)
drives a **1.9" ST7789 TFT** (170x320) and shows CPU, RAM, CPU temperature and network
download / upload rates, sent once a second by the server over USB serial.

| Folder | Runs on | Content |
|---|---|---|
| [firmware](firmware) | Black Pill F401 | PlatformIO / Arduino project: USB CDC serial + display |
| [freebsd](freebsd) | FreeBSD server | `sysmonf401` sender (Go) and its rc.d service |

```
+--------------------------------------------+
| srv1                                    (●) |   hostname, green dot = data is arriving
| CPU [██████░░░░░░░░░░░░░░░░░░░░]       23% |   bars turn red above 80 %
| RAM [█████████████░░░░░░░░░░░░░]       48% |
| TMP [████████████░░░░░░░░░░░░░░]      52°C |   red from 75 °C
|--------------------------------------------|
| ↓  12.3 Mo/s  |  graph: download (area)    |
| ↑ 820.0 Ko/s  |  and upload (line), ~160 s |
+--------------------------------------------+
```

## Wiring

The screen is driven by SPI1. USB uses PA11 / PA12, keep them free.

| ST7789 pin | Black Pill pin |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SCL | PA5 (SPI1 SCK) |
| SDA | PA7 (SPI1 MOSI) |
| RES | PA2 |
| DC | PA3 |
| CS | PA4 |
| BLK | PA1 (PWM, brightness) |

Other pins: change the `TFT_*` defines at the top of `firmware/src/main.cpp`.
Upside-down mounting: `#define ROTATION 3`.

## Firmware

Requires [PlatformIO](https://platformio.org/) (`pip install platformio`, or the VS Code extension).

```
cd firmware
pio run
```

Flash it through the STM32 ROM bootloader (USB DFU): hold **BOOT0**, press and release
**NRST**, release BOOT0, then:

```
pio run -t upload
```

On Windows, `dfu-util` needs the WinUSB driver on the "STM32 BOOTLOADER" device
(install it once with [Zadig](https://zadig.akeo.ie/)). Alternative without a driver change:
[STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html) in USB mode,
with `firmware/.pio/build/blackpill_f401cc/firmware.bin` at address `0x08000000`.

After a reset the board shows up as a USB CDC ACM serial port, VID `0483` / PID `5740`,
and the screen says "En attente du serveur" until data arrives.

Notes:

- `include/wiring_private.h` is empty on purpose: the Adafruit ST77xx library includes it,
  and the STM32 Arduino core has no such file.
- The Adafruit ST7789 library centres 170x320 panels itself (35-pixel column offset).

## FreeBSD sender

Build (Go 1.26 or later), on FreeBSD:

```
cd freebsd
go build -o sysmonf401 .
```

Or cross-compile from another machine: `GOOS=freebsd GOARCH=amd64 go build -o sysmonf401 .`

The board is handled by `umodem(4)`, which creates `/dev/cuaU0`. Check it:

```
usbconfig | grep -i 0483
sysctl dev.umodem
```

If `umodem` is not loaded: `kldload umodem` (and `umodem_load="YES"` in `/boot/loader.conf`).
For the CPU temperature: `kldload coretemp` (Intel) or `kldload amdtemp` (AMD), and
`sysrc kld_list+=coretemp` to keep it at boot.

`/dev/cuaU*` belongs to `uucp:dialer`: run as root, or add the user to `dialer`.

Try it by hand:

```
./sysmonf401
```

Options:

| Option | Default | Meaning |
|---|---|---|
| `-port /dev/cuaU0` | automatic | Serial port; found by VID/PID `0483:5740` if empty (`cuaU0` alone also works) |
| `-interval 1s` | `1s` | Update period |
| `-iface em0,igb1` | all but `lo*` | Interfaces counted for the network rates |
| `-host name` | short hostname | Name shown in the header |
| `-brightness N` | unchanged | Backlight 0-100, sent at start |
| `-no-arc` | off | Do not count the ZFS ARC as used RAM |

With jails, bridges or VLANs, the same traffic is counted on several interfaces:
use `-iface` with the physical interface.

### Service at boot

Same as miniscreen: `daemon(8)` restarts `sysmonf401` 5 s after any exit (board unplugged,
not attached yet) and sends its output to syslog (tag `sysmonf401`). As root, from `freebsd/`:

```
install -m 755 sysmonf401 /usr/local/bin/sysmonf401
install -m 555 rc.d/sysmonf401 /usr/local/etc/rc.d/sysmonf401
sysrc sysmonf401_enable=YES
sysrc sysmonf401_args="-iface em0"
service sysmonf401 start
```

When the data stops, the dot turns red after 5 s, and the screen goes back to
"En attente du serveur" after one minute.

## Protocol

One text line per sample, `key=value` pairs separated by spaces, ending with `\n`:

```
cpu=12.5 ram=43.1 temp=52.0 down=123456 up=7890 host=srv1
```

| Key | Value |
|---|---|
| `cpu`, `ram` | Percent used |
| `temp` | CPU temperature, °C |
| `down`, `up` | Network rates, bytes per second |
| `host` | Name shown in the header (no spaces) |
| `bri` | Backlight 0-100; can be sent alone |

A missing key shows `--`. So the board can be fed by anything, for example from a shell:
`echo "cpu=50 ram=20 host=test" > /dev/cuaU0`.

Sources: `kern.cp_time` (CPU), `hw.physmem` and `vm.stats.vm.*` (RAM, available = free +
inactive + laundry pages, as in `top`), `dev.cpu.N.temperature` (hottest core),
`netstat -ibn --libxo json` (network).
