# can_latest — How It Works

## What this module does

`can_latest.c` is the brain of the system. It:

1. **Receives** button press events from devices on the CAN bus
2. **Decides** which output column that button press belongs to
3. **Advances** the LED state for that column (cycles through colors, selects a radio LED, etc.)
4. **Updates** the video matrix crosspoint (`videomtx_set`) — routing one video input to one output
5. **Sends** the new LED state back to all relevant devices over CAN

---

## The Big Picture

```
CAN bus
  │
  ▼
can_latest_update()          ← called for every received CAN frame
  │
  ├─ Is it a button press? (dlc=4, data[3]=0x18) ──── No ──► store raw frame, exit
  │
  ├─ Parse button event (who pressed what)
  │
  ├─ resolve_col() ──► which of the 16 output columns owns this button?
  │
  ├─ Compute new LED state for that column (mode-specific logic)
  │
  ├─ videomtx_set(col, row) ──► update video routing
  │
  └─ send_to_col_addrs(col) ──► send LED update frame(s) over CAN
```

---

## The 16×16 Matrix

The system manages 16 **output columns**. Each column can be routed to one of 16 **input rows**. The active routing is stored in the video matrix (`videomtx`).

The **color matrix** (loaded from NVS via `prog_get_matrix()`) defines which rows are valid for each column and what color they show:

```
         col 0   col 1   col 2  ...  col 15
row 0  [  RED  ] [  0   ] [ GRN ] ... [  0  ]
row 1  [ GREEN ] [  0   ] [  0  ] ... [  0  ]
row 2  [   0  ] [ YEL  ] [ RED ] ... [ GRN ]
  ...
row 15 [   0  ] [  0   ] [  0  ] ... [  0  ]
```

- `0` = this cell is unused for this column
- Non-zero = a valid routing state with a specific LED color (1=Green, 2=Red, 3=Yellow, 4=Black)

At startup, `can_latest_configure()` reads this matrix and builds a compact list of valid states per column into `led_seq_t.buf[]` and `led_seq_t.rows[]`.

---

## Column Modes

Each of the 16 output columns has one of 6 modes, configured in NVS:

### MODE_NORMAL (0) and MODE_NORMAL_SECONDARY (4)

**One button, up to 4 states.** Each button press advances the column through its states in order (Green → Red → Yellow → Black → wrap around).

```
Press 1 → state 0 (e.g. Green,  routes to row 2)
Press 2 → state 1 (e.g. Red,    routes to row 7)
Press 3 → state 2 (e.g. Yellow, routes to row 11)
Press 4 → state 0 (wraps back)
```

NORMAL_SECONDARY behaves identically but sends to LED layer `base_addr + 1` instead of `base_addr`. This allows two columns sharing the same physical device to drive different LED groups.

---

### MODE_6STEP (1) and MODE_6STEP_SECONDARY (5)

**Two buttons, 6 states.** The states are split into two halves:
- Button A controls the first half (states 0–2)
- Button B controls the second half (states 3–5)

Each button independently cycles within its own half. Switching from one button to the other resets that button's half to its first state.

```
Button A presses: state 0 → 1 → 2 → 0 (wraps within first half)
Button B presses: state 3 → 4 → 5 → 3 (wraps within second half)
Switching A→B: B starts from state 3, A resets to state 0
```

6STEP_SECONDARY behaves identically but sends to LED layer `base_addr + 2`.

---

### MODE_RADIO_GREEN (2) and MODE_RADIO_RED (3)

**Up to 16 states, radiobutton behaviour.** Exactly one LED is lit at a time. Whichever button was pressed becomes the active one — no cycling, just direct selection.

```
Press LED 3 → LED 3 on,  all others off
Press LED 7 → LED 7 on,  all others off
```

RADIO_GREEN and RADIO_RED are always configured in pairs on the same physical LED hardware. RADIO_GREEN controls the green bit of each LED group; RADIO_RED controls the red bit. They share the same `base_addr` so their updates go to the same physical LEDs.

A `BUTTON_TYPE_HH` press (type=1) routes to RADIO_GREEN.
A `BUTTON_TYPE_CSEND` press (type=8) routes to RADIO_RED.

---

## CAN Frame Format — Incoming Button Press

A button press arrives as a 4-byte CAN frame:

```
data[0] = device address - 1     (e.g. device 5 → 0x04)
data[1] = button type             (1 = HH, 8 = CSEND)
data[2] = button ID - 1          (absolute LED address on the bus minus 1)
data[3] = 0x18                   (identifies this as a button press message)
```

Any frame where `data[3] != 0x18` or `dlc != 4` is ignored (but still stored as the latest raw frame).

---

## How a Button Press is Processed

### Step 1 — Parse the frame

```c
button_event_t event = {
    .dev_addr  = data[0] + 1,   // restore 1-based address
    .type      = data[1],
    .button_id = data[2] + 1,   // restore 1-based button ID
};
```

### Step 2 — Find the column (`resolve_col`)

The module maintains a reverse lookup table `s_addr_to_col_mask[256]`. Each entry is a 16-bit bitmask where bit N means "column N has this device address in its address list."

```
s_addr_to_col_mask[device_addr]  →  bitmask of matching columns
```

