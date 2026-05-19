# UI Save / Restore Behaviour

Snapshot is taken when a screen is **opened**. Changes are live (visible immediately)
but not permanent until confirmed. Long-press always means **cancel**.

| Screen | Entry | Click | Long-press |
|---|---|---|---|
| CAN Rate | saves `s_rate_idx` | apply → back to Settings | restore → back to Settings |
| Brightness | saves `s_brightness_pct` | apply → back to Menu | restore display → back to Menu |
| Network | saves all 6 fields¹ | — | — |
| Network → Back btn | — | restores all 6 fields → Menu | — |
| Network → Apply btn | — | keeps new values, sets `applied` flag → Menu | — |
| NTP picker | (uses Network snapshot) | save selection → back to Network | restore NTP → back to Network |
| IP editor | (uses Network snapshot) | advance octet / finish → back to Network | restore this field → back to Network |

¹ Fields saved on Network entry: `dhcp`, `ip[4]`, `mask[4]`, `gw[4]`, `ntp_idx`, `ntp_ip[4]`

## Notes

- `ui_open_network` saves the snapshot; `ui_show_network` (internal return from sub-screens) does **not** — so NTP and IP edits during a session are all reverted together by one Back press.
- After IP editing completes (4th octet click), the user returns to Network — the field value is already written into the live state but the Network snapshot still holds the original, so Back still reverts it.
- `ENCODER_LONG` in `SCREEN_MAIN` opens the menu (unchanged).
