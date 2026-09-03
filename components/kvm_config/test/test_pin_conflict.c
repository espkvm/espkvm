/*
 * Host test: the rule that stops two things landing on one GPIO. No device
 * needed - it is a set comparison over a handful of small structs, which is
 * exactly why it lives on its own.
 */
#include "pin_conflict.h"

#include <stdio.h>
#include <string.h>

static int failures;
static void check(int ok, const char *what)
{
    printf("%-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        failures++;
    }
}

/** A board where GPIO 7 and 8 are the capture bus and 50 is the Ethernet PHY. */
static const char *board_holds(int gpio)
{
    switch (gpio) {
    case 7:
        return "Capture I2C SDA";
    case 8:
        return "Capture I2C SCL";
    case 50:
        return "Ethernet REFCLK";
    default:
        return NULL;
    }
}

int main(void)
{
    kvm_pin_conflict_t c;

    {
        const kvm_pin_claim_t ok[] = {
            {"atx_pwr_gpio", 46, true},
            {"atx_rst_gpio", 47, true},
            {"atx_led_gpio", 48, true},
            {"disp_sclk", 20, false},
            {"disp_mosi", 21, false},
        };
        check(!kvm_pin_conflict_find(ok, 5, board_holds, &c), "a wiring with no clash passes");
    }

    {
        /* -1 is "not assigned", and several of them at once is normal: a device
         * with no display and no buttons has every pin setting sitting there. */
        const kvm_pin_claim_t unset[] = {
            {"atx_pwr_gpio", -1, true},
            {"atx_rst_gpio", -1, true},
            {"disp_rst", -1, true},
            {"disp_bl", -1, false},
        };
        check(!kvm_pin_conflict_find(unset, 4, board_holds, &c),
              "unassigned pins do not collide with each other");
    }

    {
        const kvm_pin_claim_t dup[] = {
            {"disp_cs", 22, false},
            {"atx_led_gpio", 22, true},
        };
        check(kvm_pin_conflict_find(dup, 2, board_holds, &c) && c.gpio == 22 &&
                  strcmp(c.key, "atx_led_gpio") == 0 && strcmp(c.other, "disp_cs") == 0 &&
                  c.held_by == NULL,
              "a pin another setting holds is refused, naming both");
    }

    {
        /* The one being set now is named first whichever order they arrive in. */
        const kvm_pin_claim_t dup[] = {
            {"atx_led_gpio", 22, true},
            {"disp_cs", 22, false},
        };
        check(kvm_pin_conflict_find(dup, 2, board_holds, &c) &&
                  strcmp(c.key, "atx_led_gpio") == 0 && strcmp(c.other, "disp_cs") == 0,
              "the setting being changed is the one reported, not the stored one");
    }

    {
        const kvm_pin_claim_t fixed[] = {
            {"disp_dc", 7, true},
        };
        check(kvm_pin_conflict_find(fixed, 1, board_holds, &c) && c.gpio == 7 &&
                  strcmp(c.key, "disp_dc") == 0 && c.other == NULL &&
                  strcmp(c.held_by, "Capture I2C SDA") == 0,
              "a pin the board's own hardware holds is refused, naming it");
    }

    {
        /* Fixed hardware is the more useful answer, so it wins the race. */
        const kvm_pin_claim_t both[] = {
            {"disp_cs", 50, false},
            {"atx_pwr_gpio", 50, true},
        };
        check(kvm_pin_conflict_find(both, 2, board_holds, &c) &&
                  c.held_by && strcmp(c.held_by, "Ethernet REFCLK") == 0,
              "when a pin is both fixed and doubled, the fixed use is reported");
    }

    {
        /* A device that already had two settings on one pin - set before this
         * check existed - must still be able to change anything else. */
        const kvm_pin_claim_t stored[] = {
            {"disp_cs", 22, false},
            {"atx_led_gpio", 22, false},
            {"disp_rst", 33, true},
        };
        check(!kvm_pin_conflict_find(stored, 3, board_holds, &c),
              "a clash that was already stored does not block an unrelated change");
    }

    {
        /* But touching one half of that pair does have to be judged. */
        const kvm_pin_claim_t stored[] = {
            {"disp_cs", 22, false},
            {"atx_led_gpio", 22, true},
        };
        check(kvm_pin_conflict_find(stored, 2, board_holds, &c),
              "setting one half of an existing clash is refused");
    }

    {
        const kvm_pin_claim_t fixed[] = {
            {"disp_dc", 7, true},
        };
        check(!kvm_pin_conflict_find(fixed, 1, NULL, &c),
              "with no board map, only settings are compared");
    }

    {
        check(!kvm_pin_conflict_find(NULL, 0, board_holds, &c), "no claims is not a conflict");
    }

    printf("\n%s\n", failures ? "FAILURES" : "all good");
    return failures ? 1 : 0;
}
