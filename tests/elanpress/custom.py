#!/usr/bin/env python3
# Starting point for tests/elanpress/custom.py in libfprint, to run against
# the umockdev recording produced by tests/create-driver-test.py elanpress.

import os
import traceback
import sys
import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

# probe reads the settings once, and the threshold is the device's: a
# valid one reaches the matcher, a bad gate is logged at probe only
os.environ['FP_ELANPRESS_THRESHOLD'] = '0.5'
os.environ['FP_ELANPRESS_MIN_MEAN'] = 'x'
logged = []
for domain in ('libfprint', 'libfprint-elanpress'):
    GLib.log_set_handler(domain, GLib.LogLevelFlags.LEVEL_MESSAGE |
                         GLib.LogLevelFlags.LEVEL_DEBUG,
                         lambda dom, level, msg, *args: logged.append(msg))
ignored = 'Ignoring FP_ELANPRESS_MIN_MEAN="x", expected a number, using 150'

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()
d = devices[0]
del devices
assert logged.count(ignored) == 1, [m for m in logged if 'FP_ELANPRESS' in m]

assert d.get_driver() == "elanpress"
assert d.get_scan_type() == FPrint.ScanType.PRESS
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE)

d.open_sync()
assert d.get_nr_enroll_stages() == 20
assert logged.count(ignored) == 1
assert any(m.startswith('threshold 0.500 ') for m in logged), \
    [m for m in logged if m.startswith('threshold')]

template = FPrint.Print.new(d)

def enroll_progress(*args):
    print('enroll progress: ' + str(args))

print("enrolling")
p = d.enroll_sync(template, None, enroll_progress, None)
print("enroll done")

print("verifying")
verify_res, verify_print = d.verify_sync(p)
assert verify_res == True
print("verify done")

print("identifying")
match, print_ = d.identify_sync([FPrint.Print.deserialize(p.serialize())])
assert match is not None
print("identify done")

d.close_sync()
del d
del c