**If only 1 column matches** → use it directly.

**If 2 columns match** (e.g. a primary + secondary pair, or a RADIO_GREEN + RADIO_RED pair):

- For NORMAL / NORMAL_SECONDARY pairs: `button_id % 8` selects which column
  - position 1 → NORMAL
  - position 2 → NORMAL_SECONDARY
- For 6STEP / 6STEP_SECONDARY pairs: same idea
  - positions 1–2 → 6STEP
  - positions 3–4 → 6STEP_SECONDARY
- For RADIO_GREEN / RADIO_RED pairs: button type selects which column
  - HH (type=1) → RADIO_GREEN
  - CSEND (type=8) → RADIO_RED

### Step 3 — Compute `whichLed`

```c
whichLed = button_id - base_addr
```

This converts the absolute bus address of the button into a 0-based index within the column's LED group. For radio modes this is the selected LED index; for 6-step modes it identifies which button (0/1 or 2/3) was pressed.

### Step 4 — Update LED state (mode-specific)

Each mode updates `LedBuff` (the 4-byte LED data to transmit) differently. See the LED Buffer section below.

### Step 5 — Update video matrix

```c
videomtx_set(col, s_cols[col].rows[sel_idx]);
```

`sel_idx` is the index of the newly active state. `rows[sel_idx]` is the matrix row that corresponds to it — this is the video input that column now routes to.

### Step 6 — Transmit

`send_to_col_addrs(col)` sends the new LED state to every device address registered for that column (up to 4 addresses). Columns with more than 8 LEDs get two CAN frames (one per LED layer).

---

## LED Buffer Format (`LedBuff`)

LEDs on the bus use a packed 2-bits-per-LED format. Each byte holds 4 LEDs:

```
Byte layout:  [ z1 p1 z2 p2 z3 p3 z4 p4 ]
               bit7 ...              bit0

  z = green bit of that LED (even bit positions: 0, 2, 4, 6)
  p = red   bit of that LED (odd  bit positions: 1, 3, 5, 7)

  00 = off/black
  01 = green
  10 = red
  11 = yellow
```

4 bytes × 4 LEDs = 16 LEDs total per column.

**How each mode uses LedBuff:**

| Mode               | Bits used in LedBuff[0] | Notes |
|--------------------|------------------------|-------|
| NORMAL             | [1:0]                  | 2-bit color for LED 0 |
| NORMAL_SECONDARY   | [3:2]                  | 2-bit color for LED 1 |
| 6STEP              | [3:0]                  | 2-bit color × 2 buttons |
| 6STEP_SECONDARY    | [7:4]                  | 2-bit color × 2 buttons |
| RADIO_GREEN        | even bits across all 4 bytes | one green bit per LED |
| RADIO_RED          | odd  bits across all 4 bytes | one red  bit per LED |

---

## TX Frame Format — Outgoing LED Update

```
data[0] = device address - 1
data[1] = LedBuff[1]            ← bytes are swapped intentionally
data[2] = LedBuff[0]
data[3] = LED layer index       ← computed by baseAddressToLedLayer(base_addr)
```

For columns with > 8 LEDs, a second frame is sent for the next layer:
```
data[3] = LED layer index + 1
data[1] = LedBuff[3]
data[2] = LedBuff[2]
```

`baseAddressToLedLayer(base_addr)` converts the NVS-stored base address into the layer index the LED hardware expects:
```
layer = 48 + ((base_addr - 128) / 8)
```

---

## Per-Column State (`led_seq_t`)

Every column has one `led_seq_t` struct in the `s_cols[16]` array:

| Field          | What it holds |
|----------------|---------------|
| `buf[]`        | Sequence of color values (0–3) to cycle through, built from the matrix at configure time |
| `rows[]`       | Matrix row index for each `buf` entry — tells `videomtx_set` which input to route |
| `len`          | Number of valid entries in `buf` / `rows` |
| `ptr`          | Next position to read from `buf` (NORMAL / 6STEP modes only) |
| `sel_idx`      | Index of the last-sent state — used by `can_latest_broadcast()` to resend state after power-up |
| `mode`         | One of the 6 mode constants |
| `base_addr`    | First LED layer address for this column (from NVS `PROP_LED_BASE_ADDR`) |
| `addrToSendTo` | Up to 4 device addresses that receive LED updates; `0xFF` = unused slot |

---

## Startup Sequence

```
app_main()
  │
  ├── can_latest_init()        ← creates the mutex (must be before any task starts)
  │
  ├── prog_init()              ← loads matrix + column props from NVS
  │
  ├── can_latest_configure()   ← reads prog data, fills s_cols[], builds s_addr_to_col_mask[]
  │
  └── can_latest_broadcast()   ← sends sel_idx=0 state for every column to all devices
                                  (syncs hardware LEDs to firmware state at boot)
```

---

## Thread Safety

- `can_latest_update()` and `can_latest_broadcast()` run in the CAN task.
- `can_latest_get()` can be called from any task (e.g. the UI task) — it is protected by a mutex.
- `videomtx_set()` is safe to call from the CAN task; it triggers the LVGL notify callback via `lv_async_call`.
