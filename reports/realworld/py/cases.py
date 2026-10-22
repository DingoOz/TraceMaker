# SPDX-License-Identifier: GPL-3.0-or-later
"""ngspice cases of the report. Driver and load models are ASSUMPTIONS (typical CMOS values, stated in the report), not measurements; the layouts
(designer's, ours) differ only in the copper they carry, so the comparison between them is what the cases are for."""

T = "kitspace_teensy-fx"
S = "Hardware_Playground_serial_gw_ATMEGA328P"


def dig(ref, pad, net, f, v=3.3, rs=40, tr=1.5e-9, duty=0.5):
    return dict(ref=ref, pad=pad, net=net, v=v, rs=rs, tr=tr, period=1.0 / f, duty=duty)


def hold(ref, pad, net, v, rs):
    return dict(ref=ref, pad=pad, net=net, v=v, rs=rs)


def rx(ref, pad, net, c=5e-12, r=None):
    d = dict(ref=ref, pad=pad, net=net, c=c)
    if r:
        d["r"] = r
    return d


def probe(ref, pad, net, label, baseline=None):
    return dict(ref=ref, pad=pad, net=net, label=label, baseline=baseline)


POTS = {"a2": ("RV7", "U4", "7"), "a5": ("RV5", "U4", "4"), "a11": ("RV12", "U4", "20")}

CASES = {
    T: {
        # 3.3 V Teensy 4.0 pad: about 40 ohm, 1.5 ns edges (assumed); codec, flash and LED driver inputs 5 pF
        "audio": dict(
            title="Teensy 4.0 to SGTL5000: I2S, MCLK and I2C beside the analog line inputs",
            layers=4,
            nets=["BCLK", "LRCLK", "I2S_OUT", "I2S_IN", "MCLK", "Net-(R3-Pad1)", "SDA", "SCL", "Net-(C14-Pad1)", "Net-(C15-Pad1)"],
            drivers=[dig("U1", "28", "BCLK", 2.8224e6), dig("U1", "9", "I2S_OUT", 1.4112e6), dig("U3", "25", "I2S_IN", 1.4112e6), dig("U1", "30", "MCLK", 11.2896e6)],
            quiet=[hold("U1", "27", "LRCLK", 0.0, 40), hold("U1", "25", "SDA", 3.3, 2200), hold("U1", "26", "SCL", 3.3, 2200)],
            loads=[rx("U3", "24", "BCLK"), rx("U3", "23", "LRCLK"), rx("U3", "26", "I2S_OUT"), rx("U1", "10", "I2S_IN"), rx("U3", "21", "Net-(R3-Pad1)"),
                   rx("U3", "27", "SDA"), rx("U3", "29", "SCL"), rx("U3", "11", "Net-(C14-Pad1)", c=10e-12, r=17.5e3), rx("U3", "12", "Net-(C15-Pad1)", c=10e-12, r=17.5e3)],
            probes=[probe("U3", "24", "BCLK", "BCLK at the codec", 0.0), probe("U3", "26", "I2S_OUT", "I2S_OUT at the codec", 0.0), probe("U3", "21", "Net-(R3-Pad1)", "MCLK at the codec", 0.0),
                    probe("U3", "23", "LRCLK", "LRCLK (quiet low) at the codec", 0.0), probe("U3", "27", "SDA", "SDA (quiet) at the codec"), probe("U3", "29", "SCL", "SCL (quiet) at the codec"),
                    probe("U3", "11", "Net-(C14-Pad1)", "LINE_IN left at the codec"), probe("U3", "12", "Net-(C15-Pad1)", "LINE_IN right at the codec")],
            stop=2.0e-6, step=0.05e-9, skip=0.3e-6,
        ),
        "spi": dict(
            title="Teensy 4.0 SPI to the LED driver and the flash, beside latch, output enable, chip select and the pot wipers",
            layers=4,
            nets=["MOSI", "SCLK", "led_latch", "led_oe", "MISO", "/mem_cs", "/pots buttons & LEDs/a2", "/pots buttons & LEDs/a5", "/pots buttons & LEDs/a11"],
            drivers=[dig("U1", "20", "SCLK", 10e6), dig("U1", "13", "MOSI", 5e6)],
            quiet=[hold("U1", "23", "led_latch", 0.0, 40), hold("U1", "11", "led_oe", 0.0, 40), hold("U1", "8", "/mem_cs", 3.3, 40), hold("U6", "2", "MISO", 0.0, 100),
                   hold("RV7", "2", "/pots buttons & LEDs/a2", 1.65, 2500), hold("RV5", "2", "/pots buttons & LEDs/a5", 1.65, 2500), hold("RV12", "2", "/pots buttons & LEDs/a11", 1.65, 2500)],
            loads=[rx("U5", "3", "SCLK"), rx("U6", "6", "SCLK"), rx("U5", "4", "MOSI"), rx("U6", "5", "MOSI"), rx("U5", "30", "led_latch"), rx("U5", "2", "led_oe"), rx("U6", "1", "/mem_cs"),
                   rx("U1", "14", "MISO"), rx("U4", "7", "/pots buttons & LEDs/a2", c=10e-12), rx("U4", "4", "/pots buttons & LEDs/a5", c=10e-12), rx("U4", "20", "/pots buttons & LEDs/a11", c=10e-12)],
            probes=[probe("U5", "30", "led_latch", "led_latch (quiet low) at the LED driver", 0.0), probe("U5", "2", "led_oe", "led_oe (quiet low) at the LED driver", 0.0), probe("U6", "1", "/mem_cs", "/mem_cs (quiet high) at the flash"),
                    probe("U1", "14", "MISO", "MISO (quiet) at the Teensy", 0.0), probe("U5", "3", "SCLK", "SCLK at the LED driver", 0.0),
                    probe("U4", "7", "/pots buttons & LEDs/a2", "pot a2 wiper at the mux"), probe("U4", "4", "/pots buttons & LEDs/a5", "pot a5 wiper at the mux"), probe("U4", "20", "/pots buttons & LEDs/a11", "pot a11 wiper at the mux")],
            stop=1.0e-6, step=0.05e-9, skip=0.2e-6,
        ),
    },
    S: {
        # ATmega328P at 5 V: about 30 ohm, 3 ns edges (assumed); SPI at 8 MHz clock, 4 Mbit/s data
        "spi_usb": dict(
            title="ATmega328P SPI (SCK, MOSI, slave select) beside the USB data pair, the 16 MHz crystal and reset",
            layers=2,
            nets=["/SCK", "/MOSI", "/RFM69W_SS", "/MISO", "/RFM69W_INT", "Net-(P1-Pad2)", "Net-(P1-Pad3)", "Net-(R7-Pad1)", "Net-(R8-Pad1)", "Net-(IC1-Pad7)", "Net-(IC1-Pad8)", "/RST"],
            drivers=[dig("IC1", "17", "/SCK", 8e6, v=5.0, rs=30, tr=3e-9), dig("IC1", "15", "/MOSI", 4e6, v=5.0, rs=30, tr=3e-9)],
            quiet=[hold("IC1", "14", "/RFM69W_SS", 5.0, 30), hold("IC1", "16", "/MISO", 0.0, 1000), hold("U4", "2", "/RFM69W_INT", 0.0, 100), hold("P1", "3", "Net-(P1-Pad3)", 3.3, 45), hold("P1", "2", "Net-(P1-Pad2)", 0.0, 45),
                   hold("X1", "3", "Net-(IC1-Pad7)", 2.5, 1000), hold("X1", "1", "Net-(IC1-Pad8)", 2.5, 1000), hold("R1", "2", "/RST", 5.0, 10000)],
            loads=[rx("CON1", "3", "/SCK", c=2e-12), rx("CON1", "4", "/MOSI", c=2e-12), rx("U1", "10", "/SCK"), rx("U1", "12", "/MOSI"), rx("U1", "13", "/RFM69W_SS"), rx("U1", "11", "/MISO"),
                   rx("IC1", "32", "/RFM69W_INT"), rx("U3", "4", "Net-(R7-Pad1)", c=5e-12), rx("U3", "5", "Net-(R8-Pad1)", c=5e-12), rx("IC1", "7", "Net-(IC1-Pad7)", c=18e-12), rx("IC1", "8", "Net-(IC1-Pad8)", c=18e-12),
                   rx("IC1", "29", "/RST", c=5e-12)],
            probes=[probe("U3", "4", "Net-(R7-Pad1)", "USB D+ at the CP2102"), probe("U3", "5", "Net-(R8-Pad1)", "USB D- at the CP2102"), probe("IC1", "7", "Net-(IC1-Pad7)", "XTAL1 at the ATmega"),
                    probe("IC1", "8", "Net-(IC1-Pad8)", "XTAL2 at the ATmega"), probe("IC1", "29", "/RST", "/RESET at the ATmega"), probe("U1", "13", "/RFM69W_SS", "slave select at the level shifter"),
                    probe("IC1", "32", "/RFM69W_INT", "radio interrupt at the ATmega"), probe("U1", "10", "/SCK", "SCK at the level shifter", 0.0)],
            stop=2.0e-6, step=0.05e-9, skip=0.3e-6,
        ),
    },
}

