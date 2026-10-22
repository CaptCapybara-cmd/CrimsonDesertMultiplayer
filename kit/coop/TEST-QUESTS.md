# Two-PC test: shared quests

This kit is the normal co-op test kit with **quest sharing** switched on (`[coop] stage_sync=1` and
`quest_share=1` in both configs). It is experimental. What should happen:

- When the **host** accepts a request from an NPC, the **guest** gets the same request in their own game, even if
  they were somewhere else (it is applied once the guest comes near that NPC).
- The NPC's meeting scene is replayed for the guest too, so the guest "knows" the NPC and can hand in.
- **Each player cooks and hands in their own request.** The host's hand-in never completes the guest's.

The test below takes about 30 minutes.

## Before you start (both players)

1. Same game build: Crimson Desert 2.03.02 on Steam (`CrimsonDesert.exe` 1.0.0.2976). The mod refuses other builds.
2. Be Steam friends, both online in Steam.
3. **Back up your saves**: `%LOCALAPPDATA%\Pearl Abyss\CD\save` (Windows) or the same folder inside the Proton prefix
   (Linux, `steamapps/compatdata/3321460/pfx/drive_c/users/steamuser/AppData/Local/Pearl Abyss/CD/save`). The mod
   changes quest state in your save once you save.
4. Both saves should be at a point where the request you test is available and **not yet accepted** by either of
   you. The one this was built on is "Renee's Request" from Renee, the butcher in Hernand (cook Fish Porridge x3).
5. Install: copy every file of the kit into the game's `bin64` folder (next to `CrimsonDesert.exe`). Check them
   against `SHA256.txt` if you like.
   - Host: rename `cdmp.ini.anfitrion` to `cdmp.ini` (replace the one there). Guest: keep `cdmp.ini` (it is the guest's).
   - Set your name: `[general] player_name=` in `cdmp.ini`.
   - Linux/Proton only: Steam launch options `WINEDLLOVERRIDES="winmm=n,b" %command%`.

## The test

Each step: do it, then write down what you saw (yes / no / what happened instead).

1. **Connect.** Host loads their save first, then the guest. You should see each other (name marker above the
   head). If not, stop here and send the logs (below).
2. **Guest goes elsewhere.** The guest walks or rides away from Hernand's butcher (far enough that Renee is not
   loaded, e.g. another district or outside town).
3. **Host accepts the request.** The host walks to Renee, talks to her and accepts Renee's Request. Host: does the
   quest log show it? (It should, as without the mod.)
4. **Guest: not yet.** The guest's quest log should NOT show it yet (Renee is not loaded on the guest's side).
5. **Guest comes to Renee.** The guest walks to the butcher. Within a few seconds of arriving, the guest's quest log
   should show Renee's Request (Fish Porridge). The guest may see her shop screen or a short scene; close it as usual.
6. **Both cook and hand in, host first.** The host cooks 3 Fish Porridge and hands them in to Renee. Then check on the
   guest: the guest's request must still be open (not completed).
7. **Guest hands in.** The guest cooks 3 Fish Porridge and hands them in. Does the "hand in" option work for the guest,
   and does the request complete with a reward?
8. **Reconnect (optional).** Pick another request both of you have available. The guest quits to the main menu (or
   closes the game) while the host talks to that NPC and accepts. The guest comes back and walks to the NPC: does the
   guest get the request?

## When something looks wrong

**Both players press Ctrl+F12 at the same moment** ("now!"). Each game then saves a flight record
(`bin64\flight\*.cofr`) and writes a numbered mark in `cdmp.log`. Write down which step it was.

## What to send back

From **both** PCs, from the `bin64` folder:

- the `flight` folder (`*.cofr`, small): the flight recorder keeps the last minutes of what each game shared and
  applied and saves `...-latest.cofr` every 30 s, plus one file per Ctrl+F12 and per failed shared accept. With both
  PCs' files, `python -I tools/flight/flight.py diverge host.cofr guest.cofr` (coop-anything) shows where the two games
  first disagreed;
- `cdmp.log` (and `cdmp.1.log` ... if there are several);
- if you can: `cdmp_packets_*.log` and `.bin` (`[sniffer] record=1`, 50-100 MB per session), which show exactly what each
  game did.

Plus your notes from each step.

In `cdmp.log`, the interesting lines start with `Quest share:` and `Stage sync:` (the guest's also say whether an accept
was applied, waited, retried or given up).

## If something goes wrong

- Quest sharing on its own: set `quest_share=0` (and `stage_sync=0`) in `[coop]`; the rest of co-op works as before.
- Restore your backed-up saves if a quest ended up in a state you do not want.
