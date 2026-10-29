# phibat

The battery's draw in watts, as a number in the tray. One small exe, nothing to configure.

Windows shows the percentage and a guess at the time left; it never says how hard the
machine is pulling on the battery right now. phibat asks the battery itself every two
seconds and draws the watts next to the battery icon:

- one decimal below 10 W, whole watts above; white or black digits, whichever the taskbar
  needs; green while charging
- `AC` on mains with a full battery, `--` without a battery, `?` for a battery that
  does not report its rate
- hover for the exact figure, the charge and the time left
- left click opens the battery settings; right click: **Start with Windows**, **Quit**
- on its first run it moves itself out of the hidden overflow menu, next to the battery icon;
  hide it again by dragging and it stays hidden

## Install

```
winget install mechanicsunlocked.phibat
phibat
```

or take `phibat.exe` from the [latest release](../../releases/latest) and run it from
anywhere. It is a single static exe with no services, no installer and no files of its own;
the one thing it writes is the autostart entry, if you ask for it.

## Build

```
make                       # Linux, needs mingw-w64
make CROSS=                # Windows, in an MSYS2 UCRT64 shell
```

## How it reads the battery

Every battery device is opened through its device interface and asked for
`IOCTL_BATTERY_QUERY_STATUS`. The rate comes back in milliwatts, negative while
discharging, and is summed over batteries. Charge and time left come from
`GetSystemPowerStatus`. Batteries that report in relative units (rare) get a `?`.

MIT License.
