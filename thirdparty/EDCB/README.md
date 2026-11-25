# Vendored EDCB Sources

This directory contains the minimal EDCB source files needed to build Write_MMTS.

Source repository: https://github.com/xtne6f/EDCB
Base source commit: 302a05135786157efdd9e849fee7545132114b60

The bundled writers also include the write and finalization error fixes adapted
from the nanamitm/EDCB commits `2af82a3f` (Write_OneService) and `08b0f372`
(Write_Default).

Only the files used by `CMakeLists.txt` are copied here so Write_MMTS can be built without a separate adjacent EDCB checkout.
