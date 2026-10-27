# common/protocol

Message structs, encode/decode and CRC/parity shared by more than one
device. This is the **only** place packet definitions live in code; the
format itself is described in [docs/protocol.md](../../docs/protocol.md).

Empty for now: the packet formats in use are still defined inside
prototypes under [tools/bringup](../../tools/bringup). Move them here as
soon as a second device needs them.
