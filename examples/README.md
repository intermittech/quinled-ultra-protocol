# Wire-3 codec example

`host_frame_example.c` builds two slots and a wire-3 trailer. It demonstrates
a light above index 15 and per-output stream selection. TIME is illustrative;
this is a byte-layout example, not a scheduler/transport. Real transports must
satisfy `docs/timing.md` and validate against the receiver descriptor. The `sender/` library implements the wire-3 host scheduler and encoders.

```text
cc -std=c99 -Wall -Wextra -Werror host_frame_example.c -o host_frame_example
./host_frame_example
```

See [wire-3 sender notes](../docs/senders.md) for per-port profiles.

[Dig2Analog-Plus-5ch](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Dig2Analog-Plus-5ch) (board 1) ALWAYS forwards: LINK.FORWARD is set on its
single-wire command link, CHAIN_END is clear. It has NO return channel:
RETURN and RETURN_RELAY are clear, reply=NONE, return timing/rate fields zero.
There is no chain/no-chain assembly bit or variant. Its only assembly variants
are 3 A and 5 A channels, represented by opaque IDs owned by board firmware.
The firmware artifact/jig supplies its descriptor to the inventory; absence of
replies is expected and never means an empty port. It cannot relay downstream
telemetry. UPDATE_FORWARD is a separate bootloader capability, not implied by
FORWARD. This allocation document is not a numeric assembly-ID registry.
