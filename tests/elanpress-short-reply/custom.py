#!/usr/bin/env python3
# Replays short replies to data reads (see elanpress-recordings.py). The
# driver reads past one on the command endpoint and fails on the next one
# or when no reply follows, and fails at once on one from the image endpoint

import traceback
import sys
import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()
d = devices[0]
del devices

assert d.get_driver() == "elanpress"



def expect_short_reply(endpoint, fn, *args):
    try:
        fn(*args)
        raise AssertionError(fn.__name__ + " did not fail")
    except GLib.GError as e:
        assert e.matches(FPrint.DeviceError.quark(),
                         FPrint.DeviceError.PROTO), e
        assert "endpoint " + endpoint in e.message, e.message


d.open_sync()

for endpoint in ('0x82', '0x82', '0x83', '0x82'):
    expect_short_reply(endpoint, d.enroll_sync, FPrint.Print.new(d),
                       None, None, None)

d.close_sync()
expect_short_reply('0x83', d.open_sync)

del d
del c
