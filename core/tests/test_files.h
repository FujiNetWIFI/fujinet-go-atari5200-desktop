/* Tiny 5200 cartridges the tests write themselves. No third-party bytes.
 *
 * The program: SEI CLD LDX #$FF TXS / LDA #$AA STA $0400 / loop: INC $0401
 * JMP loop. It needs no interrupts and no BIOS service, so "the game runs" is
 * "RAM $0400 = $AA and $0401 moves". The header is the BIOS's: $BFFC year,
 * $BFFD = $FF (no title screen), $BFFE the entry. */
#ifndef A52_TEST_FILES_H
#define A52_TEST_FILES_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* An image of `size` bytes ending at $BFFF. claim: the "FUJI" claim at $BFE0
 * (a FujiNet app that keeps the mailbox). */
static void make_game(uint8_t *img, unsigned size, int claim)
{
    const unsigned base = 0xC000u - size;      /* the CPU address of byte 0 */
    const unsigned loop = base + 10;
    const uint8_t code[] = {0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0xAA, 0x8D, 0x00, 0x04,
                            0xEE, 0x01, 0x04, 0x4C, (uint8_t)loop, (uint8_t)(loop >> 8)};
    memset(img, 0xEA, size);
    memcpy(img, code, sizeof code);
    img[size - 4] = 0xFF;
    img[size - 3] = 0xFF;
    img[size - 2] = (uint8_t)base;
    img[size - 1] = (uint8_t)(base >> 8);
    if (claim) {
        memcpy(img + size - 0x20, "FUJI", 4);   /* $BFE0 */
        img[size - 0x20 + 4] = 1;
        img[size - 0x20 + 7] = 0x02;            /* $BFE7: PAL-compatible */
    }
}

static int write_game(const char *path, unsigned size, int claim)
{
    static uint8_t img[0x20000];
    FILE *fp;
    make_game(img, size, claim);
    fp = fopen(path, "wb");
    if (!fp)
        return -1;
    fwrite(img, 1, size, fp);
    return fclose(fp);
}

/* The same with a 16-byte .car header (type 4: 5200 32K) in front. */
static int write_game_car(const char *path)
{
    static uint8_t img[16 + 0x8000];
    FILE *fp;
    memset(img, 0, 16);
    memcpy(img, "CART", 4);
    img[7] = 4;
    make_game(img + 16, 0x8000, 0);
    fp = fopen(path, "wb");
    if (!fp)
        return -1;
    fwrite(img, 1, sizeof img, fp);
    return fclose(fp);
}

#endif
