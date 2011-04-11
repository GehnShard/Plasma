/*==LICENSE==*

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

You can contact Cyan Worlds, Inc. by email legal@cyan.com
 or by snail mail at:
      Cyan Worlds, Inc.
      14617 N Newport Hwy
      Mead, WA   99021

*==LICENSE==*/
#ifndef plNetOwnershipMsg_INC
#define plNetOwnershipMsg_INC

#include "hsStlUtils.h"
#include "pnMessage/plMessage.h"
#include "plNetMessage/plNetMessage.h"

//
// A msg sent locally when this client changes ownership of a group of objects
//
class hsResMgr;
class hsStream;
class plNetOwnershipMsg : public plMessage
{
protected:
    std::vector<plNetMsgGroupOwner::GroupInfo> fGroups; 
public:
    plNetOwnershipMsg() { SetBCastFlag(plMessage::kBCastByType); }
    
    CLASSNAME_REGISTER( plNetOwnershipMsg );
    GETINTERFACE_ANY( plNetOwnershipMsg, plMessage );
    
    // getters
    int GetNumGroups() const { return fGroups.size(); }
    plNetMsgGroupOwner::GroupInfo GetGroupInfo(int i) const { return fGroups[i]; }
    
    // setters
    void AddGroupInfo(plNetMsgGroupOwner::GroupInfo gi) { fGroups.push_back(gi); }
    void ClearGroupInfo() { fGroups.clear(); }
    
    // IO 
    void Read(hsStream* stream, hsResMgr* mgr) {    hsAssert(false, "NA: localOnly msg"); }
    void Write(hsStream* stream, hsResMgr* mgr) {   hsAssert(false, "NA: localOnly msg"); }
};

#endif      // plNetOwnershipMsg
