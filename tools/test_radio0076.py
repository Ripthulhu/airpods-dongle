#!/usr/bin/env python3
"""Bounded .0076 startup adapter checks. No device access."""
import pathlib
import subprocess
import tempfile
from check_radio0076 import check

ROOT=pathlib.Path(__file__).resolve().parents[1]
RADIO=ROOT/"src/radio0076"
TEST=ROOT/"tests/radio0076"
check()
with tempfile.TemporaryDirectory(prefix="radio0076-") as temp:
    for name in ("readback","preboot","calibration","bridge","gate","hci"):
        exe=pathlib.Path(temp)/name
        command=["gcc","-std=c11","-Wall","-Wextra","-Werror","-Wno-unused-const-variable",
                 "-g","-fsanitize=address,undefined","-fno-omit-frame-pointer","-no-pie",
                 "-DEXTERNAL_PI_TRANSPORT=1","-I"+str(TEST/"stubs"),"-I"+str(RADIO),str(TEST/(name+".c"))]
        if name in ("readback","gate"):
            command.append(str(RADIO/"readback.c"))
        subprocess.run([*command,"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
main=(ROOT/"src/main.c").read_text()
assert main.index("radio0076_begin();")<main.index("if (cyw43_arch_init())")<main.index("radio0076_start()")<main.index("uint8_t currect_slot")
app=(ROOT/"src/btstack/btstack_avdtp_source.c").read_text().split("int btstack_main(",1)[1]
assert app.index("radio0076_register_gate();")<app.index("l2cap_init();")
hci=(RADIO/"hci_gate.c").read_text()
assert "panic(\"Experimental .0076 startup failed" in hci
print(".0076 application startup ordering checked")
