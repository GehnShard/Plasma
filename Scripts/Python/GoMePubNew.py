from Plasma import *
from PlasmaTypes import *
from PlasmaKITypes import *
import time

class GoMePubNew(ptResponder):

    def __init__(self):
        ptResponder.__init__(self)
        self.id = -1
        self.version = 1

    def OnFirstUpdate(self):
            pass
    def OnNotify(self,state,id,events):
        pass

    def OnServerInitComplete(self):
        self.UpdateRecentVisitors()

    def UpdateRecentVisitors(self):
        try:
            AmCCR = ptCCRMgr().getLevel()
        except:
            AmCCR = 0
        if not AmCCR:
            # add player to recent players list
            deviceNode = None
            deviceInbox = None
            playerlist = None
            
            # find the device
            avault = ptAgeVault()
            adevicesfolder = avault.getAgeDevicesFolder()
            adevices = adevicesfolder.getChildNodeRefList()
            for device in adevices:
                device = device.getChild()
                devicetn = device.upcastToTextNoteNode()
                if devicetn and devicetn.getTitle() == "Visitor Imager":
                    deviceNode = devicetn
                    break

            # if we have the device then find the inbox
            if deviceNode:
                inboxes = deviceNode.getChildNodeRefList()
                for inbox in inboxes:
                    inbox = inbox.getChild()
                    inboxfolder = inbox.upcastToFolderNode()
                    if inboxfolder:
                        deviceInbox = inboxfolder
                        break

                # if we have the inbox then look for the heek score note
                if deviceInbox:
                    items = deviceInbox.getChildNodeRefList()
                    for item in items:
                        item = item.getChild()
                        itemtn = item.upcastToTextNoteNode()
                        if itemtn:
                            if itemtn.getTitle() == "Visitors, Visiteurs, Besucher":
                                playerlist = itemtn
                                break
                            elif itemtn.getTitle() == "Most Recent Visitors":
                                itemtn.setTitle("Visitors, Visiteurs, Besucher")
                                playerlist = itemtn
                                break

                    # if we have the text note then update it, otherwise create it
                    if playerlist:
                        currenttime = time.gmtime(PtGetDniTime())
                        currenttimestr = time.strftime("%m/%d/%Y %I:%M %p", currenttime)
                        playername = PtGetLocalPlayer().getPlayerName()
                        thetext = playerlist.getText()
                        if (thetext.count("\n") + 1) > 15:
                            thetext = thetext[:thetext.rfind("\n")]
                        thetext = currenttimestr + (" " * (30 - len(currenttimestr))) + playername + "\n" + thetext
                        playerlist.setText(thetext)
                        playerlist.save()
                    else:
                        currenttime = time.gmtime(PtGetDniTime())
                        currenttimestr = time.strftime("%m/%d/%Y %I:%M %p", currenttime)
                        playername = PtGetLocalPlayer().getPlayerName()
                        thetext = currenttimestr + (" " * (30 - len(currenttimestr))) + playername
                        
                        playerlist = ptVaultTextNoteNode(0)
                        playerlist.setTitle("Visitors, Visiteurs, Besucher")
                        playerlist.setText(thetext)
                        deviceInbox.addNode(playerlist)