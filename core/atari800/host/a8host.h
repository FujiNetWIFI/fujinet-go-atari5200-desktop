/* The atari800 core's side of the host bridge.

   platform.c implements atari800's PLATFORM_* hooks over this state; the
   C++ host (A52Host.cpp) fills it in on the emulation thread between
   frames. Nothing here is thread-safe: only the emulation thread touches it. */
#ifndef A8HOST_H
#define A8HOST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct a8host_callbacks {
	/* One frame of PCM: signed 16-bit native-endian, Sound_out.channels. */
	void (*sound)(void *user, const void *pcm, unsigned bytes);
	/* A break (breakpoint, step, BRK, CIM): park until the debugger
	   resumes. The CPU registers are already synced to CPU_reg*. */
	void (*brk)(void *user);
	/* Inside Atari800_Initialise, where atari800 would read its config
	   file (PLATFORM_ConfigInit): apply the host's options. */
	void (*config)(void *user);
	void *user;
} a8host_callbacks;

void a8host_set_callbacks(const a8host_callbacks *cb);

/* The host-served cartridge (the FujiNet 5200 cart): atari800 routes every
   hardware access to $4000-$BFFF (patch 0006) and every JMP (abcd) here.
   cart_get/put run on the emulation thread. NULL clears them. */
typedef struct a8host_cart {
	unsigned char (*get)(void *user, unsigned short addr, int no_side_effects);
	void (*put)(void *user, unsigned short addr, unsigned char value);
	void (*jmp_ind)(void *user, unsigned short pc);
	void *user;
} a8host_cart;
void a8host_set_cart(const a8host_cart *cart);

/* Joystick nibbles (INPUT_STICK_*, 0x0f centred) and triggers (1 released,
   0 pressed) for ports 1-4, and the eight POT lines: 114 = centred; 228 is
   what an unplugged line reads, also taken as "no analog" (the digital
   stick decides). 5200 pots: 2*port = X (6 left .. 220 right), 2*port+1 = Y
   (6 up .. 220 down). */
extern unsigned char a8host_stick[4];
extern unsigned char a8host_trig[4];
extern unsigned char a8host_pot[8];

/* The rate the session wants (Sound_desired.freq is set from it). */
#define A8HOST_AUDIO_RATE 48000

#ifdef __cplusplus
}
#endif

#endif
