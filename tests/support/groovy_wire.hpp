#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// The Groovy_MiSTer wire vocabulary, as the tests expect to see it on the socket.
//
// These values are deliberately NOT taken from the production headers. The wire
// format is Groovy_MiSTer's contract, not ours: if the tests read the opcodes and
// frame sizes out of the code under test, then "the transport sends CMD_INIT as
// opcode 2 in a 5-byte datagram" could never fail. They are pinned here once so
// every suite and both fakes agree, without agreeing with the implementation.
namespace mistercast::test {

// Command opcodes, in the first byte of a command datagram.
constexpr uint8_t kClose = 1, kInit = 2, kSwitchMode = 3, kAudio = 4, kBlit = 7;

// Exact datagram size of each command. kBlit is 8 bytes uncompressed and 12 with
// the trailing compressed-size field; everything else is fixed. A command is
// followed by its payload (PCM, pixels) in separate datagrams, which carry no
// opcode and must not be decoded as commands.
constexpr size_t kCloseBytes = 1, kInitBytes = 5, kSwitchModeBytes = 26,
                 kAudioBytes = 3, kBlitBytes = 8, kBlitCompressedBytes = 12;

// Status bits in the last byte of the 13-byte acknowledgement.
constexpr uint8_t kVramSynced = 0x04, kFrameskip = 0x08, kVblank = 0x10,
                  kFpgaField = 0x20, kCoreAudioOn = 0x40, kQueuePresent = 0x80;

// A receiver that is keeping up: VRAM synced with a frame queued.
constexpr uint8_t kHealthy = kVramSynced | kQueuePresent;
constexpr uint8_t kHealthyWithAudio = kHealthy | kCoreAudioOn;
// Synced but falling back to VGA frameskip with nothing queued, which is what
// drives the unhealthy side of the diagnostics.
constexpr uint8_t kUnhealthy = kFrameskip | kVramSynced;

// Little-endian field readers, for decoding straight out of a receive buffer
// without copying it first.
template <class T>
T readWire(const uint8_t* bytes, size_t size, size_t offset) noexcept {
  T value{};
  if (offset + sizeof(value) <= size) std::memcpy(&value, bytes + offset, sizeof(value));
  return value;
}

// True when the datagram is the named command at its documented size.
constexpr bool isGroovyCommand(uint8_t first, size_t size,
                               uint8_t command) noexcept {
  if (first != command) return false;
  switch (command) {
    case kClose:
      return size == kCloseBytes;
    case kInit:
      return size == kInitBytes;
    case kSwitchMode:
      return size == kSwitchModeBytes;
    case kAudio:
      return size == kAudioBytes;
    case kBlit:
      return size == kBlitBytes || size == kBlitCompressedBytes;
    default:
      return true;
  }
}

}  // namespace mistercast::test
