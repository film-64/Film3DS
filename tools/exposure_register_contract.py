"""Read-only exposure evidence contract; no device commands or gain encoding.

Register roles: original GBATEK MT9V113 register documentation, checked against
FilmProbe readbacks. Configured multipliers do not prove ISP bypass/enable state.
The legacy composite analogue-gain formula is deliberately NOT applied.
"""
TIMING = ("0010", "0012", "0014", "300a", "300c", "3016", "3012", "3014")
CHANNELS = ("Gr", "R", "B", "Gb")
COMPOSITE = dict(zip(CHANNELS, ("3056", "305a", "3058", "305c")))
ANALOG = dict(zip(CHANNELS, ("302a", "302c", "302e", "3030")))
CORE_DIGITAL = dict(zip(CHANNELS, ("3032", "3034", "3036", "3038")))
ISP_DIGITAL = dict(zip(CHANNELS, ("32d6", "32d4", "32da", "32d8")))
LOCKS = ("a11d", "a11f")
REGISTERS = TIMING + tuple(COMPOSITE.values()) + ("3028",) + tuple(ANALOG.values()) + tuple(CORE_DIGITAL.values()) + tuple(ISP_DIGITAL.values())
REQUIRED = tuple(("reg", a) for a in REGISTERS) + tuple(("mcu", a) for a in LOCKS)
TARGETS = {"3012": "coarse", "3056": "gain_gr", "3058": "gain_b", "305a": "gain_r", "305c": "gain_gb"}
KINDS = {"seed": {0}, "normalize": {30}, "integration": {1}, "integration_long": {1},
         "analog": {30}, "digital": {30}, "restore": {1, 30}, "code_baseline": {31}, "code": {30},
         "auto_gain": {31}, "auto_digital": {31}}
REFERENCE = "https://mgba-emu.github.io/gbatek/#dsi-aptina-camera-registers-soc1-3210h-33fdh"
FRAME_SCOPES = {"calibration-legacy": (8, 12), "exposure-auto/v3": (2, 4), "exposure-auto/v4": (2, 4), "exposure-auto/v5": (2, 4)}
FRAME_SCOPES["exposure-auto/v6"]={"default":(8,4),"integration":(2,4)}
FRAME_SCOPES["exposure-field/v1"]={"default":(8,4),"integration":(2,4)}