# DC cases: a net's copper is extracted and loaded with the currents named here (assumed worst-case, in A) from its source pin. The rail voltage is where the source pin is held.
DC = {
    T: {
        "layers": 4,
        "nets": {
            "+5V": dict(source=("U1", "33", 5.0), sinks=[("L2", "1", 0.17)]),
            "+5VA": dict(source=("L2", "2", 5.0), sinks=[(f"D{i}", "2", 0.0083) for i in range(1, 21)]),
            "+3V3": dict(source=("U1", "31", 3.3), sinks=[("U5", "32", 0.020), ("U3", "20", 0.001), ("L1", "1", 0.010), ("U6", "8", 0.015), ("U4", "24", 0.001), ("U2", "2", 0.025)]
                         + [(f"RV{i}", "1", 3.3 / 10e3) for i in range(1, 13)]),
        },
        # LED cathode nets: 8.3 mA from the LED cathode to the constant-current sink pin of the TLC5947 (R5 = 4.7 kohm sets 1.24 V / 4.7 k x 31.5 = 8.3 mA)
        "led": [(f"Net-(D{i}-Pad1)", (f"D{i}", "1"), 0.0083) for i in range(1, 21)],
    },
    S: {
        "layers": 2,
        "nets": {
            "+5V": dict(source=("L1", "1", 5.0), sinks=[("U2", "2", 0.060), ("IC1", "4", 0.004), ("IC1", "6", 0.004), ("IC1", "18", 0.004), ("U3", "7", 0.013), ("U3", "8", 0.013)]),
            "+3V3": dict(source=("U2", "3", 3.3), sinks=[("U4", "8", 0.050), ("U1", "1", 0.0005), ("U1", "8", 0.0005)]),
        },
        "led": [],
    },
}
