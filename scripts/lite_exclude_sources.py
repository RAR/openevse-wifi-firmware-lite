#
# openevse_lite build-only source exclusion.
#
# MicroTasks ships MicroTasksInterrupt.cpp, which is NOT reachable from the lite
# EVSE-core path (MicroTasks::update / Task / Alarm / Event never touch the
# Interrupt class). It is, however, un-compilable on LibreTiny/EFM32: it declares
# a static array literally named `interrupts` (which collides with the standard
# Arduino interrupts() macro the rest of the firmware needs) and calls
# attachInterrupt(pin, fn, int) against LibreTiny's ArduinoCore-API signature
# attachInterrupt(pin, fn, PinStatus).
#
# Rather than patch the MicroTasks library (off-limits), this middleware skips
# that single translation unit at build time. It is purely build configuration —
# the library source is untouched. Scoped to the openevse_lite env via platformio.ini.
#
Import("env")

EXCLUDE_BASENAMES = ("MicroTasksInterrupt.cpp",)

def skip_excluded(node):
    # node.srcnode().get_abspath() is the source path; drop the unwanted TU.
    path = node.srcnode().get_abspath()
    if path.endswith(EXCLUDE_BASENAMES):
        return None
    return node

env.AddBuildMiddleware(skip_excluded)
