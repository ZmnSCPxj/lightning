#!/usr/bin/env python3

from pyln.client import Plugin

plugin = Plugin()

@plugin.method('getdeveloperflag')
def getdeveloperflag(plugin):
    return {'developer': plugin.developer}

# The "custommsg" hook does not exist if DEVELOPER=0.
# If the unhook_if_not_developer does not work, this
# would cause a failure in DEVELOPER=0 mode.
@plugin.hook("custommsg", unhook_if_not_developer=True)
def custommsg(plugin, **kwargs):
    print("got custommsg")
    return {'result': 'continue'}

plugin.run()
