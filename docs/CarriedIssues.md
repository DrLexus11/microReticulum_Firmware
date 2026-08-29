# Issues carried between branches

Faults that outlive the branch they were found on. Recorded here so they are not
rediscovered, and so a branch that happens to touch the same area knows what is
already known.

## 1. Rev 1 resets roughly every two hours, cause unknown

**Status: open. Not memory exhaustion, and not caused by the observer.**

### What was measured

A metrics poller held **one** serial connection open for two hours and read
uptime, internal free heap, largest free block, PSRAM free and stack margin over
provisioning every two minutes. Uptime is the ground truth: it cannot be faked
by a reconnect, and no port was opened or closed during the window.

Sixty samples. The board ran 6608 seconds -- one hour fifty -- and then reset:

    uptime=6008   internal=57516   largest=40948
    uptime=6248   internal=54084   largest=40948
    uptime=6608   internal=53084   largest=36852
    uptime=106    internal=60332   largest=45044   <- restarted here
    uptime=466    internal=59944   largest=47092

### What that rules out

- **Not memory exhaustion.** Free internal heap at the moment of the reset was
  53084 bytes, about 21% of the pool. `RNS_LOW_MEMORY_REBOOT` fires at 98%
  consumption and cannot have been the cause.
- **Not the observer.** Opening or closing the USB CDC port *does* reset this
  board -- uptime read 7 seconds immediately after one reattach during earlier
  diagnosis, and that invalidated the reset counts gathered that way. This run
  had no such interference. The reset is real.

That second point **corrects** [`BridgeBacklog.md`](BridgeBacklog.md) §5, which
says a large share of the resets were self-inflicted. That was true of the ones
counted during console captures. It is not true in general, and the conclusion
drawn from it -- that removing the observer might remove the resets -- is wrong.

### What is still unexplained

The reset reason. Earlier bootlogs recorded these as `UNKNOWN (0)`, alongside
`SW (ESP.restart)` and one `TASK_WDT`. Which of those this was is not known,
because reading the boot banner requires attaching to the console, which itself
resets the board.

There is a real, slower heap decline underneath it: flat at ~57.5 KB for an
hour, then 57516 to 53084 over twenty minutes, with the largest free block
stepping 42996 to 40948 to 36852. That period coincides with bridge test traffic
crossing the mesh, and Rev 1 is a transport node carrying it, so load is the
likeliest explanation. It recovered fully across the restart. It is worth
watching but it is not what caused the reset.

### The fix that did work, so it is not confused with this

The KISS listener was closing healthy clients after 6.5 seconds of no readable
data, and Columba reconnected every 11.8 seconds -- over 300 times an hour, each
cycle churning lwIP socket state in DMA-capable internal RAM that never spills
to PSRAM. That is fixed and independently proven by connection counts (17 per
200 s, then 1 per 240 s, then a single held connection). It removed a ~15 KB per
hour drain. It did not remove these resets, and the two should not be conflated.

### Next step

Expose the reset reason and a boot counter as provisioning metrics, the way heap
and uptime already are. The poller can then read *why* the board restarted
without attaching to a console that causes a restart of its own. That circular
problem is the only reason this is still open, and it is the same move --
measure it where the measurement does not disturb it -- that turned the memory
question from folklore into a number.

### Why it belongs on the Bluetooth branch

The overhaul replaces a stack that owns memory, its own task and its own
callbacks, on these same boards. A board that resets every couple of hours for
unknown reasons will make any Bluetooth stability result ambiguous: a dropped
link at the two-hour mark could be the new stack or could be this. Either settle
it first, or run BLE acceptance on Rev 2 and treat any Rev 1 reset as suspect
until proven otherwise.
