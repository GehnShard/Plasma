# -*- coding: utf-8 -*-
""" *==LICENSE==*

CyanWorlds.com Engine - MMOG client, server and tools
Copyright (C) 2011  Cyan Worlds, Inc.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

Additional permissions under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or
combining it with any of RAD Game Tools Bink SDK, Autodesk 3ds Max SDK,
NVIDIA PhysX SDK, Microsoft DirectX SDK, OpenSSL library, Independent
JPEG Group JPEG library, Microsoft Windows Media SDK, or Apple QuickTime SDK
(or a modified version of those libraries),
containing parts covered by the terms of the Bink SDK EULA, 3ds Max EULA,
PhysX SDK EULA, DirectX SDK EULA, OpenSSL and SSLeay licenses, IJG
JPEG Library README, Windows Media SDK EULA, or QuickTime SDK EULA, the
licensors of this Program grant you additional
permission to convey the resulting work. Corresponding Source for a
non-source form of such a combination shall include the source code for
the parts of OpenSSL and IJG JPEG Library used as well as that of the covered
work.

You can contact Cyan Worlds, Inc. by email legal@cyan.com
 or by snail mail at:
      Cyan Worlds, Inc.
      14617 N Newport Hwy
      Mead, WA   99021

 *==LICENSE==* """


from Plasma import *
from PlasmaKITypes import *
from PlasmaTypes import *
from xPsnlVaultSDL import *
import time

rgnCalStar = ptAttribActivator(1, "Region: Sparklie activator")
sdlCalStar = ptAttribString(3, "SDL: Calendar Stone SDL Value")
respCalStar = ptAttribResponder(4, "Resp: Get Sparklie")

class xCalendarStar(ptResponder, object):
    def __init__(self):
        ptResponder.__init__(self)
        self.id = 225
        self.version = 2

    def _get_have_calendar_page(self):
        psnl = xPsnlVaultSDL() # we want our YP20 status
        return bool(psnl["YeeshaPage20"][0])
    _have_calendar_page = property(_get_have_calendar_page)

    def OnServerInitComplete(self):
        # Determine the sparklie month
        if not sdlCalStar.value:
            raise RuntimeError("You forgot to specify the SDL Name!")
        self._month = int(sdlCalStar.value[-2:])

        if self.sceneobject.isLocallyOwned():
            tm = time.gmtime(PtGetServerTime()) # don't trust client time.
            time_case = tm.tm_mon == self._month

            # Now update age vault
            vault = ptAgeVault()
            if vault:
                ageSDL = vault.getAgeSDL()

                # do we have a var that matches?
                match = "CalendarSpark%02i" % self._month
                for var in ageSDL.getVarList():
                    if var.endswith(match):
                        desc = ageSDL.findVar(var)
                        if desc.getBool() != time_case: # Only update if we must
                            PtDebugPrint("xCalendarStar.OnServerInitComplete():\tSetting '%s' to '%i'" % (var, time_case))
                            desc.setBool(time_case)
                            vault.updateAgeSDL(ageSDL)
                        break
                else:
                    PtDebugPrint("xCalendarStar.OnServerInitComplete():\tNo AgeSDL value similar to '%s' found!" % match)
            else:
                PtDebugPrint("xCalendarStar.OnServerInitComplete():\tAge Vault is dead!")

    def OnNotify(self, state, id, events):
        if id == rgnCalStar.id and state:
            if PtFindAvatar(events) != PtGetLocalAvatar():
                return
            psnl = xPsnlVaultSDL()
            if not psnl[sdlCalStar.value][0]:
                if not self._have_calendar_page:
                    PtDebugPrint("xCalendarStar.OnNotify():\tYou don't have YP20, fool!", level=kWarningLevel)
                    return
                respCalStar.run(self.key)
                psnl[sdlCalStar.value] = (True,)
                PtSendKIMessageInt(kStartBookAlert, 0)
                PtDebugPrint("xCalendarStar.OnNotify():\tCongrats, you got a sparklie!", level=kWarningLevel)
                return
            else:
                PtDebugPrint("xCalendarStar.OnNotify():\tYou already have this sparklie!", level=kWarningLevel)
