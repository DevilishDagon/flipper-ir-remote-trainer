# IR Remote Trainer

A Flipper Zero app that turns the **universal IR brute-force database into a save-as-you-go remote builder**.

Ever found a working code with the Flipper's universal remote — a fan, a cheap soundbar, an AC — but had no way to *keep* it? The universal remote blasts hundreds of codes per button press and never tells you which one worked, and "Learn" only captures signals from a physical remote you point at the Flipper. If the device came with no remote (or you lost it), you were stuck.

**IR Remote Trainer fills that gap.** Point it at any `.ir` database, step through the codes **one at a time**, transmit each one, and when your device reacts, **save that single code** into a brand-new remote with a button name you choose. The result is an ordinary saved remote you can use like any other.

No computer, no knowing your device's model number, no hunting for a pre-made remote file.

---

## What it does

1. Browse to any `.ir` file — e.g. the built-in universal database at `infrared/assets/fans.ir`, or any file from [Flipper-IRDB](https://github.com/Lucaslhm/Flipper-IRDB).
2. Name the new remote you're about to build (e.g. `Fan`).
3. Step through every code in the file, one at a time, and **test** each with a button press.
4. When the device responds, **save** that code under a name you type (`Speed up`, `Power`, `Oscillate`…).
5. Repeat for every function. Open your new remote under **Infrared → Saved Remotes**.

## Controls

| Key | Action |
| --- | --- |
| **OK** | Transmit (test) the current code |
| **Up / Down** | Previous / next code |
| **Left** | Jump back 10 codes |
| **Right** | Save the current code (prompts for a button name) |
| **Back** | Exit |

## Install (pre-built)

1. Download `ir_trainer.fap` from the [latest release](../../releases) (or build it — see below).
2. Copy it onto your Flipper's SD card at `apps/Infrared/`.
3. On the Flipper: **Apps → Infrared → IR Remote Trainer**.

## Usage walkthrough (fan with no remote)

1. Launch the app; a file browser opens in the `infrared` folder.
2. Go into `assets/` and pick **`fans.ir`** (note the plural — that's the built-in universal fan database).
3. Type a remote name (`Fan`) and confirm.
4. Point the Flipper at the fan. Press **OK** to test the first code, **Down** for the next, and so on.
5. When the fan reacts, press **Right**, name the button (`Speed up`), done.
6. Move on to the other functions. Everything you save lands in `infrared/Fan.ir`.
7. Play it back any time: **Infrared → Saved Remotes → Fan**.

> Tip: `fans.ir` holds a few thousand codes. Use **Left** to jump back 10 if you overshoot. IR is one-way, so if a code seems weak, press **OK** again.

## Build from source

Uses [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt), the micro Flipper Build Tool.

```bash
pip install ufbt

# Target Momentum firmware's SDK (see Compatibility below for other firmwares)
ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json

# From the repo root
ufbt
```

The compiled app lands at `dist/ir_trainer.fap`. To flash a connected Flipper directly: `ufbt launch`.

## How it works

Flipper's high-level infrared *signal* library (`infrared_signal_*`) is private and can't be linked by external apps. So this app uses only firmware-exported APIs:

- **Reading `.ir` files** with `flipper_format` — it iterates the file code-by-code by name, then parses each entry on demand.
- **Transmitting** with `infrared_send` (for decoded/parsed protocols like NEC, Samsung, RC5…) and `infrared_send_raw_ext` (for raw timing signals).
- **Saving** by writing a standard `.ir` remote file — so the output is 100% compatible with the stock Infrared app.

Both parsed and raw signal types are supported. Saves append to the target file, so you can add buttons across multiple sessions.

## Compatibility

- **Built and tested against Momentum firmware** (`mntm-012`, API 87.1) on the Flipper Zero.
- Because it relies only on stable, firmware-exported APIs, it should also build for official firmware and other forks (Unleashed, RogueMaster) by pointing `ufbt update` at the corresponding SDK — though those haven't been verified yet. Reports welcome.
- Note: the built-in universal *fan* database (`fans.ir`) ships with Momentum and some forks but not stock official firmware. On any firmware you can still use any `.ir` database file (e.g. from Flipper-IRDB).

## Limitations / roadmap

- No on-screen "sending…" feedback yet.
- No in-app rename/reorder of saved buttons (edit the `.ir` file or use the stock app).
- Very large databases are capped at 4096 codes.
- Not yet submitted to the Flipper app catalog.

PRs welcome.

## License

[MIT](LICENSE) © Rob Kaltenbach

## Acknowledgements

Built for the Flipper Zero community. IR databases referenced: the Momentum firmware universal assets and [Flipper-IRDB](https://github.com/Lucaslhm/Flipper-IRDB).
